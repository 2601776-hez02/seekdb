/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SERVER_OMT
#include "config_bridge.h"
#include "ob_ai_service.h"
#include "observer/ai_service/ob_ai_service_executor.h"
#include "observer/ob_inner_sql_connection.h"
#include "sql/privilege_check/ob_ai_model_priv_util.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"
// internal SQL result sets
#include "common/mysqlclient/ob_mysql_result.h"
// ObObj / ObObjCaster / json / vector helpers used by the result validation step
#include "common/object/ob_object.h"
#include "common/json_type/ob_json_parse.h"
#include "common/storage/ob_sequence.h"
#include "share/object/ob_obj_cast.h"
#include "share/ob_lob_access_utils.h"
// schema lookups (primary key columns and the declared result type)
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
// the hidden session variable which carries the AI column result type hint
#include "share/system_variable/ob_sys_var_class_type.h"
#include "query/vector/ob_vector_index_util.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/utility/ob_smart_var.h"
#include "lib/profile/ob_trace_id.h"

using namespace oceanbase::share;
using namespace oceanbase::common;
using namespace oceanbase::sqlclient;

namespace oceanbase
{
namespace observer
{

namespace
{

// Result type hints of an AI column, appended to the final prompt by AI_COMPLETE.
// They are identical to the ones ObDDLResolver writes into the ai spec at DDL time;
// the runtime column schema does not carry the spec, so the worker recomputes the
// hint from the declared result type.
const char *const AI_TYPE_HINT_STRING =
    "Return only the value, no explanation, no quotes.";
const char *const AI_TYPE_HINT_JSON =
    "Return exactly one valid JSON value, no explanation, no markdown.";
const char *const AI_TYPE_HINT_NUMBER =
    "Return only the base-10 integer or number value, no explanation, no units.";
const char *const AI_TYPE_HINT_DATETIME =
    "Return only the date or datetime value, no explanation.";

void get_ai_column_type_hint_(const ObColumnSchemaV2 &column, ObString &type_hint)
{
  type_hint.reset();
  if (column.is_json()) {
    type_hint = ObString::make_string(AI_TYPE_HINT_JSON);
  } else if (column.is_string_type() || ob_is_text_tc(column.get_data_type())) {
    type_hint = ObString::make_string(AI_TYPE_HINT_STRING);
  } else if (ObIntTC == column.get_data_type_class()
             || ObUIntTC == column.get_data_type_class()
             || ObNumberTC == column.get_data_type_class()
             || ObFloatTC == column.get_data_type_class()
             || ObDoubleTC == column.get_data_type_class()
             || ObDecimalIntTC == column.get_data_type_class()) {
    type_hint = ObString::make_string(AI_TYPE_HINT_NUMBER);
  } else if (ObDateTimeTC == column.get_data_type_class()
             || ObDateTC == column.get_data_type_class()
             || ObTimeTC == column.get_data_type_class()
             || ObOTimestampTC == column.get_data_type_class()
             || ObMySQLDateTimeTC == column.get_data_type_class()) {
    type_hint = ObString::make_string(AI_TYPE_HINT_DATETIME);
  }
}
// __all_ai_column column names (contract 1.7).  tenant_id is intentionally not
// used: seekdb is a single-tenant server and its inner tables (see
// __all_ai_model_endpoint) carry no tenant_id column.
const char *const AI_COLUMN_TABLE_COL_TABLE_ID = "table_id";
const char *const AI_COLUMN_TABLE_COL_COLUMN_ID = "column_id";
const char *const AI_COLUMN_TABLE_COL_DATABASE_NAME = "database_name";
const char *const AI_COLUMN_TABLE_COL_TABLE_NAME = "table_name";
const char *const AI_COLUMN_TABLE_COL_COLUMN_NAME = "column_name";
const char *const AI_COLUMN_TABLE_COL_EXPRESSION = "normalized_expr";
const char *const AI_COLUMN_TABLE_COL_DEPENDENCIES = "dependencies";
const char *const AI_COLUMN_TABLE_COL_SYNC_MODE = "sync_mode";
const char *const AI_COLUMN_TABLE_COL_MAX_RETRIES = "max_retries";
const char *const AI_COLUMN_TABLE_COL_STATUS_COLUMN_ID = "state_column_id";
const char *const AI_COLUMN_TABLE_COL_SRCVER_COLUMN_ID = "request_column_id";
const char *const AI_COLUMN_TABLE_COL_RESULT_SPEC_COLUMN_ID = "result_spec_column_id";
const char *const AI_COLUMN_TABLE_COL_DEF_VERSION = "def_version";
const char *const AI_COLUMN_TABLE_COL_AI_FUNCTION = "ai_function";


// __all_ai_column_task task_status values (contract 1.7)
const int64_t AI_TASK_PENDING = 0;
const int64_t AI_TASK_RUNNING = 1;
const int64_t AI_TASK_DONE = 2;
const int64_t AI_TASK_FAILED = 3;

const int64_t AI_CLAIM_LEASE_US = 60 * 1000 * 1000;  // lease advertised in the task table
const int64_t AI_TASK_ID_SEQ_MASK = 0x3FF;
}  // namespace

ObAIColumnDef::ObAIColumnDef() { reset(); }

void ObAIColumnDef::reset()
{
  table_id_ = OB_INVALID_ID;
  column_id_ = OB_INVALID_ID;
  status_column_id_ = OB_INVALID_ID;
  srcver_column_id_ = OB_INVALID_ID;
  result_spec_column_id_ = OB_INVALID_ID;
  def_version_ = 0;
  max_retries_ = 0;
  func_type_ = 0;  // ObAIColumnFuncType::INVALID
  sync_mode_ = 0;
  obj_type_ = ObNullType;
  collation_type_ = CS_TYPE_INVALID;
  data_length_ = 0;
  vector_dim_ = 0;
  is_json_ = false;
  is_vector_ = false;
  db_name_.reset();
  table_name_.reset();
  column_name_.reset();
  status_column_name_.reset();
  srcver_column_name_.reset();
  expr_.reset();
  expr_hint_.reset();
  type_hint_.reset();
  dep_column_ids_.reuse();
}

ObAIColumnTargetTable::ObAIColumnTargetTable() { reset(); }

void ObAIColumnTargetTable::reset()
{
  table_id_ = OB_INVALID_ID;
  db_name_.reset();
  table_name_.reset();
  pk_column_names_.reuse();
  columns_.reuse();
}

int64_t ObAIColumnTargetTable::find_column_index(const uint64_t column_id) const
{
  int64_t idx = -1;
  for (int64_t i = 0; i < columns_.count() && idx < 0; ++i) {
    if (columns_.at(i).column_id_ == column_id) {
      idx = i;
    }
  }
  return idx;
}

ObAIColumnCandidate::ObAIColumnCandidate() { reset(); }

void ObAIColumnCandidate::reset()
{
  row_key_.reset();
  statuses_.reuse();
  srcvers_.reuse();
}

ObAIColumnWorkItem::ObAIColumnWorkItem() { reset(); }

void ObAIColumnWorkItem::reset()
{
  col_index_ = -1;
  retry_count_ = 0;
  srcver_ = 0;
  ok_ = false;
  exhausted_ = false;
  literal_.reset();
  err_msg_.reset();
}

ObAIColumnBackoff::ObAIColumnBackoff() { reset(); }

void ObAIColumnBackoff::reset()
{
  table_id_ = OB_INVALID_ID;
  column_id_ = OB_INVALID_ID;
  col_index_ = -1;
  retry_count_ = 0;
  next_ts_ = 0;
  srcver_ = 0;
  task_id_ = 0;
  row_key_len_ = 0;
  MEMSET(row_key_, 0, sizeof(row_key_));
  MEMSET(request_id_, 0, sizeof(request_id_));
}

ObAIColumnWorker::ObAIColumnWorker()
  : is_inited_(false),
    idle_interval_us_(DEFAULT_IDLE_INTERVAL_US),
    def_refresh_interval_us_(DEFAULT_DEF_REFRESH_INTERVAL_US),
    batch_size_(DEFAULT_BATCH_SIZE),
    last_def_refresh_ts_(0),
    task_id_counter_(0),
    recovered_(false),
    claimed_count_(0),
    published_count_(0),
    dropped_count_(0),
    failed_count_(0),
    round_count_(0),
    def_allocator_(ObMemAttr("AIColumnDef", ObCtxIds::DEFAULT_CTX_ID)),
    round_allocator_(ObMemAttr("AIColumnRound", ObCtxIds::DEFAULT_CTX_ID)),
    tables_(),
    backoffs_()
{
}

ObAIColumnWorker::~ObAIColumnWorker() { destroy(); }

int ObAIColumnWorker::init()
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ai column worker has been inited", KR(ret));
  } else if (OB_FAIL(ObServerThreadHelper::create("AIColumnWrk", 1))) {
    LOG_WARN("fail to create ai column worker thread", KR(ret));
  } else {
    is_inited_ = true;
    LOG_INFO("[AI_COLUMN_WORKER] init success", K_(idle_interval_us),
             K_(def_refresh_interval_us), K_(batch_size));
  }
  return ret;
}

int ObAIColumnWorker::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ai column worker has not been inited", KR(ret));
  } else if (OB_FAIL(ObServerThreadHelper::start())) {
    LOG_WARN("fail to start ai column worker", KR(ret));
  } else {
    LOG_INFO("[AI_COLUMN_WORKER] start success");
  }
  return ret;
}

void ObAIColumnWorker::stop()
{
  // logical_stop only flips the flag: wake the thread up so that a long idle()
  // does not delay shutdown.
  ObServerThreadHelper::stop();
  if (is_inited_) {
    ObServerThreadHelper::wakeup();
  }
  LOG_INFO("[AI_COLUMN_WORKER] stop", K_(claimed_count), K_(published_count),
           K_(dropped_count), K_(failed_count), K_(round_count));
}

void ObAIColumnWorker::wait()
{
  ObServerThreadHelper::wait();
  LOG_INFO("[AI_COLUMN_WORKER] wait success");
}

void ObAIColumnWorker::destroy()
{
  if (is_inited_) {
    ObServerThreadHelper::destroy();
    tables_.reuse();
    backoffs_.reuse();
    def_allocator_.reset();
    round_allocator_.reset();
    is_inited_ = false;
    recovered_ = false;
    LOG_INFO("[AI_COLUMN_WORKER] destroyed");
  }
}

int ObAIColumnWorker::activate()
{
  // see the declaration: task ownership is guarded by the __all_ai_column_task
  // lease columns, so a role change does not start or stop the thread.
  LOG_INFO("[AI_COLUMN_WORKER] activate");
  return OB_SUCCESS;
}

void ObAIColumnWorker::deactivate()
{
  LOG_INFO("[AI_COLUMN_WORKER] deactivate");
}

void ObAIColumnWorker::do_work()
{
  int ret = OB_SUCCESS;
  LOG_INFO("[AI_COLUMN_WORKER] worker thread start", K_(idle_interval_us));
  while (OB_SUCC(ret) && !has_set_stop()) {
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = run_round_())) {
      LOG_WARN_RET(tmp_ret, "[AI_COLUMN_WORKER] run round failed", K(tmp_ret), K_(round_count));
    }
    // sleep the fixed tick; wakeup() breaks it early (RETRY / shutdown).
    idle(idle_interval_us_);
  }
  LOG_INFO("[AI_COLUMN_WORKER] worker thread quit", KR(ret), K_(round_count),
           K_(claimed_count), K_(published_count), K_(dropped_count), K_(failed_count));
}

