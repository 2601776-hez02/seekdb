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

#ifndef OCEANBASE_OBSERVER_OMT_OB_AI_SERVICE_H_
#define OCEANBASE_OBSERVER_OMT_OB_AI_SERVICE_H_

#include "share/ai_service/ob_ai_service_struct.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/lock/ob_tc_rwlock.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_sql_string.h"
#include "lib/allocator/page_arena.h"
#include "lib/utility/ob_print_utils.h"
#include "sql/privilege_check/ob_ai_model_priv_util.h"
#include "share/ob_server_struct.h"
#include "lib/worker.h"
// rootserver::ObServerThreadHelper: the background-thread skeleton shared with
// ObDBMSSchedService (create/start/stop/wait/destroy/do_work/idle/wakeup).
#include "rootserver/ob_server_thread_helper.h"

namespace oceanbase
{
namespace common
{
class ObObj;
}
namespace observer
{

class ObInnerSQLConnection;

// AI column state machine, see notes/contract.md 1.2.
struct ObAIColumnStatus
{
  enum TYPE
  {
    INVALID = 0,  // not generated / invalidated without a pending job
    PENDING = 1,  // durable work marker written by DML / RETRY
    READY   = 2,  // value published and valid
    FAILED  = 3,  // retries exhausted, terminal state
    RUNNING = 4   // claimed by a worker (crash recovery resets it to PENDING)
  };
};

// One AI column definition discovered from __all_ai_column.  The result-column
// type information is a snapshot taken from the data table schema, so the worker
// never has to keep a schema guard (and therefore a schema version) alive.
struct ObAIColumnDef
{
  ObAIColumnDef();
  void reset();
  TO_STRING_KV(K_(table_id), K_(column_id), K_(def_version), K_(max_retries),
               K_(func_type), K_(obj_type), K_(vector_dim), K_(is_json),
               K_(db_name), K_(table_name), K_(column_name), K_(status_column_name),
               K_(srcver_column_name), K_(expr), K_(type_hint));
  uint64_t table_id_;
  uint64_t column_id_;
  uint64_t status_column_id_;
  uint64_t srcver_column_id_;
  uint64_t result_spec_column_id_;
  int64_t def_version_;
  int64_t max_retries_;
  int64_t func_type_;
  int64_t sync_mode_;
  // declared result type (snapshot of the AI column schema)
  int64_t obj_type_;        // ObObjType of the AI column
  int64_t collation_type_;  // ObCollationType of the AI column
  int64_t data_length_;     // ObColumnSchemaV2::get_data_length()
  int64_t vector_dim_;      // > 0 only for a dense VECTOR(N) column
  bool is_json_;
  bool is_vector_;
  // names / expression text, owned by ObAIColumnWorker::def_allocator_
  common::ObString db_name_;
  common::ObString table_name_;
  common::ObString column_name_;
  common::ObString status_column_name_;  // __ai_status_<c>
  common::ObString srcver_column_name_;  // __ai_srcver_<c>
  common::ObString expr_;                // column expression text
  common::ObString expr_hint_;           // hint stored after the expression (may be empty)
  common::ObString type_hint_;           // result type hint, appended to the prompt (design 7.1)
  common::ObSEArray<uint64_t, 4> dep_column_ids_;
};

// All AI columns of one data table, together with the primary key column names
// needed to address a row by value.
struct ObAIColumnTargetTable
{
  ObAIColumnTargetTable();
  void reset();
  int64_t find_column_index(const uint64_t column_id) const;
  TO_STRING_KV(K_(table_id), K_(db_name), K_(table_name), K_(pk_column_names),
               K_(columns));
  uint64_t table_id_;
  common::ObString db_name_;
  common::ObString table_name_;
  common::ObSEArray<common::ObString, 4> pk_column_names_;
  common::ObSEArray<ObAIColumnDef, 8> columns_;
};

// One row that has at least one AI column in PENDING state.
struct ObAIColumnCandidate
{
  ObAIColumnCandidate();
  void reset();
  // rendered primary key predicate, e.g. "`id` = 123 AND `k` = 'a'"
  common::ObString row_key_;
  // per table.columns_ index
  common::ObSEArray<int64_t, 8> statuses_;
  common::ObSEArray<int64_t, 8> srcvers_;
};

// One AI column participating in a publish step.
struct ObAIColumnWorkItem
{
  ObAIColumnWorkItem();
  void reset();
  int64_t col_index_;       // index into ObAIColumnTargetTable::columns_
  int64_t retry_count_;     // retries already consumed (0 on the first attempt)
  int64_t srcver_;          // __ai_srcver_<c> captured when the row was claimed
  bool ok_;                 // value produced and validated
  bool exhausted_;          // failed and no retry budget left -> FAILED
  common::ObString literal_;  // rendered SQL literal of the value (when ok_)
  common::ObString err_msg_;  // failure reason
};

// "Requeue later" bookkeeping.  Fixed size POD on purpose: it lives in an
// ObSEArray and must be copyable without an allocator.
struct ObAIColumnBackoff
{
  static const int64_t MAX_ROW_KEY_LEN = 320;
  static const int64_t MAX_REQUEST_ID_LEN = 40;
  ObAIColumnBackoff();
  void reset();
  uint64_t table_id_;
  uint64_t column_id_;
  int64_t col_index_;     // position inside the cached target table
  int64_t retry_count_;   // attempts already consumed
  int64_t next_ts_;       // earliest next evaluation time
  int64_t srcver_;        // version fence captured at claim time
  int64_t task_id_;       // __all_ai_column_task.task_id
  int32_t row_key_len_;
  char row_key_[MAX_ROW_KEY_LEN];
  char request_id_[MAX_REQUEST_ID_LEN];
};

// The asynchronous AI COLUMN generation worker.
//
// It is the *only* component that calls a model for AI columns: it discovers
// definitions, claims rows through the durable status marker, evaluates the
// stored column expression with an internal SQL SELECT (so the existing AI
// function stack does the provider/HTTP/retry work), validates the returned
// values against the declared column type and publishes them with a
// version-fenced internal UPDATE.
class ObAIColumnWorker : public rootserver::ObServerThreadHelper
{
public:
  static const int64_t DEFAULT_IDLE_INTERVAL_US = 200 * 1000;             // 200ms
  static const int64_t DEFAULT_DEF_REFRESH_INTERVAL_US = 2 * 1000 * 1000; // 2s
  static const int64_t DEFAULT_BATCH_SIZE = 32;
  static const int64_t BACKOFF_BASE_US = 200 * 1000;         // 200ms
  static const int64_t BACKOFF_MAX_US = 2 * 1000 * 1000;     // 2s
  static const int64_t SHORT_STMT_TIMEOUT_US = 30 * 1000 * 1000;
  static const int64_t EVAL_STMT_TIMEOUT_US = 10 * 60 * 1000 * 1000;

public:
  ObAIColumnWorker();
  virtual ~ObAIColumnWorker();