// ---------------------------------------------------------------------------
// worker round
// ---------------------------------------------------------------------------
int ObAIColumnWorker::run_round_()
{
  int ret = OB_SUCCESS;
  sqlclient::ObISQLConnectionGuard conn_guard;
  ObInnerSQLConnection *conn = NULL;
  const int64_t now = ObTimeUtility::current_time();
  common::ObCurTraceId::TraceId trace_id;
  trace_id.init(GCTX.self_addr());
  ObTraceIdGuard trace_id_guard(trace_id);

  round_allocator_.reuse();
  ++round_count_;

  if (OB_FAIL(ObInnerSQLConnection::create_connection_with_owned_session(false, 0, conn_guard))) {
    LOG_WARN("fail to create inner sql connection for ai column worker", KR(ret));
  } else if (OB_ISNULL(conn = static_cast<ObInnerSQLConnection *>(conn_guard.get_ptr()))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("inner sql connection is null", KR(ret));
  } else {
    // (a) discovery: refresh the AI column definitions from __all_ai_column
    if (tables_.empty() || (now - last_def_refresh_ts_) >= def_refresh_interval_us_) {
      int tmp_ret = refresh_definitions_(*conn);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN_RET(tmp_ret, "[AI_COLUMN_WORKER] refresh ai column definitions failed", K(tmp_ret));
      }
    }
    // crash recovery: rows left in RUNNING by a previous incarnation go back to PENDING
    if (!recovered_ && tables_.count() > 0) {
      int tmp_ret = recover_stuck_rows_(*conn);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN_RET(tmp_ret, "[AI_COLUMN_WORKER] crash recovery failed, retry next round", K(tmp_ret));
      } else {
        recovered_ = true;
      }
    }
    // (b)..(e) per table: candidates -> claim -> evaluate -> publish
    for (int64_t i = 0; OB_SUCC(ret) && i < tables_.count(); ++i) {
      int tmp_ret = process_table_(*conn, tables_.at(i));
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN_RET(tmp_ret, "[AI_COLUMN_WORKER] process table failed", K(tmp_ret),
                     K(tables_.at(i).table_id_));
      }
    }
    // (f) retries whose backoff expired
    if (OB_SUCC(ret)) {
      int tmp_ret = process_backoffs_(*conn);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN_RET(tmp_ret, "[AI_COLUMN_WORKER] process backoff failed", K(tmp_ret));
      }
    }
  }
  return ret;
}

int ObAIColumnWorker::recover_stuck_rows_(ObInnerSQLConnection &conn)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < tables_.count(); ++i) {
    const ObAIColumnTargetTable &table = tables_.at(i);
    ObSqlString sql;
    int64_t affected_rows = 0;
    if (table.columns_.empty()) {
      // nothing to recover for this table
    } else if (OB_FAIL(sql.assign("UPDATE "))) {
    } else if (OB_FAIL(append_qualified_table_(sql, table.db_name_, table.table_name_))) {
    } else if (OB_FAIL(sql.append(" SET "))) {
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < table.columns_.count(); ++j) {
      if (OB_FAIL(sql.append_fmt("%s`%.*s` = %d",
                                 (0 == j) ? "" : ", ",
                                 table.columns_.at(j).status_column_name_.length(),
                                 table.columns_.at(j).status_column_name_.ptr(),
                                 ObAIColumnStatus::PENDING))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(sql.append(" WHERE "))) {
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < table.columns_.count(); ++j) {
      if (OB_FAIL(sql.append_fmt("%s`%.*s` = %d",
                                 (0 == j) ? "" : " OR ",
                                 table.columns_.at(j).status_column_name_.length(),
                                 table.columns_.at(j).status_column_name_.ptr(),
                                 ObAIColumnStatus::RUNNING))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(execute_sql_(conn, sql, SHORT_STMT_TIMEOUT_US, affected_rows))) {
    } else if (affected_rows > 0) {
      LOG_INFO("[AI_COLUMN_WORKER] crash recovery reset claimed rows", K(affected_rows),
               K(table.table_id_), K(sql));
    }
  }
  if (OB_SUCC(ret)) {
    // tasks that were RUNNING but not finished by the dead incarnation
    ObSqlString sql;
    int64_t affected_rows = 0;
    if (OB_FAIL(sql.assign_fmt("UPDATE %s SET task_status = %ld WHERE task_status = %ld",
                               OB_ALL_AI_COLUMN_TASK_TNAME,
                               AI_TASK_PENDING,
                               AI_TASK_RUNNING))) {
    } else if (OB_FAIL(execute_sql_(conn, sql, SHORT_STMT_TIMEOUT_US, affected_rows))) {
    } else if (affected_rows > 0) {
      LOG_INFO("[AI_COLUMN_WORKER] crash recovery requeued tasks", K(affected_rows));
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// discovery
// ---------------------------------------------------------------------------
int ObAIColumnWorker::deep_copy_str_(ObIAllocator &allocator,
                                     const ObString &src,
                                     ObString &dst)
{
  int ret = OB_SUCCESS;
  dst.reset();
  if (src.empty()) {
    // keep it empty
  } else {
    char *buf = static_cast<char *>(allocator.alloc(src.length()));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to allocate memory for string", KR(ret), K(src));
    } else {
      MEMCPY(buf, src.ptr(), src.length());
      dst.assign_ptr(buf, src.length());
    }
  }
  return ret;
}

int ObAIColumnWorker::parse_dependencies_(const ObString &text,
                                          ObSEArray<uint64_t, 4> &ids) const
{
  int ret = OB_SUCCESS;
  ids.reuse();
  int64_t pos = 0;
  while (OB_SUCC(ret) && pos < text.length()) {
    // skip separators
    while (pos < text.length()
           && (',' == text.ptr()[pos] || ' ' == text.ptr()[pos] || ';' == text.ptr()[pos])) {
      ++pos;
    }
    if (pos >= text.length()) {
      break;
    }
    int64_t value = 0;
    const int64_t start = pos;
    while (pos < text.length() && text.ptr()[pos] >= '0' && text.ptr()[pos] <= '9') {
      value = value * 10 + (text.ptr()[pos] - '0');
      ++pos;
    }
    if (pos == start) {
      // not a number, stop parsing
      break;
    } else if (OB_FAIL(ids.push_back(static_cast<uint64_t>(value)))) {
      LOG_WARN("fail to push dependency column id", KR(ret), K(value));
    }
  }
  return ret;
}

int ObAIColumnWorker::refresh_definitions_(ObInnerSQLConnection &conn)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  last_def_refresh_ts_ = ObTimeUtility::current_time();
  tables_.reuse();
  // the cached definitions own memory in def_allocator_, they are dropped above
  def_allocator_.reuse();
  if (OB_FAIL(sql.assign_fmt(
          "SELECT %s, %s, %s, %s, %s, `%s`, `%s`, %s, %s, %s, %s, %s, %s, %s FROM %s "
          "ORDER BY %s, %s",
          AI_COLUMN_TABLE_COL_TABLE_ID,
          AI_COLUMN_TABLE_COL_COLUMN_ID,
          AI_COLUMN_TABLE_COL_DATABASE_NAME,
          AI_COLUMN_TABLE_COL_TABLE_NAME,
          AI_COLUMN_TABLE_COL_COLUMN_NAME,
          AI_COLUMN_TABLE_COL_EXPRESSION,
          AI_COLUMN_TABLE_COL_DEPENDENCIES,
          AI_COLUMN_TABLE_COL_SYNC_MODE,
          AI_COLUMN_TABLE_COL_MAX_RETRIES,
          AI_COLUMN_TABLE_COL_STATUS_COLUMN_ID,
          AI_COLUMN_TABLE_COL_SRCVER_COLUMN_ID,
          AI_COLUMN_TABLE_COL_RESULT_SPEC_COLUMN_ID,
          AI_COLUMN_TABLE_COL_DEF_VERSION,
          AI_COLUMN_TABLE_COL_AI_FUNCTION,
          OB_ALL_AI_COLUMN_TNAME,
          AI_COLUMN_TABLE_COL_TABLE_ID,
          AI_COLUMN_TABLE_COL_COLUMN_ID))) {
    LOG_WARN("fail to build ai column definition query", KR(ret));
  } else {
    SMART_VAR(common::ObISQLClient::ReadResult, res)
    {
      sqlclient::ObMySQLResult *result = NULL;
      THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + SHORT_STMT_TIMEOUT_US);
      if (OB_FAIL(conn.execute_read(sql.string(), res))) {
        LOG_WARN("fail to read __all_ai_column", KR(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result of __all_ai_column is null", KR(ret));
      } else {
        const int64_t COL_TABLE_ID = 0;
        const int64_t COL_COLUMN_ID = 1;
        const int64_t COL_DATABASE_NAME = 2;
        const int64_t COL_TABLE_NAME = 3;
        const int64_t COL_COLUMN_NAME = 4;
        const int64_t COL_EXPRESSION = 5;
        const int64_t COL_DEPENDENCIES = 6;
        const int64_t COL_SYNC_MODE = 7;
        const int64_t COL_MAX_RETRIES = 8;
        const int64_t COL_STATUS_COLUMN_ID = 9;
        const int64_t COL_SRCVER_COLUMN_ID = 10;
        const int64_t COL_RESULT_SPEC_COLUMN_ID = 11;
        const int64_t COL_DEF_VERSION = 12;
        const int64_t COL_AI_FUNCTION = 13;
        int64_t cur_table_idx = -1;
        while (OB_SUCC(ret)) {
          int tmp_ret = result->next();
          if (OB_ITER_END == tmp_ret) {
            break;
          } else if (OB_SUCCESS != tmp_ret) {
            ret = tmp_ret;
            LOG_WARN("fail to iterate __all_ai_column", KR(ret));
          } else {
            uint64_t table_id = OB_INVALID_ID;
            uint64_t column_id = OB_INVALID_ID;
            int64_t table_id_i64 = 0;
            int64_t column_id_i64 = 0;
            int64_t sync_mode = 0;
            int64_t max_retries = 0;
            int64_t status_column_id = -1;
            int64_t srcver_column_id = -1;
            int64_t result_spec_column_id = -1;
            int64_t def_version = 0;
            int64_t func_type = 0;
            ObString db_name;
            ObString table_name;
            ObString column_name;
            ObString expr;
            ObString dependencies;
            ObString ai_function;
            ObObj obj;
            if (OB_FAIL(result->get_int(COL_TABLE_ID, table_id_i64))) {
            } else if (OB_FAIL(result->get_int(COL_COLUMN_ID, column_id_i64))) {
            } else if (OB_FAIL(result->get_int(COL_SYNC_MODE, sync_mode))) {
            } else if (OB_FAIL(result->get_int(COL_MAX_RETRIES, max_retries))) {
            } else if (OB_FAIL(result->get_int(COL_STATUS_COLUMN_ID, status_column_id))) {
            } else if (OB_FAIL(result->get_int(COL_SRCVER_COLUMN_ID, srcver_column_id))) {
            } else if (OB_FAIL(result->get_int(COL_RESULT_SPEC_COLUMN_ID, result_spec_column_id))) {
            } else if (OB_FAIL(result->get_int(COL_DEF_VERSION, def_version))) {
            } else if (OB_FAIL(result->get_obj(COL_AI_FUNCTION, obj))) {
            } else if (OB_FAIL(read_column_text_(obj, ai_function))) {
            } else if (OB_FAIL(result->get_obj(COL_DATABASE_NAME, obj))) {
            } else if (OB_FAIL(read_column_text_(obj, db_name))) {
            } else if (OB_FAIL(result->get_obj(COL_TABLE_NAME, obj))) {
            } else if (OB_FAIL(read_column_text_(obj, table_name))) {
            } else if (OB_FAIL(result->get_obj(COL_COLUMN_NAME, obj))) {
            } else if (OB_FAIL(read_column_text_(obj, column_name))) {
            } else if (OB_FAIL(result->get_obj(COL_EXPRESSION, obj))) {
            } else if (OB_FAIL(read_column_text_(obj, expr))) {
            } else if (OB_FAIL(result->get_obj(COL_DEPENDENCIES, obj))) {
            } else if (OB_FAIL(read_column_text_(obj, dependencies))) {
            } else {
              // __all_ai_column.ai_function is the function NAME ('AI_COMPLETE' etc),
              // not the numeric ObAIColumnFuncType
              if (0 == ai_function.case_compare(OB_AI_COLUMN_FUNC_COMPLETE)) {
                func_type = ObAIColumnFuncType::COMPLETE;
              } else if (0 == ai_function.case_compare(OB_AI_COLUMN_FUNC_EMBED)) {
                func_type = ObAIColumnFuncType::EMBED;
              } else if (0 == ai_function.case_compare(OB_AI_COLUMN_FUNC_RERANK)) {
                func_type = ObAIColumnFuncType::RERANK;
              } else {
                func_type = ObAIColumnFuncType::INVALID;
              }
            }
            if (OB_SUCC(ret)) {
              table_id = static_cast<uint64_t>(table_id_i64);
              column_id = static_cast<uint64_t>(column_id_i64);
            }

            if (OB_FAIL(ret)) {
            } else if (OB_INVALID_ID == table_id || OB_INVALID_ID == column_id
                       || column_name.empty()) {
              LOG_WARN("[AI_COLUMN_WORKER] skip invalid ai column definition",
                       K(table_id), K(column_id), K(column_name));
            } else if (cur_table_idx < 0 || tables_.at(cur_table_idx).table_id_ != table_id) {
              ObAIColumnTargetTable new_table;
              new_table.table_id_ = table_id;
              ++cur_table_idx;
              if (OB_FAIL(deep_copy_str_(def_allocator_, db_name, new_table.db_name_))) {
              } else if (OB_FAIL(deep_copy_str_(def_allocator_, table_name, new_table.table_name_))) {
              } else if (OB_FAIL(tables_.push_back(new_table))) {
                LOG_WARN("fail to push ai column target table", KR(ret), K(table_id));
              }
            }

            if (OB_FAIL(ret)) {
            } else if (OB_UNLIKELY(cur_table_idx < 0 || cur_table_idx >= tables_.count())) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              ObAIColumnTargetTable &table = tables_.at(cur_table_idx);
              ObAIColumnDef def;
              def.table_id_ = table_id;
              def.column_id_ = column_id;
              def.status_column_id_ = static_cast<uint64_t>(status_column_id);
              def.srcver_column_id_ = static_cast<uint64_t>(srcver_column_id);
              def.result_spec_column_id_ = static_cast<uint64_t>(result_spec_column_id);
              def.def_version_ = def_version;
              def.max_retries_ = (max_retries < 0) ? 0 : max_retries;
              def.sync_mode_ = sync_mode;
              def.func_type_ = func_type;
              // normalized_expr carries the expression followed by the result
              // type hint on its own line; only the expression is SQL.
              ObString expr_sql = expr;
              ObString expr_hint;
              const char *const newline = static_cast<const char *>(
                  memrchr(expr.ptr(), '\n', static_cast<size_t>(expr.length())));
              if (OB_NOT_NULL(newline) && newline > expr.ptr()) {
                expr_sql.assign_ptr(expr.ptr(), static_cast<int32_t>(newline - expr.ptr()));
                expr_hint.assign_ptr(newline + 1,
                                     static_cast<int32_t>(expr.ptr() + expr.length() - newline - 1));
              }
              ObSqlString status_name;
              ObSqlString srcver_name;
              if (OB_FAIL(deep_copy_str_(def_allocator_, db_name, def.db_name_))) {
              } else if (OB_FAIL(deep_copy_str_(def_allocator_, table_name, def.table_name_))) {
              } else if (OB_FAIL(deep_copy_str_(def_allocator_, column_name, def.column_name_))) {
              } else if (OB_FAIL(deep_copy_str_(def_allocator_, expr_sql, def.expr_))) {
              } else if (OB_FAIL(deep_copy_str_(def_allocator_, expr_hint, def.expr_hint_))) {
              } else if (OB_FAIL(parse_dependencies_(dependencies, def.dep_column_ids_))) {
              } else if (OB_FAIL(status_name.assign_fmt("%s%.*s",
                                                        OB_AI_COLUMN_STATUS_COLUMN_PREFIX,
                                                        column_name.length(),
                                                        column_name.ptr()))) {
              } else if (OB_FAIL(srcver_name.assign_fmt("%s%.*s",
                                                        OB_AI_COLUMN_SRCVER_COLUMN_PREFIX,
                                                        column_name.length(),
                                                        column_name.ptr()))) {
              } else if (OB_FAIL(deep_copy_str_(def_allocator_, status_name.string(), def.status_column_name_))) {
              } else if (OB_FAIL(deep_copy_str_(def_allocator_, srcver_name.string(), def.srcver_column_name_))) {
              } else if (OB_FAIL(table.columns_.push_back(def))) {
                LOG_WARN("fail to push ai column definition", KR(ret), K(def));
              }
            }
          }
        }
        if (OB_SUCC(ret)) {
          // fill the primary key column names and the declared result type
          for (int64_t i = 0; OB_SUCC(ret) && i < tables_.count(); ++i) {
            ObAIColumnTargetTable &table = tables_.at(i);
            int tmp = refresh_table_schema_(table);
            if (OB_SUCCESS != tmp) {
              LOG_WARN_RET(tmp, "[AI_COLUMN_WORKER] fail to load table schema, skip table",
                           K(tmp), K(table.table_id_), K(table.db_name_), K(table.table_name_));
              table.columns_.reuse();
            }
          }
          LOG_INFO("[AI_COLUMN_WORKER] ai column definitions refreshed",
                   "table_count", tables_.count());
        }
      }
    }
  }
  return ret;
}

int ObAIColumnWorker::refresh_table_schema_(ObAIColumnTargetTable &table)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard schema_guard;
  const ObTableSchema *table_schema = NULL;
  table.pk_column_names_.reuse();
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema service is null", KR(ret));
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema_guard))) {
    LOG_WARN("fail to get runtime schema guard", KR(ret));
  } else if (OB_FAIL(schema_guard.get_table_schema(table.table_id_, table_schema))) {
    LOG_WARN("fail to get table schema", KR(ret), K(table.table_id_));
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("ai column target table does not exist", KR(ret), K(table.table_id_));
  } else {
    ObSEArray<uint64_t, 4> pk_column_ids;
    const share::schema::ObDatabaseSchema *db_schema = NULL;
    // The DDL hooks that maintain __all_ai_column only know the database id, so its
    // database_name column is empty while every target statement of this worker is
    // `db`.`table`: refresh the name from the schema guard instead of the metadata.
    if (OB_FAIL(schema_guard.get_database_schema(table_schema->get_database_id(), db_schema))) {
      LOG_WARN("fail to get database schema", KR(ret), K(table.table_id_));
    } else if (OB_ISNULL(db_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("database schema of the ai column target table is null", KR(ret),
               K(table.table_id_));
    } else if (OB_FAIL(deep_copy_str_(def_allocator_, db_schema->get_database_name_str(),
                                      table.db_name_))) {
      LOG_WARN("fail to copy the database name of the ai column target table", KR(ret),
               K(table.table_id_));
    } else if (OB_FAIL(table_schema->get_rowkey_column_ids(pk_column_ids))) {
      LOG_WARN("fail to get rowkey column ids", KR(ret), K(table.table_id_));
    } else if (pk_column_ids.empty()) {
      // an AI column table always has a primary key
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai column target table has no primary key", KR(ret), K(table.table_id_));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < pk_column_ids.count(); ++i) {
      const ObColumnSchemaV2 *pk_column = table_schema->get_column_schema(pk_column_ids.at(i));
      ObString pk_name;
      if (OB_ISNULL(pk_column)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("primary key column not found", KR(ret), K(pk_column_ids.at(i)));
      } else if (OB_FAIL(deep_copy_str_(def_allocator_, pk_column->get_column_name_str(), pk_name))) {
      } else if (OB_FAIL(table.pk_column_names_.push_back(pk_name))) {
        LOG_WARN("fail to push primary key column name", KR(ret));
      }
    }
    // snapshot the declared result type of every AI column; a definition whose
    // column vanished from the schema (drop column / drop table) is discarded.
    for (int64_t i = table.columns_.count() - 1; OB_SUCC(ret) && i >= 0; --i) {
      ObAIColumnDef &def = table.columns_.at(i);
      const ObColumnSchemaV2 *ai_column = table_schema->get_column_schema(def.column_id_);
      if (OB_ISNULL(ai_column) || !ai_column->is_ai_column()) {
        // the column disappeared or lost its AI attribute between the metadata
        // read and the schema read: stop generating for it.
        LOG_INFO("[AI_COLUMN_WORKER] drop stale ai column definition",
                 K(def.table_id_), K(def.column_id_), KP(ai_column));
        if (OB_FAIL(table.columns_.remove(i))) {
          LOG_WARN("fail to remove stale ai column definition", KR(ret), K(i));
        }
      } else {
        def.obj_type_ = ai_column->get_data_type();
        def.collation_type_ = ai_column->get_collation_type();
        def.data_length_ = ai_column->get_data_length();
        def.is_json_ = ai_column->is_json();
        def.is_vector_ = false;
        def.vector_dim_ = 0;
        const ObIArray<ObString> &extend_type_info = ai_column->get_extended_type_info();
        if (extend_type_info.count() == 1 && extend_type_info.at(0).prefix_match("VECTOR(")) {
          int64_t dim = 0;
          int tmp_ret = ObVectorIndexUtil::get_vector_dim_from_extend_type_info(extend_type_info, dim);
          if (OB_SUCCESS == tmp_ret && dim > 0) {
            def.is_vector_ = true;
            def.vector_dim_ = dim;
          } else {
            LOG_WARN_RET(tmp_ret, "[AI_COLUMN_WORKER] fail to parse vector dimension",
                         K(tmp_ret), K(extend_type_info.at(0)));
          }
        }
        // the result type hint belongs to the column definition (design 7.1); it is
        // empty for a VECTOR result column. The ai spec is only a catalog level copy
        // (it is not part of the runtime column schema), so the hint is recomputed
        // from the declared result type when the spec does not carry it.
        if (def.is_vector_) {
          def.type_hint_.reset();
        } else if (!def.expr_hint_.empty()) {
          // the hint persisted in normalized_expr is authoritative
          if (OB_FAIL(deep_copy_str_(def_allocator_, def.expr_hint_, def.type_hint_))) {
            LOG_WARN("fail to copy the stored ai column type hint", KR(ret), K(def.column_id_));
          }
        } else if (!ai_column->get_ai_spec().type_hint_.empty()) {
          if (OB_FAIL(deep_copy_str_(def_allocator_,
                                     ai_column->get_ai_spec().type_hint_,
                                     def.type_hint_))) {
            LOG_WARN("fail to copy ai column type hint", KR(ret), K(def.column_id_));
          }
        } else {
          ObString hint;
          get_ai_column_type_hint_(*ai_column, hint);
          if (OB_FAIL(deep_copy_str_(def_allocator_, hint, def.type_hint_))) {
            LOG_WARN("fail to copy the computed ai column type hint", KR(ret), K(def.column_id_));
          }
        }
      }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// claim
// ---------------------------------------------------------------------------
int ObAIColumnWorker::append_ident_(ObSqlString &sql, const ObString &name) const
{
  int ret = OB_SUCCESS;
  // column names come from the catalog; embedded back quotes cannot appear there.
  if (OB_FAIL(sql.append("`"))) {
  } else if (OB_FAIL(sql.append(name))) {
  } else if (OB_FAIL(sql.append("`"))) {
  }
  return ret;
}

int ObAIColumnWorker::append_qualified_table_(ObSqlString &sql,
                                              const ObString &db_name,
                                              const ObString &table_name) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(append_ident_(sql, db_name))) {
  } else if (OB_FAIL(sql.append("."))) {
  } else if (OB_FAIL(append_ident_(sql, table_name))) {
  }
  return ret;
}

int ObAIColumnWorker::append_text_literal_(const ObString &value, ObSqlString &sql) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const char c = value.ptr()[i];
    switch (c) {
      case '\'': ret = sql.append("''"); break;
      case '\\': ret = sql.append("\\\\"); break;
      case '\0': ret = sql.append("\\0"); break;
      case '\n': ret = sql.append("\\n"); break;
      case '\r': ret = sql.append("\\r"); break;
      case '\032': ret = sql.append("\\Z"); break;
      default: ret = sql.append(&c, 1); break;
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("'");
  }
  return ret;
}