  int init();
  int start();
  void stop();
  void wait();
  void destroy();
  virtual void do_work() override;
  // logservice::ObILocalLogHandler (via ObServerThreadHelper): leader/follower
  // hooks. The worker runs on every replica but only ever picks up work through
  // the __all_ai_column_task lease columns, so switching role is a no-op.
  virtual int activate() override;
  virtual void deactivate() override;

  void set_idle_interval_us(const int64_t us)
  {
    if (us > 0) {
      idle_interval_us_ = us;
    }
  }
  int64_t get_idle_interval_us() const { return idle_interval_us_; }
  bool is_inited() const { return is_inited_; }

private:
  // ---- worker loop ----
  int run_round_();
  int recover_stuck_rows_(ObInnerSQLConnection &conn);
  int process_table_(ObInnerSQLConnection &conn,
                     const ObAIColumnTargetTable &table);
  int process_backoffs_(ObInnerSQLConnection &conn);

  // ---- discovery ----
  int refresh_definitions_(ObInnerSQLConnection &conn);
  int refresh_table_schema_(ObAIColumnTargetTable &table);

  // ---- claim ----
  int collect_candidates_(ObInnerSQLConnection &conn,
                          const ObAIColumnTargetTable &table,
                          common::ObIArray<ObAIColumnCandidate> &rows);
  int claim_row_(ObInnerSQLConnection &conn,
                 const ObAIColumnTargetTable &table,
                 const ObAIColumnCandidate &row,
                 common::ObIArray<ObAIColumnWorkItem> &items,
                 int64_t &task_id);
  bool is_backing_off_(const uint64_t table_id,
                       const uint64_t column_id,
                       const common::ObString &row_key,
                       const int64_t srcver) const;