int ObAIColumnWorker::append_obj_literal_(const ObObj &obj,
                                          const ObObjPrintParams &print_params,
                                          ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  int64_t buf_len = 128;
  int64_t pos = 0;
  char *buf = static_cast<char *>(round_allocator_.alloc(buf_len));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to allocate literal buffer", KR(ret));
  } else if (OB_FAIL(obj.print_sql_literal(buf, buf_len, pos, print_params))) {
    LOG_WARN("fail to print sql literal", KR(ret), K(obj));
  } else if (OB_FAIL(sql.append(buf, pos))) {
  }
  return ret;
}

int ObAIColumnWorker::collect_candidates_(ObInnerSQLConnection &conn,
                                          const ObAIColumnTargetTable &table,
                                          ObIArray<ObAIColumnCandidate> &rows)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObSqlString select_list;
  bool first_item = true;
  if (table.columns_.empty() || table.pk_column_names_.empty()) {
    // nothing to do for this table
  } else if (OB_FAIL(sql.assign("SELECT "))) {
  }
  // primary key columns, then one (status, srcver) pair per AI column
  for (int64_t i = 0; OB_SUCC(ret) && i < table.pk_column_names_.count(); ++i) {
    if (!first_item && OB_FAIL(select_list.append(", "))) {
    } else if (OB_FAIL(append_ident_(select_list, table.pk_column_names_.at(i)))) {
    } else {
      first_item = false;
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table.columns_.count(); ++i) {
    const ObAIColumnDef &def = table.columns_.at(i);
    if (!first_item && OB_FAIL(select_list.append(", "))) {
    } else if (OB_FAIL(append_ident_(select_list, def.status_column_name_))) {
    } else if (OB_FAIL(select_list.append(", "))) {
    } else if (OB_FAIL(append_ident_(select_list, def.srcver_column_name_))) {
    } else {
      first_item = false;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sql.append(select_list.string()))) {
  } else if (OB_FAIL(sql.append(" FROM "))) {
  } else if (OB_FAIL(append_qualified_table_(sql, table.db_name_, table.table_name_))) {
  } else if (OB_FAIL(sql.append(" WHERE "))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table.columns_.count(); ++i) {
    if (OB_FAIL(sql.append_fmt("%s`%.*s` = %d",
                               (0 == i) ? "(" : " OR ",
                               table.columns_.at(i).status_column_name_.length(),
                               table.columns_.at(i).status_column_name_.ptr(),
                               ObAIColumnStatus::PENDING))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sql.append(")"))) {
  } else if (OB_FAIL(sql.append_fmt(" LIMIT %ld", batch_size_))) {
  } else {
    SMART_VAR(common::ObISQLClient::ReadResult, res)
    {
      sqlclient::ObMySQLResult *result = NULL;
      THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + SHORT_STMT_TIMEOUT_US);
      if (OB_FAIL(conn.execute_read(sql.string(), res))) {
        LOG_WARN("fail to scan AI column candidate rows", KR(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("candidate result is null", KR(ret));
      } else {
        const int64_t pk_cnt = table.pk_column_names_.count();
        const int64_t column_cnt = table.columns_.count();
        const ObObjPrintParams print_params = conn.get_session().create_obj_print_params();
        while (OB_SUCC(ret)) {
          int tmp_ret = result->next();
          if (OB_ITER_END == tmp_ret) {
            break;
          } else if (OB_SUCCESS != tmp_ret) {
            ret = tmp_ret;
            LOG_WARN("fail to iterate AI column candidates", KR(ret));
          } else {
            ObAIColumnCandidate candidate;
            ObSqlString row_key;
            int64_t col_idx = 0;
            for (int64_t i = 0; OB_SUCC(ret) && i < pk_cnt; ++i) {
              ObObj pk_value;
              if (i > 0 && OB_FAIL(row_key.append(" AND "))) {
              } else if (OB_FAIL(append_ident_(row_key, table.pk_column_names_.at(i)))) {
              } else if (OB_FAIL(row_key.append(" = "))) {
              } else if (OB_FAIL(result->get_obj(col_idx++, pk_value))) {
              } else if (OB_FAIL(append_obj_literal_(pk_value, print_params, row_key))) {
              }
            }
            for (int64_t i = 0; OB_SUCC(ret) && i < column_cnt; ++i) {
              int64_t status = 0;
              int64_t srcver = 0;
              if (OB_FAIL(result->get_int(col_idx++, status))) {
                LOG_WARN("fail to read ai column status", KR(ret), K(i));
              } else if (OB_FAIL(candidate.statuses_.push_back(status))) {
              } else if (OB_FAIL(result->get_int(col_idx++, srcver))) {
                LOG_WARN("fail to read ai column srcver", KR(ret), K(i));
              } else if (OB_FAIL(candidate.srcvers_.push_back(srcver))) {
              }
            }
            if (OB_FAIL(ret)) {
            } else if (OB_FAIL(deep_copy_str_(round_allocator_, row_key.string(), candidate.row_key_))) {
            } else if (OB_FAIL(rows.push_back(candidate))) {
              LOG_WARN("fail to push candidate row", KR(ret));
            }
          }
        }
      }
    }
  }
  return ret;
}

bool ObAIColumnWorker::is_backing_off_(const uint64_t table_id,
                                       const uint64_t column_id,
                                       const ObString &row_key,
                                       const int64_t srcver) const
{
  bool found = false;
  for (int64_t i = 0; i < backoffs_.count() && !found; ++i) {
    const ObAIColumnBackoff &backoff = backoffs_.at(i);
    if (backoff.table_id_ == table_id
        && backoff.column_id_ == column_id
        && backoff.srcver_ == srcver
        && backoff.row_key_len_ == row_key.length()
        && 0 == MEMCMP(backoff.row_key_, row_key.ptr(), row_key.length())) {
      found = true;
    }
  }
  return found;
}

int ObAIColumnWorker::build_claim_sql_(const ObAIColumnTargetTable &table,
                                       const ObAIColumnCandidate &row,
                                       const ObIArray<int64_t> &group,
                                       ObSqlString &sql) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(group.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(sql.assign("UPDATE "))) {
  } else if (OB_FAIL(append_qualified_table_(sql, table.db_name_, table.table_name_))) {
  } else if (OB_FAIL(sql.append(" SET "))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
    const ObAIColumnDef &def = table.columns_.at(group.at(i));
    if (OB_FAIL(sql.append_fmt("%s`%.*s` = %d",
                               (0 == i) ? "" : ", ",
                               def.status_column_name_.length(),
                               def.status_column_name_.ptr(),
                               ObAIColumnStatus::RUNNING))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sql.append(" WHERE "))) {
  } else if (OB_FAIL(sql.append(row.row_key_))) {
  }
  // the "status = PENDING AND srcver = <observed>" predicate is the mutex: only
  // one claimer can move the row out of PENDING for this exact source version.
  for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
    const int64_t col_idx = group.at(i);
    const ObAIColumnDef &def = table.columns_.at(col_idx);
    if (OB_FAIL(sql.append_fmt(" AND `%.*s` = %d",
                               def.status_column_name_.length(),
                               def.status_column_name_.ptr(),
                               ObAIColumnStatus::PENDING))) {
    } else if (OB_FAIL(sql.append_fmt(" AND `%.*s` = %ld",
                                      def.srcver_column_name_.length(),
                                      def.srcver_column_name_.ptr(),
                                      row.srcvers_.at(col_idx)))) {
    }
  }
  return ret;
}

int ObAIColumnWorker::claim_row_(ObInnerSQLConnection &conn,
                                 const ObAIColumnTargetTable &table,
                                 const ObAIColumnCandidate &row,
                                 ObIArray<ObAIColumnWorkItem> &items,
                                 int64_t &task_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObSEArray<int64_t, 8> group;
  bool claimed = false;
  task_id = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < table.columns_.count(); ++i) {
    const ObAIColumnDef &def = table.columns_.at(i);
    if (OB_UNLIKELY(i >= row.statuses_.count() || i >= row.srcvers_.count())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("candidate row and definition are out of sync", KR(ret), K(i));
    } else if (ObAIColumnStatus::PENDING != row.statuses_.at(i)) {
      // this AI column of the row is not waiting for generation
    } else if (is_backing_off_(table.table_id_, def.column_id_, row.row_key_,
                               row.srcvers_.at(i))) {
      // already claimed by this worker, waiting for its backoff to expire
    } else if (OB_FAIL(group.push_back(i))) {
      LOG_WARN("fail to record claimed column", KR(ret), K(i));
    }
  }
  if (OB_FAIL(ret) || group.empty()) {
    // nothing claimable
  } else {
    int64_t affected_rows = 0;
    if (OB_FAIL(build_claim_sql_(table, row, group, sql))) {
    } else if (OB_FAIL(execute_sql_(conn, sql, SHORT_STMT_TIMEOUT_US, affected_rows))) {
    } else if (1 != affected_rows) {
      // row updated / deleted / definition changed between the scan and the claim
      LOG_INFO("[AI_COLUMN_WORKER] claim lost, drop candidate",
               K(affected_rows), K(table.table_id_), K(row.row_key_));
    } else {
      claimed = true;
    }
  }
  if (OB_SUCC(ret) && claimed) {
    char request_id[ObAIColumnBackoff::MAX_REQUEST_ID_LEN];
    gen_request_id_(request_id, sizeof(request_id));
    for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
      ObAIColumnWorkItem item;
      item.col_index_ = group.at(i);
      // the attempts already consumed by this row/column/source version are kept in
      // the backoff list, so a retry never restarts the retry budget
      item.srcver_ = row.srcvers_.at(group.at(i));
      item.retry_count_ = -1;
      {
        const ObAIColumnDef &def = table.columns_.at(item.col_index_);
        for (int64_t j = 0; j < backoffs_.count() && item.retry_count_ < 0; ++j) {
          const ObAIColumnBackoff &backoff = backoffs_.at(j);
          if (backoff.table_id_ == table.table_id_
              && backoff.column_id_ == def.column_id_
              && backoff.srcver_ == item.srcver_
              && backoff.row_key_len_ == row.row_key_.length()
              && 0 == MEMCMP(backoff.row_key_, row.row_key_.ptr(), row.row_key_.length())) {
            item.retry_count_ = backoff.retry_count_;
          }
        }
      }
      if (item.retry_count_ < 0) {
        item.retry_count_ = 0;
      }
      if (OB_FAIL(items.push_back(item))) {
        LOG_WARN("fail to push claimed column", KR(ret), K(item.col_index_));
      }
    }
    if (OB_SUCC(ret)) {
      ++claimed_count_;
      LOG_INFO("[AI_COLUMN_WORKER] claimed row", K(table.table_id_), K(row.row_key_),
               "column_count", items.count(), "request_id", request_id,
               "srcver", items.at(0).srcver_, "retry", items.at(0).retry_count_);
      int tmp_ret = insert_task_row_(conn, table, row, items, request_id, task_id);
      if (OB_SUCCESS != tmp_ret) {
        // the task table is observability only: never block generation on it
        LOG_WARN_RET(tmp_ret, "[AI_COLUMN_WORKER] fail to write task row", K(tmp_ret),
                     K(table.table_id_), K(row.row_key_));
        task_id = 0;
      }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// evaluate
// ---------------------------------------------------------------------------
int ObAIColumnWorker::read_column_text_(const ObObj &obj, ObString &text)
{
  int ret = OB_SUCCESS;
  text.reset();
  if (obj.is_null()) {
    // an empty text means "no value" for the validation step
  } else if (obj.is_string_type() || obj.is_lob_storage() || obj.is_json()) {
    // longtext values may carry a lob header; the helper resolves both in-row
    // and out-of-row storage.
    if (OB_FAIL(common::lob_helper::read_real_string_data(&round_allocator_, obj, text, NULL))) {
      LOG_WARN("fail to read text value", KR(ret), K(obj));
    }
  } else {
    ObObj in_obj = obj;
    ObObj out_obj;
    ObCastCtx cast_ctx(&round_allocator_, NULL, CM_NONE, CS_TYPE_UTF8MB4_BIN);
    if (OB_FAIL(ObObjCaster::to_type(ObVarcharType, CS_TYPE_UTF8MB4_BIN,
                                     cast_ctx, in_obj, out_obj))) {
      LOG_WARN("fail to cast value to varchar", KR(ret), K(obj));
    } else if (OB_FAIL(deep_copy_str_(round_allocator_, out_obj.get_string(), text))) {
      LOG_WARN("fail to copy value text", KR(ret));
    }
  }
  return ret;
}

int ObAIColumnWorker::check_value_(ObAIColumnWorkItem &item,
                                   const ObAIColumnDef &def,
                                   const ObString &value)
{
  int ret = OB_SUCCESS;
  item.ok_ = false;
  item.err_msg_.reset();
  const ObObjType obj_type = static_cast<ObObjType>(def.obj_type_);
  const ObCollationType cs_type = static_cast<ObCollationType>(def.collation_type_);
  if (value.empty()) {
    item.err_msg_ = ObString::make_string("empty or NULL model output");
  } else if (def.is_vector_) {
    // Reuse the embedding path conversion: it validates the element count against
    // the declared VECTOR(N) dimension and that every element is a finite number.
    ObString binary_vector;
    if (def.vector_dim_ <= 0) {
      item.err_msg_ = ObString::make_string("vector dimension is unknown");
    } else if (OB_FAIL(ObVectorIndexUtil::cast_vector_array_str_to_float_array_binary(
                   round_allocator_, value, def.vector_dim_, binary_vector))) {
      item.err_msg_ = ObString::make_string("output is not a finite VECTOR(N) value");
      LOG_WARN("ai column vector output is invalid", K(def.column_id_), K(def.vector_dim_));
      ret = OB_SUCCESS;
    } else {
      item.ok_ = true;
    }
  } else if (def.is_json_) {
    if (OB_FAIL(ObJsonParser::check_json_syntax(value, &round_allocator_))) {
      item.err_msg_ = ObString::make_string("output is not a valid JSON document");
      LOG_WARN("ai column json output is invalid", K(def.column_id_));
      ret = OB_SUCCESS;
    } else {
      item.ok_ = true;
    }
  } else if (ob_is_string_type(obj_type)) {
    const size_t char_len = ObCharset::strlen_char(cs_type, value.ptr(), value.length());
    if (def.data_length_ > 0 && static_cast<int64_t>(char_len) > def.data_length_) {
      item.err_msg_ = ObString::make_string("output is longer than the declared length");
      LOG_WARN("ai column text output is too long", K(def.column_id_),
               K(char_len), K(def.data_length_));
    } else {
      item.ok_ = true;
    }
  } else {
    // numeric / temporal / bit / enum: reuse the engine cast rules, they are the
    // authoritative definition of "convertible to the declared type".
    ObObj in_obj;
    ObObj out_obj;
    ObCastCtx cast_ctx(&round_allocator_, NULL, CM_NONE, cs_type);
    in_obj.set_varchar(value);
    in_obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (OB_FAIL(ObObjCaster::to_type(obj_type, cs_type, cast_ctx, in_obj, out_obj))) {
      item.err_msg_ = ObString::make_string("output is not convertible to the declared type");
      LOG_WARN("ai column output cast failed", K(def.column_id_), K(obj_type));
      ret = OB_SUCCESS;
    } else {
      item.ok_ = true;
    }
  }
  return ret;
}

int ObAIColumnWorker::render_value_(ObAIColumnWorkItem &item,
                                    const ObAIColumnDef &def,
                                    const ObString &value)
{
  int ret = OB_SUCCESS;
  ObSqlString literal;
  UNUSED(def);
  if (OB_FAIL(append_text_literal_(value, literal))) {
  } else if (OB_FAIL(deep_copy_str_(round_allocator_, literal.string(), item.literal_))) {
    LOG_WARN("fail to copy rendered literal", KR(ret));
  }
  return ret;
}

int ObAIColumnWorker::set_type_hint_(ObInnerSQLConnection &conn, const ObString &type_hint)
{
  int ret = OB_SUCCESS;
  // The result type hint of one AI column is handed to the model through the hidden
  // session variable _ai_column_type_hint of the internal connection; ObExprAIComplete
  // appends it to the prompt.  The value is passed as a value, never as SQL text, so no
  // quoting/escaping is involved (and the name based overload, which strips wrapping
  // quotes, is deliberately not used).  An empty hint clears it.
  if (OB_FAIL(conn.get_session().update_sys_variable(share::SYS_VAR__AI_COLUMN_TYPE_HINT,
                                                     type_hint))) {
    LOG_WARN("fail to set ai column type hint", KR(ret), K(type_hint));
  }
  return ret;
}

int ObAIColumnWorker::evaluate_group_(ObInnerSQLConnection &conn,
                                      const ObAIColumnTargetTable &table,
                                      const ObString &row_key,
                                      ObIArray<ObAIColumnWorkItem> &items,
                                      bool &row_gone)
{
  int ret = OB_SUCCESS;
  row_gone = false;
  if (OB_UNLIKELY(items.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("no ai column to evaluate", KR(ret));
  }
  // One statement per AI column: the type hint is a property of a single column and it
  // reaches the model through a session variable, so a statement shared by a whole group
  // would give every column the same hint.  Grouped visibility is not affected: the group
  // is still published by the single transaction of publish_row_.
  for (int64_t i = 0; OB_SUCC(ret) && !row_gone && i < items.count(); ++i) {
    ObAIColumnWorkItem &item = items.at(i);
    int tmp_ret = evaluate_statement_(conn, table, row_key, item, row_gone);
    if (OB_SUCCESS != tmp_ret) {
      // one broken expression only fails its own column, the other columns of the
      // group keep their own result
      item.ok_ = false;
      item.literal_.reset();
      item.err_msg_ = ObString::make_string("expression evaluation failed");
      LOG_WARN_RET(tmp_ret, "[AI_COLUMN_WORKER] single column evaluation failed",
                   K(tmp_ret), K(table.table_id_), K(item.col_index_));
    }
  }
  return ret;
}

int ObAIColumnWorker::evaluate_statement_(ObInnerSQLConnection &conn,
                                          const ObAIColumnTargetTable &table,
                                          const ObString &row_key,
                                          ObAIColumnWorkItem &item,
                                          bool &row_gone)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  const ObAIColumnDef *def = NULL;
  row_gone = false;
  if (OB_UNLIKELY(item.col_index_ < 0 || item.col_index_ >= table.columns_.count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid ai column index", KR(ret), K(item.col_index_),
             K(table.columns_.count()));
  } else if (OB_FALSE_IT(def = &table.columns_.at(item.col_index_))) {
  } else if (OB_FAIL(set_type_hint_(conn, def->type_hint_))) {
  } else if (OB_FAIL(sql.append_fmt("SELECT (%.*s) AS `ai_col_%ld` FROM ",
                                    def->expr_.length(),
                                    def->expr_.ptr(),
                                    item.col_index_))) {
  } else if (OB_FAIL(append_qualified_table_(sql, table.db_name_, table.table_name_))) {
  } else if (OB_FAIL(sql.append(" WHERE "))) {
  } else if (OB_FAIL(sql.append(row_key))) {
  } else {
    SMART_VAR(common::ObISQLClient::ReadResult, res)
    {
      sqlclient::ObMySQLResult *result = NULL;
      THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + EVAL_STMT_TIMEOUT_US);
      if (OB_FAIL(conn.execute_read(sql.string(), res))) {
        LOG_WARN("fail to evaluate ai column expression", KR(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("evaluation result is null", KR(ret));
      } else {
        int tmp_ret = result->next();
        if (OB_ITER_END == tmp_ret) {
          // the row disappeared after the claim: nothing to publish
          row_gone = true;
          LOG_INFO("[AI_COLUMN_WORKER] target row disappeared, drop work",
                   K(table.table_id_), K(row_key));
        } else if (OB_SUCCESS != tmp_ret) {
          ret = tmp_ret;
          LOG_WARN("fail to fetch evaluated ai column value", KR(ret), K(sql));
        } else {
          ObObj value_obj;
          ObString text;
          if (OB_FAIL(result->get_obj(static_cast<int64_t>(0), value_obj))) {
            LOG_WARN("fail to read evaluated value", KR(ret));
          } else if (OB_FAIL(read_column_text_(value_obj, text))) {
            LOG_WARN("fail to read evaluated value text", KR(ret));
          } else if (OB_FAIL(check_value_(item, *def, text))) {
            LOG_WARN("fail to validate evaluated value", KR(ret), K(item.col_index_));
          } else if (item.ok_ && OB_FAIL(render_value_(item, *def, text))) {
            LOG_WARN("fail to render evaluated value", KR(ret), K(item.col_index_));
          }
        }
      }
    }
  }
  // the hint belongs to this statement only: never leave it behind for the next one.
  // A failure here must not mask the evaluation result.
  int tmp_ret = set_type_hint_(conn, ObString());
  if (OB_SUCCESS != tmp_ret) {
    LOG_WARN_RET(tmp_ret, "[AI_COLUMN_WORKER] fail to clear ai column type hint", K(tmp_ret));
  }
  return ret;
}

int ObAIColumnWorker::evaluate_one_(ObInnerSQLConnection &conn,
                                    const ObAIColumnTargetTable &table,
                                    const ObString &row_key,
                                    ObAIColumnWorkItem &item,
                                    bool &row_gone)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObAIColumnWorkItem, 1> items;
  if (OB_UNLIKELY(item.col_index_ < 0 || item.col_index_ >= table.columns_.count())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(items.push_back(item))) {
  } else if (OB_FAIL(evaluate_group_(conn, table, row_key, items, row_gone))) {
  } else {
    item = items.at(0);
  }
  return ret;
}

int ObAIColumnWorker::evaluate_row_(ObInnerSQLConnection &conn,
                                    const ObAIColumnTargetTable &table,
                                    const ObAIColumnCandidate &row,
                                    ObIArray<ObAIColumnWorkItem> &items,
                                    bool &row_gone)
{
  // evaluate_group_ already isolates every column of the group with its own statement,
  // so a broken expression can no longer fail the other columns of the same row.
  return evaluate_group_(conn, table, row.row_key_, items, row_gone);
}

// ---------------------------------------------------------------------------
// publish
// ---------------------------------------------------------------------------
int ObAIColumnWorker::publish_row_(ObInnerSQLConnection &conn,
                                   const ObAIColumnTargetTable &table,
                                   const ObAIColumnCandidate &row,
                                   ObIArray<ObAIColumnWorkItem> &items,
                                   const int64_t task_id)
{
  int ret = OB_SUCCESS;
  const int64_t now = ObTimeUtility::current_time();
  ObSqlString sql;
  bool should_publish = false;
  bool dropped = false;
  int64_t ok_cnt = 0;
  int64_t exhausted_cnt = 0;
  int64_t retry_cnt = 0;
  int64_t max_retry_count = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
    ObAIColumnWorkItem &item = items.at(i);
    const ObAIColumnDef &def = table.columns_.at(item.col_index_);
    if (item.ok_) {
      ++ok_cnt;
      should_publish = true;
    } else {
      // a failed attempt consumes one retry
      ++item.retry_count_;
      if (item.retry_count_ > def.max_retries_) {
        item.exhausted_ = true;
        ++exhausted_cnt;
        should_publish = true;
      } else {
        ++retry_cnt;
      }
      if (item.retry_count_ > max_retry_count) {
        max_retry_count = item.retry_count_;
      }
      // a retryable failure puts the column back to PENDING in the same statement:
      // a row must never be left in RUNNING, otherwise a lost in-memory retry state
      // (restart, definition refresh) strands it forever.
      should_publish = true;
      LOG_WARN("[AI_COLUMN_WORKER] ai column generation failed",
               K(table.table_id_), K(row.row_key_), K(def.column_id_),
               K(item.retry_count_), K(def.max_retries_), K(item.err_msg_));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(conn.start_transaction())) {
    LOG_WARN("fail to start publish transaction", KR(ret));
  } else {
    if (should_publish) {
      // one UPDATE publishes every value of the group together with the READY
      // (and terminal FAILED) state; srcver is the version fence. Assigning the
      // hidden status column in the same statement is what identifies the engine's
      // own writer to the resolver (see ObDMLResolver::check_basic_column_ai()).
      if (OB_FAIL(sql.assign("UPDATE "))) {
      } else if (OB_FAIL(append_qualified_table_(sql, table.db_name_, table.table_name_))) {
      } else if (OB_FAIL(sql.append(" SET "))) {
      }
      bool first = true;
      for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
        const ObAIColumnWorkItem &item = items.at(i);
        if (!item.ok_) {
          // the failed columns are only written when they reached a terminal state
          continue;
        }
        const ObAIColumnDef &def = table.columns_.at(item.col_index_);
        if (OB_FAIL(sql.append_fmt("%s`%.*s` = ",
                                   first ? "" : ", ",
                                   def.column_name_.length(),
                                   def.column_name_.ptr()))) {
        } else if (OB_FAIL(sql.append(item.literal_))) {
        } else if (OB_FAIL(sql.append_fmt(", `%.*s` = %d",
                                          def.status_column_name_.length(),
                                          def.status_column_name_.ptr(),
                                          ObAIColumnStatus::READY))) {
        } else {
          first = false;
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
        const ObAIColumnWorkItem &item = items.at(i);
        if (!item.exhausted_) {
          continue;
        }
        const ObAIColumnDef &def = table.columns_.at(item.col_index_);
        if (OB_FAIL(sql.append_fmt("%s`%.*s` = %d",
                                   first ? "" : ", ",
                                   def.status_column_name_.length(),
                                   def.status_column_name_.ptr(),
                                   ObAIColumnStatus::FAILED))) {
        } else {
          first = false;
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
        const ObAIColumnWorkItem &item = items.at(i);
        if (item.ok_ || item.exhausted_) {
          continue;
        }
        // a retryable failure: keep the (stale) value and the source version, but
        // make the row claimable again so that a lost retry state can not strand it
        const ObAIColumnDef &def = table.columns_.at(item.col_index_);
        if (OB_FAIL(sql.append_fmt("%s`%.*s` = %d",
                                   first ? "" : ", ",
                                   def.status_column_name_.length(),
                                   def.status_column_name_.ptr(),
                                   ObAIColumnStatus::PENDING))) {
        } else {
          first = false;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(sql.append(" WHERE "))) {
      } else if (OB_FAIL(sql.append(row.row_key_))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
        const ObAIColumnWorkItem &item = items.at(i);
        const ObAIColumnDef &def = table.columns_.at(item.col_index_);
        if (OB_FAIL(sql.append_fmt(" AND `%.*s` = %ld",
                                   def.srcver_column_name_.length(),
                                   def.srcver_column_name_.ptr(),
                                   item.srcver_))) {
        }
      }
      int64_t affected_rows = 0;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(execute_sql_(conn, sql, SHORT_STMT_TIMEOUT_US, affected_rows))) {
        LOG_WARN("fail to publish ai column values", KR(ret), K(sql));
      } else if (1 != affected_rows) {
        // the row changed, was deleted, or the definition moved on: the result is
        // stale, drop it silently.
        dropped = true;
        ++dropped_count_;
        LOG_INFO("[AI_COLUMN_WORKER] stale publish dropped", K(affected_rows),
                 K(table.table_id_), K(row.row_key_), K(sql));
      } else {
        published_count_ += ok_cnt;
        failed_count_ += exhausted_cnt;
        LOG_INFO("[AI_COLUMN_WORKER] published ai column values",
                 K(table.table_id_), K(row.row_key_), K(ok_cnt), K(exhausted_cnt));
      }
    }
    if (OB_SUCC(ret) && task_id > 0) {
      // a stale publish is not a failure: the request is simply superseded
      const int64_t task_status = dropped ? AI_TASK_DONE
                                  : ((exhausted_cnt > 0) ? AI_TASK_FAILED
                                  : ((retry_cnt > 0) ? AI_TASK_RUNNING
                                                     : AI_TASK_DONE));
      // a finished (or dropped) row leaves no task row behind: the queue entry the
      // dml of the row wrote is dropped together with the claim row of this worker.
      int tmp_ret = (AI_TASK_RUNNING == task_status)
                        ? update_task_row_(conn, task_id, task_status, max_retry_count, ObString())
                        : delete_row_tasks_(conn, table.table_id_, row.row_key_);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN_RET(tmp_ret, "[AI_COLUMN_WORKER] fail to update task row",
                     K(tmp_ret), K(task_id), K(task_status));
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(conn.commit())) {
        LOG_WARN("fail to commit publish transaction", KR(ret));
        int tmp_ret = conn.rollback();
        LOG_WARN_RET(tmp_ret, "fail to rollback publish transaction", K(tmp_ret));
      }
    } else {
      int tmp_ret = conn.rollback();
      LOG_WARN_RET(tmp_ret, "fail to rollback publish transaction", K(tmp_ret));
    }
  }
  // backoff bookkeeping: retryable failures stay claimed (status RUNNING) and are
  // re-evaluated from the in-memory list; a restart resets them to PENDING.
  if (OB_SUCC(ret)) {
    if (dropped) {
      int tmp_ret = purge_backoffs_(table.table_id_, row.row_key_);
      LOG_WARN_RET(tmp_ret, "fail to purge backoff entries", K(tmp_ret));
    } else {
      int tmp_ret = requeue_or_fail_(table, row, items, task_id, NULL, now);
      LOG_WARN_RET(tmp_ret, "fail to requeue failed ai columns", K(tmp_ret));
    }
  }
  return ret;
}

int ObAIColumnWorker::purge_backoffs_(const uint64_t table_id, const ObString &row_key)
{
  int ret = OB_SUCCESS;
  for (int64_t i = backoffs_.count() - 1; OB_SUCC(ret) && i >= 0; --i) {
    const ObAIColumnBackoff &backoff = backoffs_.at(i);
    if (backoff.table_id_ == table_id
        && backoff.row_key_len_ == row_key.length()
        && 0 == MEMCMP(backoff.row_key_, row_key.ptr(), row_key.length())) {
      if (OB_FAIL(backoffs_.remove(i))) {
        LOG_WARN("fail to remove backoff entry", KR(ret), K(i));
      }
    }
  }
  return ret;
}

int ObAIColumnWorker::requeue_or_fail_(const ObAIColumnTargetTable &table,
                                       const ObAIColumnCandidate &row,
                                       ObIArray<ObAIColumnWorkItem> &items,
                                       const int64_t task_id,
                                       const char *request_id,
                                       const int64_t now)
{
  int ret = OB_SUCCESS;
  UNUSED(request_id);
  for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
    const ObAIColumnWorkItem &item = items.at(i);
    const ObAIColumnDef &def = table.columns_.at(item.col_index_);
    if (item.ok_ || item.exhausted_) {
      // published or terminal: no retry state to keep for this column
    } else {
      ObAIColumnBackoff backoff;
      backoff.table_id_ = table.table_id_;
      backoff.column_id_ = def.column_id_;
      backoff.col_index_ = item.col_index_;
      backoff.retry_count_ = item.retry_count_;
      backoff.srcver_ = item.srcver_;
      backoff.task_id_ = task_id;
      backoff.next_ts_ = next_backoff_ts_(item.retry_count_);
      backoff.row_key_len_ = static_cast<int32_t>(row.row_key_.length());
      if (backoff.row_key_len_ >= ObAIColumnBackoff::MAX_ROW_KEY_LEN) {
        LOG_WARN("[AI_COLUMN_WORKER] row key too long for the retry list, drop retry state",
                 K(backoff.row_key_len_), K(table.table_id_));
      } else {
        MEMCPY(backoff.row_key_, row.row_key_.ptr(), row.row_key_.length());
        // refresh an existing entry (same table/column/row) instead of duplicating it
        bool replaced = false;
        for (int64_t j = 0; !replaced && j < backoffs_.count(); ++j) {
          ObAIColumnBackoff &exist = backoffs_.at(j);
          if (exist.table_id_ == backoff.table_id_
              && exist.column_id_ == backoff.column_id_
              && exist.row_key_len_ == backoff.row_key_len_
              && 0 == MEMCMP(exist.row_key_, backoff.row_key_, backoff.row_key_len_)) {
            exist = backoff;
            replaced = true;
          }
        }
        if (!replaced && OB_FAIL(backoffs_.push_back(backoff))) {
          LOG_WARN("fail to push backoff entry", KR(ret));
        }
      }
    }
  }
  LOG_INFO("[AI_COLUMN_WORKER] requeued failed ai columns", K(table.table_id_),
           K(row.row_key_), "backoff_count", backoffs_.count(), K(now),
           "srcvers", items.at(0).srcver_, "retries", items.at(0).retry_count_);
  return ret;
}