  // ---- evaluate ----
  int evaluate_row_(ObInnerSQLConnection &conn,
                    const ObAIColumnTargetTable &table,
                    const ObAIColumnCandidate &row,
                    common::ObIArray<ObAIColumnWorkItem> &items,
                    bool &row_gone);
  int evaluate_group_(ObInnerSQLConnection &conn,
                      const ObAIColumnTargetTable &table,
                      const common::ObString &row_key,
                      common::ObIArray<ObAIColumnWorkItem> &items,
                      bool &row_gone);
  int evaluate_one_(ObInnerSQLConnection &conn,
                    const ObAIColumnTargetTable &table,
                    const common::ObString &row_key,
                    ObAIColumnWorkItem &item,
                    bool &row_gone);
  int evaluate_statement_(ObInnerSQLConnection &conn,
                          const ObAIColumnTargetTable &table,
                          const common::ObString &row_key,
                          ObAIColumnWorkItem &item,
                          bool &row_gone);
  static int set_type_hint_(ObInnerSQLConnection &conn, const common::ObString &type_hint);

  // ---- publish ----
  int publish_row_(ObInnerSQLConnection &conn,
                   const ObAIColumnTargetTable &table,
                   const ObAIColumnCandidate &row,
                   common::ObIArray<ObAIColumnWorkItem> &items,
                   const int64_t task_id);
  int publish_retry_(ObInnerSQLConnection &conn,
                     const ObAIColumnTargetTable &table,
                     const ObAIColumnBackoff &backoff,
                     const ObAIColumnWorkItem &item);
  int fail_retry_(ObInnerSQLConnection &conn,
                  const ObAIColumnTargetTable &table,
                  const ObAIColumnBackoff &backoff);
  int requeue_or_fail_(const ObAIColumnTargetTable &table,
                       const ObAIColumnCandidate &row,
                       common::ObIArray<ObAIColumnWorkItem> &items,
                       const int64_t task_id,
                       const char *request_id,
                       const int64_t now);
  int purge_backoffs_(const uint64_t table_id, const common::ObString &row_key);

  // ---- __all_ai_column_task ----
  int insert_task_row_(ObInnerSQLConnection &conn,
                       const ObAIColumnTargetTable &table,
                       const ObAIColumnCandidate &row,
                       const common::ObIArray<ObAIColumnWorkItem> &items,
                       const char *request_id,
                       int64_t &task_id);
  int delete_task_row_(ObInnerSQLConnection &conn, const int64_t task_id);
  int update_task_row_(ObInnerSQLConnection &conn,
                       const int64_t task_id,
                       const int64_t task_status,
                       const int64_t retry_count,
                       const common::ObString &err_msg);

  // ---- value handling ----
  int check_value_(ObAIColumnWorkItem &item,
                   const ObAIColumnDef &def,
                   const common::ObString &value);
  int render_value_(ObAIColumnWorkItem &item,
                    const ObAIColumnDef &def,
                    const common::ObString &value);
  int read_column_text_(const common::ObObj &obj, common::ObString &text);