int64_t ObAIColumnWorker::next_backoff_ts_(const int64_t retry_count) const
{
  int64_t delay_us = BACKOFF_BASE_US;
  for (int64_t i = 0; i < retry_count && delay_us < BACKOFF_MAX_US; ++i) {
    delay_us *= 2;
  }
  if (delay_us > BACKOFF_MAX_US) {
    delay_us = BACKOFF_MAX_US;
  }
  return ObTimeUtility::current_time() + delay_us;
}

int ObAIColumnWorker::process_table_(ObInnerSQLConnection &conn,
                                     const ObAIColumnTargetTable &table)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObAIColumnCandidate, DEFAULT_BATCH_SIZE> rows;
  if (table.columns_.empty() || table.pk_column_names_.empty()) {
    // nothing to generate for this table
  } else if (OB_FAIL(collect_candidates_(conn, table, rows))) {
    LOG_WARN("fail to collect ai column candidates", KR(ret), K(table.table_id_));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
    ObSEArray<ObAIColumnWorkItem, 8> items;
    int64_t task_id = 0;
    int tmp_ret = claim_row_(conn, table, rows.at(i), items, task_id);
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN_RET(tmp_ret, "[AI_COLUMN_WORKER] claim failed", K(tmp_ret),
                   K(table.table_id_), K(rows.at(i).row_key_));
    } else if (items.empty()) {
      // the row was claimed by somebody else or is waiting for its backoff
    } else {
      bool row_gone = false;
      int eval_ret = evaluate_row_(conn, table, rows.at(i), items, row_gone);
      if (row_gone) {
        int purge_ret = purge_backoffs_(table.table_id_, rows.at(i).row_key_);
        LOG_WARN_RET(purge_ret, "fail to purge backoff entries", K(purge_ret));
      } else {
        if (OB_SUCCESS != eval_ret) {
          // an evaluation level failure is a failure of every column of the group:
          // route it through publish so the retry budget is honoured instead of
          // leaving the row stuck in RUNNING.
          LOG_WARN_RET(eval_ret, "[AI_COLUMN_WORKER] evaluation failed", K(eval_ret),
                       K(table.table_id_), K(rows.at(i).row_key_));
          for (int64_t j = 0; j < items.count(); ++j) {
            items.at(j).ok_ = false;
            items.at(j).literal_.reset();
            if (items.at(j).err_msg_.empty()) {
              items.at(j).err_msg_ = ObString::make_string("expression evaluation failed");
            }
          }
        }
        int pub_ret = publish_row_(conn, table, rows.at(i), items, task_id);
        if (OB_SUCCESS != pub_ret) {
          LOG_WARN_RET(pub_ret, "[AI_COLUMN_WORKER] publish failed", K(pub_ret),
                       K(table.table_id_), K(rows.at(i).row_key_));
        }
      }
    }
  }
  return ret;
}

int ObAIColumnWorker::process_backoffs_(ObInnerSQLConnection &conn)
{
  int ret = OB_SUCCESS;
  const int64_t now = ObTimeUtility::current_time();
  for (int64_t i = backoffs_.count() - 1; OB_SUCC(ret) && i >= 0; --i) {
    ObAIColumnBackoff &backoff = backoffs_.at(i);
    if (backoff.next_ts_ > now) {
      // still in backoff
    } else {
      const int64_t table_idx = find_table_index_(backoff.table_id_);
      if (table_idx < 0) {
        // table definition disappeared
        if (OB_FAIL(backoffs_.remove(i))) {
          LOG_WARN("fail to remove stale backoff entry", KR(ret), K(i));
        }
      } else {
        ObAIColumnTargetTable &table = tables_.at(table_idx);
        const int64_t col_idx = table.find_column_index(backoff.column_id_);
        if (col_idx < 0) {
          if (OB_FAIL(backoffs_.remove(i))) {
            LOG_WARN("fail to remove stale backoff entry", KR(ret), K(i));
          }
        } else {
          ObString row_key;
          row_key.assign_ptr(backoff.row_key_, backoff.row_key_len_);
          ObAIColumnWorkItem item;
          item.col_index_ = col_idx;
          item.retry_count_ = backoff.retry_count_;
          item.srcver_ = backoff.srcver_;
          bool row_gone = false;
          int eval_ret = evaluate_one_(conn, table, row_key, item, row_gone);
          if (OB_SUCCESS != eval_ret) {
            item.ok_ = false;
            item.err_msg_ = ObString::make_string("expression evaluation failed");
            LOG_WARN_RET(eval_ret, "[AI_COLUMN_WORKER] retry evaluation failed", K(eval_ret),
                         K(backoff.table_id_), K(backoff.column_id_));
          }
          if (row_gone) {
            if (OB_FAIL(backoffs_.remove(i))) {
              LOG_WARN("fail to remove dead backoff entry", KR(ret), K(i));
            }
          } else if (item.ok_) {
            int pub_ret = publish_retry_(conn, table, backoff, item);
            if (OB_SUCCESS != pub_ret) {
              LOG_WARN_RET(pub_ret, "[AI_COLUMN_WORKER] retry publish failed", K(pub_ret),
                           K(backoff.table_id_), K(backoff.column_id_));
            } else if (OB_FAIL(backoffs_.remove(i))) {
              LOG_WARN("fail to remove finished backoff entry", KR(ret), K(i));
            }
          } else {
            const ObAIColumnDef &def = table.columns_.at(col_idx);
            ++item.retry_count_;
            if (item.retry_count_ > def.max_retries_) {
              int fail_ret = fail_retry_(conn, table, backoff);
              if (OB_SUCCESS != fail_ret) {
                LOG_WARN_RET(fail_ret, "[AI_COLUMN_WORKER] fail to mark column FAILED",
                             K(fail_ret), K(backoff.table_id_), K(backoff.column_id_));
              }
              if (OB_FAIL(backoffs_.remove(i))) {
                LOG_WARN("fail to remove exhausted backoff entry", KR(ret), K(i));
              }
            } else {
              backoff.retry_count_ = item.retry_count_;
              backoff.next_ts_ = next_backoff_ts_(item.retry_count_);
              int upd_ret = update_task_row_(conn, backoff.task_id_, AI_TASK_RUNNING,
                                             item.retry_count_, item.err_msg_);
              LOG_WARN_RET(upd_ret, "fail to update retry task row", K(upd_ret));
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObAIColumnWorker::publish_retry_(ObInnerSQLConnection &conn,
                                     const ObAIColumnTargetTable &table,
                                     const ObAIColumnBackoff &backoff,
                                     const ObAIColumnWorkItem &item)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  const ObAIColumnDef &def = table.columns_.at(item.col_index_);
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.assign("UPDATE "))) {
  } else if (OB_FAIL(append_qualified_table_(sql, table.db_name_, table.table_name_))) {
  } else if (OB_FAIL(sql.append(" SET "))) {
  } else if (OB_FAIL(append_ident_(sql, def.column_name_))) {
  } else if (OB_FAIL(sql.append(" = "))) {
  } else if (OB_FAIL(sql.append(item.literal_))) {
  } else if (OB_FAIL(sql.append_fmt(", `%.*s` = %d",
                                    def.status_column_name_.length(),
                                    def.status_column_name_.ptr(),
                                    ObAIColumnStatus::READY))) {
  } else if (OB_FAIL(sql.append(" WHERE "))) {
  } else if (OB_FAIL(sql.append(ObString(backoff.row_key_len_, backoff.row_key_)))) {
  } else if (OB_FAIL(sql.append_fmt(" AND `%.*s` = %ld",
                                    def.srcver_column_name_.length(),
                                    def.srcver_column_name_.ptr(),
                                    backoff.srcver_))) {
  } else if (OB_FAIL(execute_sql_(conn, sql, SHORT_STMT_TIMEOUT_US, affected_rows))) {
    LOG_WARN("fail to publish retried ai column", KR(ret), K(sql));
  } else if (1 == affected_rows) {
    ++published_count_;
    LOG_INFO("[AI_COLUMN_WORKER] retried ai column published",
             K(table.table_id_), K(def.column_id_), K(backoff.retry_count_));
    int tmp_ret = delete_row_tasks_(conn, table.table_id_, ObString(backoff.row_key_len_, backoff.row_key_));
    LOG_WARN_RET(tmp_ret, "fail to delete finished task row", K(tmp_ret));
  } else {
    ++dropped_count_;
    LOG_INFO("[AI_COLUMN_WORKER] stale retried publish dropped", K(affected_rows),
             K(table.table_id_), K(def.column_id_));
    int tmp_ret = delete_row_tasks_(conn, table.table_id_, ObString(backoff.row_key_len_, backoff.row_key_));
    LOG_WARN_RET(tmp_ret, "fail to delete dropped task row", K(tmp_ret));
  }
  return ret;
}

int ObAIColumnWorker::fail_retry_(ObInnerSQLConnection &conn,
                                  const ObAIColumnTargetTable &table,
                                  const ObAIColumnBackoff &backoff)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  const int64_t col_idx = table.find_column_index(backoff.column_id_);
  int64_t affected_rows = 0;
  if (OB_UNLIKELY(col_idx < 0)) {
    ret = OB_ENTRY_NOT_EXIST;
  } else {
    const ObAIColumnDef &def = table.columns_.at(col_idx);
    if (OB_FAIL(sql.assign("UPDATE "))) {
    } else if (OB_FAIL(append_qualified_table_(sql, table.db_name_, table.table_name_))) {
    } else if (OB_FAIL(sql.append_fmt(" SET `%.*s` = %d",
                                      def.status_column_name_.length(),
                                      def.status_column_name_.ptr(),
                                      ObAIColumnStatus::FAILED))) {
    } else if (OB_FAIL(sql.append(" WHERE "))) {
    } else if (OB_FAIL(sql.append(ObString(backoff.row_key_len_, backoff.row_key_)))) {
    } else if (OB_FAIL(sql.append_fmt(" AND `%.*s` = %ld",
                                      def.srcver_column_name_.length(),
                                      def.srcver_column_name_.ptr(),
                                      backoff.srcver_))) {
    } else if (OB_FAIL(execute_sql_(conn, sql, SHORT_STMT_TIMEOUT_US, affected_rows))) {
      LOG_WARN("fail to mark ai column FAILED", KR(ret), K(sql));
    } else {
      ++failed_count_;
      LOG_INFO("[AI_COLUMN_WORKER] ai column reached terminal FAILED",
               K(table.table_id_), K(def.column_id_), K(affected_rows));
      int tmp_ret = delete_row_tasks_(conn, table.table_id_,
                                      ObString(backoff.row_key_len_, backoff.row_key_));
      LOG_WARN_RET(tmp_ret, "fail to delete failed task row", K(tmp_ret));
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// __all_ai_column_task
// ---------------------------------------------------------------------------
int ObAIColumnWorker::next_task_id_(int64_t &task_id)
{
  int ret = OB_SUCCESS;
  const int64_t now = ObTimeUtility::current_time();
  ++task_id_counter_;
  task_id = (now << 10) | (task_id_counter_ & AI_TASK_ID_SEQ_MASK);
  return ret;
}

void ObAIColumnWorker::gen_request_id_(char *buf, const int64_t buf_len) const
{
  // There is no shared UUID helper in this tree (ObExprUuid needs a sql-engine
  // include and the sql operator init), so build an RFC-4122 shaped v1 id from
  // the wall clock and ObSequence: 36 chars, unique per claimed group.
  const int64_t now = ObTimeUtility::current_time();
  const int64_t seq = ObSequence::inc_and_get_max_seq_no();
  const uint32_t time_low = static_cast<uint32_t>(static_cast<uint64_t>(now) & 0xffffffffULL);
  const uint16_t time_mid = static_cast<uint16_t>((static_cast<uint64_t>(now) >> 32) & 0xffff);
  const uint16_t time_hi = static_cast<uint16_t>(
      ((static_cast<uint64_t>(now) >> 48) & 0x0fff) | 0x1000);
  const uint16_t clock = static_cast<uint16_t>(((static_cast<uint64_t>(seq) >> 4) & 0x3fff) | 0x8000);
  const uint64_t node = static_cast<uint64_t>(seq) * 2654435761ULL;
  if (NULL != buf && buf_len > 0) {
    snprintf(buf, static_cast<size_t>(buf_len),
             "%08x-%04x-%04x-%04x-%012llx",
             time_low, time_mid, time_hi, clock,
             static_cast<unsigned long long>(node & 0xffffffffffffULL));
  }
}

int ObAIColumnWorker::insert_task_row_(ObInnerSQLConnection &conn,
                                       const ObAIColumnTargetTable &table,
                                       const ObAIColumnCandidate &row,
                                       const ObIArray<ObAIColumnWorkItem> &items,
                                       const char *request_id,
                                       int64_t &task_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObSqlString column_ids;
  ObSqlString srcvers;
  ObSqlString def_versions;
  ObSqlString snapshot;
  int64_t new_task_id = 0;
  task_id = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
    const ObAIColumnWorkItem &item = items.at(i);
    const ObAIColumnDef &def = table.columns_.at(item.col_index_);
    if (OB_FAIL(column_ids.append_fmt("%s%lu", (0 == i) ? "" : ",",
                                      static_cast<unsigned long>(def.column_id_)))) {
    } else if (OB_FAIL(srcvers.append_fmt("%s%ld", (0 == i) ? "" : ",", item.srcver_))) {
    } else if (OB_FAIL(def_versions.append_fmt("%s%ld", (0 == i) ? "" : ",",
                                               def.def_version_))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(next_task_id_(new_task_id))) {
  } else if (OB_FAIL(snapshot.assign_fmt("table_id=%lu;srcvers=%.*s;def_versions=%.*s;"
                                        "request_id=%s",
                                        static_cast<unsigned long>(table.table_id_),
                                        static_cast<int>(srcvers.length()), srcvers.ptr(),
                                        static_cast<int>(def_versions.length()),
                                        def_versions.ptr(),
                                        NULL == request_id ? "" : request_id))) {
  } else if (OB_FAIL(sql.assign_fmt(
                 "INSERT INTO %s (task_id, table_id, row_key, column_ids, srcvers, "
                 "def_versions, request_id, task_status, retry_count, lease_owner, "
                 "lease_expire_ts, input_snapshot) VALUES (%ld, %lu, ",
                 OB_ALL_AI_COLUMN_TASK_TNAME,
                 new_task_id,
                 static_cast<unsigned long>(table.table_id_)))) {
  } else if (OB_FAIL(append_text_literal_(row.row_key_, sql))) {
  } else if (OB_FAIL(sql.append(", "))) {
  } else if (OB_FAIL(append_text_literal_(column_ids.string(), sql))) {
  } else if (OB_FAIL(sql.append(", "))) {
  } else if (OB_FAIL(append_text_literal_(srcvers.string(), sql))) {
  } else if (OB_FAIL(sql.append(", "))) {
  } else if (OB_FAIL(append_text_literal_(def_versions.string(), sql))) {
  } else if (OB_FAIL(sql.append(", "))) {
  } else if (OB_FAIL(append_text_literal_(ObString::make_string(
                         NULL == request_id ? "" : request_id), sql))) {
  } else if (OB_FAIL(sql.append_fmt(", %ld, %ld, ", AI_TASK_RUNNING, 0L))) {
  } else if (OB_FAIL(append_text_literal_(ObString::make_string("ai_column_worker"), sql))) {
  } else if (OB_FAIL(sql.append_fmt(", %ld, ", ObTimeUtility::current_time() + AI_CLAIM_LEASE_US))) {
  } else if (OB_FAIL(append_text_literal_(snapshot.string(), sql))) {
  } else if (OB_FAIL(sql.append(")"))) {
  } else {
    int64_t affected_rows = 0;
    if (OB_FAIL(execute_sql_(conn, sql, SHORT_STMT_TIMEOUT_US, affected_rows))) {
      LOG_WARN("fail to insert ai column task row", KR(ret), K(sql));
    } else if (1 != affected_rows) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected affected rows of task insert", KR(ret), K(affected_rows));
    } else {
      task_id = new_task_id;
    }
  }
  return ret;
}

int ObAIColumnWorker::update_task_row_(ObInnerSQLConnection &conn,
                                       const int64_t task_id,
                                       const int64_t task_status,
                                       const int64_t retry_count,
                                       const ObString &err_msg)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  UNUSED(err_msg);
  if (task_id <= 0) {
    // no task row (the insert failed earlier), nothing to keep in sync
  } else if (OB_FAIL(sql.assign_fmt("UPDATE %s SET task_status = %ld, retry_count = %ld "
                                    "WHERE task_id = %ld",
                                    OB_ALL_AI_COLUMN_TASK_TNAME,
                                    task_status,
                                    retry_count,
                                    task_id))) {
  } else if (OB_FAIL(execute_sql_(conn, sql, SHORT_STMT_TIMEOUT_US, affected_rows))) {
    LOG_WARN("fail to update ai column task row", KR(ret), K(sql));
  } else if (0 == affected_rows) {
    LOG_INFO("[AI_COLUMN_WORKER] task row disappeared", K(task_id), K(task_status));
  }
  return ret;
}

// A task row only lives while the work is outstanding: once the row is published
// (or finally failed) it is deleted, so a finished table has no task rows left.
int ObAIColumnWorker::delete_task_row_(ObInnerSQLConnection &conn, const int64_t task_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (task_id <= 0) {
    // no task row, nothing to delete
  } else if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE task_id = %ld",
                                    OB_ALL_AI_COLUMN_TASK_TNAME, task_id))) {
  } else if (OB_FAIL(execute_sql_(conn, sql, SHORT_STMT_TIMEOUT_US, affected_rows))) {
    LOG_WARN("fail to delete ai column task row", KR(ret), K(sql));
  }
  return ret;
}

// The task row of a row is written by the dml of the statement that invalidated the
// row (visible in its transaction), the worker only claims the row. Both rows have
// the same (table_id, row_key) and the queue entry is dropped whenever the work of
// the row is finished, whichever task row it belongs to.
int ObAIColumnWorker::delete_row_tasks_(ObInnerSQLConnection &conn,
                                       const uint64_t table_id,
                                       const ObString &row_key)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %lu AND row_key = ",
                             OB_ALL_AI_COLUMN_TASK_TNAME,
                             static_cast<unsigned long>(table_id)))) {
  } else if (OB_FAIL(append_text_literal_(row_key, sql))) {
  } else if (OB_FAIL(execute_sql_(conn, sql, SHORT_STMT_TIMEOUT_US, affected_rows))) {
    LOG_WARN("fail to delete the ai column task rows of a row", KR(ret), K(sql));
  }
  return ret;
}