  // ---- SQL text helpers ----
  int append_ident_(common::ObSqlString &sql, const common::ObString &name) const;
  int append_qualified_table_(common::ObSqlString &sql,
                              const common::ObString &db_name,
                              const common::ObString &table_name) const;
  int append_text_literal_(const common::ObString &value, common::ObSqlString &sql) const;
  int append_obj_literal_(const common::ObObj &obj,
                          const common::ObObjPrintParams &print_params,
                          common::ObSqlString &sql);
  int build_claim_sql_(const ObAIColumnTargetTable &table,
                       const ObAIColumnCandidate &row,
                       const common::ObIArray<int64_t> &group,
                       common::ObSqlString &sql) const;
  int64_t find_table_index_(const uint64_t table_id) const;

  // ---- misc ----
  int deep_copy_str_(common::ObIAllocator &allocator,
                     const common::ObString &src,
                     common::ObString &dst);
  int parse_dependencies_(const common::ObString &text,
                          common::ObSEArray<uint64_t, 4> &ids) const;
  int next_task_id_(int64_t &task_id);
  void gen_request_id_(char *buf, const int64_t buf_len) const;
  int64_t next_backoff_ts_(const int64_t retry_count) const;
  static int execute_sql_(ObInnerSQLConnection &conn,
                          const common::ObSqlString &sql,
                          const int64_t timeout_us,
                          int64_t &affected_rows);

private:
  bool is_inited_;
  int64_t idle_interval_us_;
  int64_t def_refresh_interval_us_;
  int64_t batch_size_;
  int64_t last_def_refresh_ts_;
  int64_t task_id_counter_;
  bool recovered_;
  // observability counters (per process, dumped in logs)
  int64_t claimed_count_;
  int64_t published_count_;
  int64_t dropped_count_;
  int64_t failed_count_;
  int64_t round_count_;
  // definitions outlive single rounds (refreshed every def_refresh_interval_us_),
  // per-round scratch (claims, rendered literals) is freed on every round.
  common::ObArenaAllocator def_allocator_;
  common::ObArenaAllocator round_allocator_;
  common::ObSEArray<ObAIColumnTargetTable, 8> tables_;
  common::ObSEArray<ObAIColumnBackoff, 16> backoffs_;
  DISALLOW_COPY_AND_ASSIGN(ObAIColumnWorker);
};

} // namespace observer

namespace omt
{

class ObAiServiceGuard
{
public:
  ObAiServiceGuard();
  ~ObAiServiceGuard();
  int get_ai_endpoint(const common::ObString &name, const share::ObAiModelEndpointInfo *&endpoint_info);
  int get_ai_endpoint_by_ai_model_name(const common::ObString &ai_model_name, const share::ObAiModelEndpointInfo *&endpoint_info, bool need_check = true);
private:
  int check_access_privilege();
  ObArenaAllocator local_allocator_;
  DISALLOW_COPY_AND_ASSIGN(ObAiServiceGuard);
};

class ObAiService
{
public:
  ObAiService();
  virtual ~ObAiService();
  static int server_module_init(ObAiService* &ai_service);
  int init();
  int start();
  void stop();
  void wait();
  void destroy();
public:
  int get_ai_service_guard(ObAiServiceGuard &ai_service_guard);
  // AI COLUMN async generation worker.  It is owned by this module, so the
  // worker lifecycle follows the existing mods_ai_service_ registration in
  // ObServer::obs_{construct,init,start,stop,wait,destroy}_modules().
  observer::ObAIColumnWorker &get_ai_column_worker() { return ai_column_worker_; }
  void wakeup_ai_column_worker() { ai_column_worker_.wakeup(); }
private:
  bool is_inited_;
  observer::ObAIColumnWorker ai_column_worker_;
  DISALLOW_COPY_AND_ASSIGN(ObAiService);
};

} // namespace omt
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_OMT_OB_AI_SERVICE_H_