// ---------------------------------------------------------------------------
// misc helpers
// ---------------------------------------------------------------------------
int64_t ObAIColumnWorker::find_table_index_(const uint64_t table_id) const
{
  int64_t idx = -1;
  for (int64_t i = 0; i < tables_.count() && idx < 0; ++i) {
    if (tables_.at(i).table_id_ == table_id) {
      idx = i;
    }
  }
  return idx;
}

int ObAIColumnWorker::execute_sql_(ObInnerSQLConnection &conn,
                                   const ObSqlString &sql,
                                   const int64_t timeout_us,
                                   int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  affected_rows = 0;
  THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + timeout_us);
  if (OB_UNLIKELY(sql.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty inner sql", KR(ret));
  } else if (OB_FAIL(conn.execute_write(sql.string(), affected_rows))) {
    LOG_WARN("inner sql write failed", KR(ret), K(sql));
  }
  return ret;
}

}  // namespace observer
}  // namespace oceanbase

namespace oceanbase
{
namespace omt
{

ObAiService::ObAiService()
  : is_inited_(false),
    ai_column_worker_()
{
}

ObAiService::~ObAiService()
{
  destroy();
}

int ObAiService::server_module_init(ObAiService* &ai_service)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ai_service)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ai_service->init())) {
  }
  return ret;
}

int ObAiService::init()
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(ai_column_worker_.init())) {
    LOG_WARN("fail to init ai column worker", KR(ret));
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObAiService::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(ai_column_worker_.start())) {
    LOG_WARN("fail to start ai column worker", KR(ret));
  }
  return ret;
}

void ObAiService::stop()
{
  ai_column_worker_.stop();
}

void ObAiService::wait()
{
  ai_column_worker_.wait();
}

void ObAiService::destroy()
{
  ai_column_worker_.destroy();
  is_inited_ = false;
}

int ObAiService::get_ai_service_guard(ObAiServiceGuard &ai_service_guard)
{
  // TODO: implement after cache support
  int ret = OB_SUCCESS;
  UNUSED(ai_service_guard);
  return ret;
}

ObAiServiceGuard::ObAiServiceGuard()
  : local_allocator_(ObMemAttr("AiServiceGuard", ObCtxIds::DEFAULT_CTX_ID))
{
}

ObAiServiceGuard::~ObAiServiceGuard()
{
}

int ObAiServiceGuard::check_access_privilege()
{
  int ret = OB_SUCCESS;
  bool has_priv = false;

  sql::ObSQLSessionInfo *session = nullptr;
  if (OB_ISNULL(session = THIS_WORKER.get_session())) {
    // system background task thread do not have session, skip check
  } else {
    ObArenaAllocator tmp_allocator;
    share::schema::ObSchemaGetterGuard *schema_guard = session->get_cur_exec_ctx()->get_sql_ctx()->schema_guard_;
    if (OB_ISNULL(schema_guard)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      sql::ObAIServiceEndpointPrivUtil priv_util(*schema_guard);
      share::schema::ObSessionPrivInfo session_priv;
      uint64_t user_id = session->get_priv_user_id();
      if (user_id == OB_INVALID_ID) {
        user_id = OB_SYS_USER_ID;
      }
      if (OB_FAIL(schema_guard->get_session_priv_info(
                                                    user_id,
                                                    session->get_database_name(),
                                                    session_priv))) {
      } else if (OB_FAIL(priv_util.check_access_ai_model_priv(tmp_allocator, session_priv, has_priv))) {
      } else if (!has_priv) {
        ret = OB_ERR_NO_PRIVILEGE;
        LOG_WARN("no privilege for ai model access", K(ret));
        LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "access ai model");
      }
    }
  }

  return ret;
}

int ObAiServiceGuard::get_ai_endpoint(const common::ObString &name, const share::ObAiModelEndpointInfo *&endpoint_info)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (OB_FAIL(check_access_privilege())) {
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint(local_allocator_, name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

int ObAiServiceGuard::get_ai_endpoint_by_ai_model_name(const common::ObString &ai_model_name, const share::ObAiModelEndpointInfo *&endpoint_info, bool need_check)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (need_check && OB_FAIL(check_access_privilege())) {
    LOG_WARN("failed to check access privilege", K(ret));
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint_by_ai_model_name(local_allocator_, ai_model_name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

} // namespace omt
} // namespace oceanbase
