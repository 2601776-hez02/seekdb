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

#define USING_LOG_PREFIX SQL_RESV

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "common/timezone/ob_time_convert.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/ob_sql_utils.h"
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/table_function/file_sql.h"
#include "sql/table_function/table_function.h"

namespace oceanbase
{
namespace sql
{
namespace file_sql_impl
{

using namespace common;
using namespace file_sql;

// Maps a reader status to the SQL error returned to the client.  The detailed
// message is forwarded as a user error so that bad rows and unsupported files
// stay diagnosable from the client side.
int to_ob_error(const Status status, const std::string &message)
{
  int ret = OB_ERR_UNEXPECTED;
  switch (status) {
    case Status::OK: ret = OB_SUCCESS; break;
    case Status::FILE_NOT_FOUND: ret = OB_FILE_NOT_EXIST; break;
    case Status::OPEN_FAILED: ret = OB_IO_ERROR; break;
    case Status::INVALID_OPTION: ret = OB_INVALID_ARGUMENT; break;
    case Status::UNSUPPORTED: ret = OB_NOT_SUPPORTED; break;
    case Status::TOO_LARGE: ret = OB_SIZE_OVERFLOW; break;
    case Status::PARSE_ERROR:
    case Status::CORRUPTED_FILE:
    default: ret = OB_ERR_UNEXPECTED; break;
  }
  if (OB_SUCCESS != ret && !message.empty()) {
    FORWARD_USER_ERROR_MSG(ret, "%s", message.c_str());
  }
  return ret;
}

// Relative paths and ".." segments must not escape the secure_file_priv root,
// so every path is canonicalized before it is checked.
int check_secure_local_path(const ObString &secure_file_priv, const std::string &path,
                            const bool is_directory)
{
  int ret = OB_SUCCESS;
  std::string canonical;
  std::string error;
  const Status status = canonicalize_path(path, canonical, error);
  if (Status::OK != status) {
    ret = to_ob_error(status, error);
  } else {
    const ObString checked(static_cast<int32_t>(canonical.size()), canonical.c_str());
    if (OB_FAIL(ObResolverUtils::check_secure_path(secure_file_priv, checked))) {
      if (OB_ERR_NO_PRIVILEGE == ret) {
        ret = OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
      }
    }
  }
  LOG_DEBUG("file sql path check", K(ret), K(is_directory), K(secure_file_priv),
            "path", ObString(static_cast<int32_t>(path.size()), path.c_str()));
  return ret;
}

// The literal directory of a glob pattern, with wildcard components dropped so
// that the secure_file_priv check can canonicalize a real directory.
std::string glob_directory(const std::string &pattern)
{
  std::string directory = pattern;
  for (;;) {
    const size_t slash = directory.rfind('/');
    if (std::string::npos == slash) {
      return std::string(".");
    }
    directory = directory.substr(0, slash);
    if (directory.empty()) {
      return std::string("/");
    }
    if (!is_glob_pattern(directory)) {
      return directory;
    }
  }
}

ObObjType to_obj_type(const ColumnType type)
{
  ObObjType obj_type = ObVarcharType;
  switch (type) {
    case ColumnType::SQL_NULL: obj_type = ObNullType; break;
    case ColumnType::BOOLEAN: obj_type = ObTinyIntType; break;
    case ColumnType::BIGINT: obj_type = ObIntType; break;
    case ColumnType::DOUBLE: obj_type = ObDoubleType; break;
    case ColumnType::DATE: obj_type = ObDateType; break;
    case ColumnType::DATETIME: obj_type = ObDateTimeType; break;
    case ColumnType::VARCHAR:
    default: obj_type = ObVarcharType; break;
  }
  return obj_type;
}

} // namespace file_sql_impl

using file_sql_impl::check_secure_local_path;
using file_sql_impl::glob_directory;
using file_sql_impl::to_ob_error;
using file_sql_impl::to_obj_type;

int bind_file_table_function(ObDMLResolver &resolver, const TableFunctionCall &call, TableItem *&item)
{
  return resolver.bind_file_table_function(call, item);
}

int compile_file_table_options(const TableItem &table, ObIAllocator &allocator, ObString &options)
{
  int ret = OB_SUCCESS;
  const ObString &encoded = table.file_sql_options_;
  char *buffer = nullptr;
  if (encoded.empty()) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(buffer = static_cast<char *>(allocator.alloc(encoded.length())))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buffer, encoded.ptr(), encoded.length());
    options.assign_ptr(buffer, encoded.length());
  }
  return ret;
}

namespace
{

using namespace file_sql;

// Streams one local file into the SQL engine.  The bind step already fixed the
// schema and the file snapshot, so execution never has to infer anything.
class FileTableScan : public ITableFunctionScan
{
public:
  explicit FileTableScan(const TableFunctionScanContext &context)
      : context_(context), file_size_(0), modify_time_(0), reader_(nullptr),
        row_allocator_("FileSqlScan"), timezone_info_(nullptr), opened_(false)
  {
  }

  ~FileTableScan() override { (void) close(); }

  int open() override
  {
    int ret = OB_SUCCESS;
    ObSQLSessionInfo *session = context_.exec_ctx_.get_my_session();
    bool enabled = false;
    if (OB_ISNULL(session)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
    } else if (!enabled) {
      ret = OB_NOT_SUPPORTED;
      FORWARD_USER_ERROR_MSG(ret, "file SQL table functions are disabled, run SET enable_file_sql = true");
    } else {
      timezone_info_ = session->get_timezone_info();
      std::string error;
      const Status status = decode_options(context_.options_.ptr(), context_.options_.length(),
                                           options_, path_, columns_, file_size_, modify_time_,
                                           files_);
      if (Status::OK != status) {
        ret = to_ob_error(status, std::string("invalid file SQL plan options"));
      }
    }
    if (OB_SUCC(ret) && files_.empty()) {
      int64_t size = 0;
      int64_t modify_time = 0;
      std::string error;
      const Status status = stat_file(path_, size, modify_time, error);
      if (Status::OK != status) {
        ret = to_ob_error(status, error);
      } else if (size != file_size_ || modify_time != modify_time_) {
        ret = OB_ERR_UNEXPECTED;
        FORWARD_USER_ERROR_MSG(ret, "file '%s' changed after the query was bound", path_.c_str());
      }
    }
    if (OB_SUCC(ret)) {
      projection_.clear();
      needed_.clear();
      if (context_.column_ids_.empty()) {
        // A table function with a bind-dependent output is planned without an
        // explicit column id list, so the scan is responsible for producing
        // every inferred column in order.  Returning zero cells here would make
        // the projection above the scan fail.
        for (size_t i = 0; OB_SUCC(ret) && i < columns_.size(); ++i) {
          projection_.push_back(static_cast<int64_t>(i));
          needed_.push_back(static_cast<int64_t>(i));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < context_.column_ids_.count(); ++i) {
          const uint64_t column_id = context_.column_ids_.at(i);
          const int64_t index = static_cast<int64_t>(column_id) - OB_APP_MIN_COLUMN_ID;
          if (column_id < static_cast<uint64_t>(OB_APP_MIN_COLUMN_ID) || index < 0 ||
              index >= static_cast<int64_t>(columns_.size())) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            projection_.push_back(index);
            if (std::find(needed_.begin(), needed_.end(), index) == needed_.end()) {
              needed_.push_back(index);
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      std::string error;
      Status status = Status::OK;
      if (!files_.empty()) {
        status = create_glob_reader(files_, reader_, error);
      } else {
        status = create_reader(path_, options_, columns_, needed_, reader_, error);
      }
      if (Status::OK != status) {
        ret = to_ob_error(status, error);
      } else if (OB_ISNULL(reader_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (Status::OK != (status = reader_->open(error))) {
        ret = to_ob_error(status, error);
      }
    }
    if (OB_SUCC(ret)) {
      opened_ = true;
    } else if (nullptr != reader_) {
      destroy_reader(reader_);
      reader_ = nullptr;
    }
    return ret;
  }

  int get_next_row(ObNewRow *&row) override
  {
    int ret = OB_SUCCESS;
    if (OB_ISNULL(reader_)) {
      ret = OB_NOT_INIT;
    } else {
      row_allocator_.reuse();
      bool eof = false;
      std::string error;
      const Status status = reader_->next(file_row_, eof, error);
      if (Status::OK != status) {
        ret = to_ob_error(status, error);
      } else if (eof) {
        ret = OB_ITER_END;
      } else {
        if (cells_.count() != static_cast<int64_t>(projection_.size())) {
          cells_.reuse();
          for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(projection_.size()); ++i) {
            OZ (cells_.push_back(ObObj()));
          }
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(projection_.size()); ++i) {
          const int64_t index = projection_[static_cast<size_t>(i)];
          if (index >= static_cast<int64_t>(file_row_.cells_.size())) {
            cells_.at(i).set_null();
          } else {
            OZ (to_obj(file_row_.cells_[static_cast<size_t>(index)],
                       columns_[static_cast<size_t>(index)].type_, cells_.at(i)));
          }
        }
        if (OB_SUCC(ret)) {
          ob_row_.cells_ = cells_.get_data();
          ob_row_.count_ = cells_.count();
          row = &ob_row_;
        }
      }
    }
    return ret;
  }

  int rescan(const ObIArray<ObObj> &arguments) override
  {
    int ret = OB_SUCCESS;
    UNUSED(arguments);
    if (OB_ISNULL(reader_)) {
      ret = OB_NOT_INIT;
    } else {
      reader_->close();
      std::string error;
      const Status status = reader_->open(error);
      if (Status::OK != status) {
        ret = to_ob_error(status, error);
      }
    }
    return ret;
  }

  int close() override
  {
    if (nullptr != reader_) {
      reader_->close();
      destroy_reader(reader_);
      reader_ = nullptr;
    }
    opened_ = false;
    return OB_SUCCESS;
  }

private:
  int to_obj(const Cell &cell, const ColumnType type, ObObj &object)
  {
    int ret = OB_SUCCESS;
    if (cell.is_null_ || ColumnType::SQL_NULL == type) {
      object.set_null();
    } else if (ColumnType::BOOLEAN == type) {
      object.set_tinyint(cell.bool_value_ ? 1 : 0);
    } else if (ColumnType::BIGINT == type) {
      object.set_int(cell.int_value_);
    } else if (ColumnType::DOUBLE == type) {
      object.set_double(cell.double_value_);
    } else if (ColumnType::DATE == type) {
      int32_t value = 0;
      const ObString text(static_cast<int32_t>(cell.text_.size()), cell.text_.c_str());
      if (OB_FAIL(ObTimeConverter::str_to_date(text, value))) {
        ret = OB_ERR_UNEXPECTED;
        FORWARD_USER_ERROR_MSG(ret, "invalid date value '%s'", cell.text_.c_str());
      } else {
        object.set_date(value);
      }
    } else if (ColumnType::DATETIME == type) {
      int64_t value = 0;
      const ObString text(static_cast<int32_t>(cell.text_.size()), cell.text_.c_str());
      ObTimeConvertCtx convert_ctx(timezone_info_, false);
      if (OB_FAIL(ObTimeConverter::str_to_datetime(text, convert_ctx, value))) {
        ret = OB_ERR_UNEXPECTED;
        FORWARD_USER_ERROR_MSG(ret, "invalid datetime value '%s'", cell.text_.c_str());
      } else {
        object.set_datetime(value);
      }
    } else {
      const int64_t length = static_cast<int64_t>(cell.text_.size());
      char *buffer = static_cast<char *>(row_allocator_.alloc(length + 1));
      if (OB_ISNULL(buffer)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (length > 0) {
          MEMCPY(buffer, cell.text_.data(), length);
        }
        buffer[length] = '\0';
        object.set_varchar(buffer, static_cast<int32_t>(length));
        object.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
        object.set_collation_level(CS_LEVEL_IMPLICIT);
      }
    }
    return ret;
  }

  TableFunctionScanContext context_;
  Options options_;
  std::string path_;
  std::vector<Column> columns_;
  std::vector<std::string> files_;
  std::vector<int64_t> projection_;
  std::vector<int64_t> needed_;
  int64_t file_size_;
  int64_t modify_time_;
  IRecordReader *reader_;
  // Fully qualified: oceanbase::sql::Row (a class template) is also visible in
  // this translation unit through the engine headers.
  file_sql::Row file_row_;
  ObSEArray<ObObj, 8> cells_;
  ObNewRow ob_row_;
  ObArenaAllocator row_allocator_;
  const ObTimeZoneInfo *timezone_info_;
  bool opened_;
};

} // namespace

int create_file_table_scan(const TableFunctionScanContext &context, ITableFunctionScan *&scan)
{
  int ret = OB_SUCCESS;
  void *memory = context.allocator_.alloc(sizeof(FileTableScan));
  if (OB_ISNULL(memory)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    scan = new (memory) FileTableScan(context);
  }
  return ret;
}

int ObDMLResolver::bind_file_table_function(const TableFunctionCall &call, TableItem *&item)
{
  using namespace file_sql;
  int ret = OB_SUCCESS;
  Options options;
  bool is_glob = false;
  std::string path;
  std::string error;
  std::vector<std::string> files;
  std::vector<Column> columns;
  int64_t file_size = 0;
  int64_t modify_time = 0;
  const char *function_name = (nullptr == call.descriptor_) ? "" : call.descriptor_->name_;
  if (OB_ISNULL(params_.session_info_) || OB_ISNULL(allocator_) || OB_ISNULL(call.descriptor_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (0 == STRCMP(function_name, "read_csv")) {
    options.format_ = FileFormat::CSV;
  } else if (0 == STRCMP(function_name, "read_json")) {
    options.format_ = FileFormat::JSON;
  } else if (0 == STRCMP(function_name, "read_ndjson")) {
    options.format_ = FileFormat::NDJSON;
  } else if (0 == STRCMP(function_name, "read_parquet")) {
    options.format_ = FileFormat::PARQUET;
  } else if (0 == STRCMP(function_name, "glob")) {
    options.format_ = FileFormat::CSV;
    is_glob = true;
  } else {
    ret = OB_ERR_UNEXPECTED;
  }

  // The switch is checked at bind time so that a plan can never be created for
  // a session that did not opt in, and again at execution time as a guard.
  bool enabled = false;
  if (OB_SUCC(ret)) {
    if (OB_FAIL(params_.session_info_->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
    } else if (!enabled) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL table functions");
      FORWARD_USER_ERROR_MSG(ret, "file SQL table functions are disabled; run SET enable_file_sql = true");
    }
  }

  const int64_t argument_count = (nullptr == call.arguments_) ? 0 : call.arguments_->num_child_;
  if (OB_FAIL(ret)) {
  } else if (0 == argument_count) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, function_name);
  }

  auto evaluate_constant = [&](const ParseNode &node, ObObj &value) -> int {
    int status = OB_SUCCESS;
    ObRawExpr *expression = nullptr;
    const ParamStore no_parameters;
    const ParamStore &parameters =
        (nullptr == params_.param_list_) ? no_parameters : *params_.param_list_;
    if (OB_FAIL(resolve_sql_expr(node, expression))) {
      status = ret;
    } else if (OB_ISNULL(expression)) {
      status = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(expression->deduce_type(session_info_))) {
      status = ret;
    } else if (OB_FAIL(expression->extract_info())) {
      status = ret;
    } else if (OB_FAIL(ObSQLUtils::calc_const_expr(session_info_, *expression, value, *allocator_,
                                                    parameters,
                                                    session_info_->get_cur_exec_ctx()))) {
      status = ret;
    }
    return status;
  };

  auto is_named_option = [](const ParseNode &node, ObString &name) -> bool {
    bool named = false;
    if (T_OP_EQ == node.type_ && 2 == node.num_child_ && nullptr != node.children_[0] &&
        nullptr != node.children_[1]) {
      const ParseNode *left = node.children_[0];
      // The name is a bare identifier in the grammar, so it arrives as a
      // T_COLUMN_REF.  Depending on how the parser fills the node the text may
      // sit in the column child, in the table child slot, or on the node
      // itself, so accept all three spellings.
      const ParseNode *ident = nullptr;
      if (T_COLUMN_REF == left->type_) {
        if (nullptr != left->str_value_ && left->str_len_ > 0) {
          ident = left;
        } else if (left->num_child_ > 1 && nullptr != left->children_[1] &&
                   T_IDENT == left->children_[1]->type_) {
          ident = left->children_[1];
        } else if (left->num_child_ > 0 && nullptr != left->children_[0] &&
                   T_IDENT == left->children_[0]->type_) {
          ident = left->children_[0];
        }
      }
      if (nullptr != ident) {
        name.assign_ptr(ident->str_value_, static_cast<int32_t>(ident->str_len_));
        named = true;
      } else if (T_VARCHAR == left->type_) {
        name.assign_ptr(left->str_value_, static_cast<int32_t>(left->str_len_));
        named = true;
      }
    }
    return named;
  };

  auto option_to_bool = [](const ObObj &value, bool &flag) -> int {
    int status = OB_SUCCESS;
    if (value.is_varchar()) {
      const ObString text = value.get_string();
      if (0 == text.case_compare("true") || 0 == text.case_compare("on") ||
          0 == text.case_compare("1") || 0 == text.case_compare("yes")) {
        flag = true;
      } else if (0 == text.case_compare("false") || 0 == text.case_compare("off") ||
                 0 == text.case_compare("0") || 0 == text.case_compare("no")) {
        flag = false;
      } else {
        status = OB_INVALID_ARGUMENT;
      }
    } else if (value.is_integer_type()) {
      int64_t int_value = 0;
      if (OB_SUCCESS == value.get_int(int_value)) {
        flag = 0 != int_value;
      } else {
        status = OB_INVALID_ARGUMENT;
      }
    } else {
      status = OB_INVALID_ARGUMENT;
    }
    return status;
  };

  auto option_to_int = [](const ObObj &value, int64_t &result) -> int {
    int status = OB_SUCCESS;
    if (value.is_integer_type()) {
      if (OB_SUCCESS != value.get_int(result)) {
        status = OB_INVALID_ARGUMENT;
      }
    } else if (value.is_varchar()) {
      const ObString text = value.get_string();
      bool negative = false;
      int64_t index = 0;
      if (text.length() > 0 && ('-' == text[0] || '+' == text[0])) {
        negative = ('-' == text[0]);
        index = 1;
      }
      int64_t parsed = 0;
      for (; index < text.length() && text[index] >= '0' && text[index] <= '9'; ++index) {
        parsed = parsed * 10 + (text[index] - '0');
      }
      if (index != text.length() || index <= (negative ? 1 : 0)) {
        status = OB_INVALID_ARGUMENT;
      } else {
        result = negative ? -parsed : parsed;
      }
    } else {
      status = OB_INVALID_ARGUMENT;
    }
    return status;
  };

  auto option_to_char = [](const ObObj &value, char &result) -> int {
    int status = OB_SUCCESS;
    if (!value.is_varchar()) {
      status = OB_INVALID_ARGUMENT;
    } else {
      const ObString text = value.get_string();
      if (1 != text.length()) {
        status = OB_INVALID_ARGUMENT;
      } else {
        result = text[0];
      }
    }
    return status;
  };

  auto apply_option = [&](const ObString &name, const ObObj &value) -> int {
    int status = OB_SUCCESS;
    bool flag = false;
    int64_t int_value = 0;
    char character = 0;
    // header/delimiter/quote/escape/nullstr/skip_rows only describe CSV layout;
    // accepting them for the other formats would silently do nothing.
    const bool csv_only =
        0 == name.case_compare("header") || 0 == name.case_compare("delim") ||
        0 == name.case_compare("delimiter") || 0 == name.case_compare("sep") ||
        0 == name.case_compare("quote") || 0 == name.case_compare("escape") ||
        0 == name.case_compare("nullstr") || 0 == name.case_compare("null_string") ||
        0 == name.case_compare("skip") || 0 == name.case_compare("skip_rows");
    if (csv_only && FileFormat::CSV != options.format_) {
      status = OB_INVALID_ARGUMENT;
      FORWARD_USER_ERROR_MSG(status, "option '%.*s' is only supported by read_csv",
                             name.length(), name.ptr());
    } else if (name.case_compare("header") == 0) {
      status = option_to_bool(value, flag);
      options.header_ = flag;
    } else if (name.case_compare("delim") == 0 || name.case_compare("delimiter") == 0 ||
               name.case_compare("sep") == 0) {
      status = option_to_char(value, character);
      options.delimiter_ = character;
    } else if (name.case_compare("quote") == 0) {
      status = option_to_char(value, character);
      options.quote_ = character;
    } else if (name.case_compare("escape") == 0) {
      status = option_to_char(value, character);
      options.escape_ = character;
    } else if (name.case_compare("nullstr") == 0 || name.case_compare("null_string") == 0) {
      if (!value.is_varchar()) {
        status = OB_INVALID_ARGUMENT;
      } else {
        const ObString text = value.get_string();
        options.null_string_.assign(text.ptr(), static_cast<size_t>(text.length()));
      }
    } else if (name.case_compare("skip") == 0 || name.case_compare("skip_rows") == 0) {
      status = option_to_int(value, int_value);
      if (OB_SUCCESS == status && int_value < 0) {
        status = OB_INVALID_ARGUMENT;
      }
      options.skip_rows_ = int_value;
    } else if (name.case_compare("sample_size") == 0) {
      status = option_to_int(value, int_value);
      if (OB_SUCCESS == status && -1 != int_value) {
        status = OB_NOT_SUPPORTED;
        FORWARD_USER_ERROR_MSG(status, "sample_size other than -1 is not supported");
      }
      options.sample_size_ = int_value;
    } else {
      status = OB_INVALID_ARGUMENT;
      FORWARD_USER_ERROR_MSG(status, "unknown table function option '%.*s'", name.length(),
                             name.ptr());
    }
    return status;
  };

  int64_t positional_count = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < argument_count; ++i) {
    const ParseNode *argument = call.arguments_->children_[i];
    ObString option_name;
    if (OB_ISNULL(argument)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (is_named_option(*argument, option_name)) {
      ObObj value;
      if (OB_SUCCESS != (ret = evaluate_constant(*argument->children_[1], value))) {
      } else if (OB_SUCCESS != (ret = apply_option(option_name, value))) {
      }
    } else if (positional_count > 0) {
      // Only the path may be passed positionally; every other argument has to
      // be written as "name = value".  Report it with ER_PARSE_ERROR (1064) so
      // that it matches how a malformed builtin argument list is reported.
      ret = OB_ERR_PARSE_SQL;
    } else {
      ObObj value;
      ++positional_count;
      if (OB_SUCCESS != (ret = evaluate_constant(*argument, value))) {
      } else if (!value.is_varchar()) {
        ret = OB_INVALID_ARGUMENT;
        FORWARD_USER_ERROR_MSG(ret, "%s expects a string path argument", function_name);
      } else {
        const ObString text = value.get_string();
        path.assign(text.ptr(), static_cast<size_t>(text.length()));
      }
    }
  }

  if (OB_SUCC(ret) && path.empty()) {
    ret = OB_INVALID_ARGUMENT;
    FORWARD_USER_ERROR_MSG(ret, "%s expects a file path", function_name);
  }
  if (OB_SUCC(ret)) {
    ObString secure_file_priv;
    if (OB_FAIL(params_.session_info_->get_secure_file_priv(secure_file_priv))) {
    } else if (is_glob) {
      if (!is_glob_pattern(path)) {
        ret = OB_INVALID_ARGUMENT;
        FORWARD_USER_ERROR_MSG(ret, "glob() expects a pattern containing *, ? or [");
      } else if (OB_FAIL(check_secure_local_path(secure_file_priv, glob_directory(path), true))) {
      }
    } else if (is_glob_pattern(path)) {
      ret = OB_NOT_SUPPORTED;
      FORWARD_USER_ERROR_MSG(ret, "a single file is expected, use glob() to discover files");
    } else if (OB_FAIL(check_secure_local_path(secure_file_priv, path, false))) {
    } else {
      const Status status = stat_file(path, file_size, modify_time, error);
      if (Status::OK != status) {
        ret = to_ob_error(status, error);
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (is_glob) {
      const Status status = expand_glob(path, files, error);
      if (Status::OK != status) {
        ret = to_ob_error(status, error);
      }
    } else {
      const Status status = infer_schema(path, options, columns, error);
      if (Status::OK != status) {
        ret = to_ob_error(status, error);
      }
    }
    if (OB_SUCC(ret) && is_glob) {
      Column column;
      column.name_ = "file";
      column.type_ = ColumnType::VARCHAR;
      columns.push_back(column);
    }
  }
  if (OB_SUCC(ret) && columns.empty()) {
    ret = OB_ERR_UNEXPECTED;
    FORWARD_USER_ERROR_MSG(ret, "no columns were inferred from '%s'", path.c_str());
  }

  // The scan only needs the path argument; named options are consumed here.
  ParseNode path_arguments = {};
  ParseNode *path_children[1] = {nullptr};
  const ParseNode *path_node = nullptr;
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < argument_count; ++i) {
      const ParseNode *argument = call.arguments_->children_[i];
      ObString option_name;
      if (OB_ISNULL(argument)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (!is_named_option(*argument, option_name)) {
        path_node = argument;
        break;
      }
    }
    if (OB_SUCC(ret) && OB_ISNULL(path_node)) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  if (OB_SUCC(ret)) {
    path_children[0] = const_cast<ParseNode *>(path_node);
    path_arguments.type_ = T_EXPR_LIST;
    path_arguments.num_child_ = 1;
    path_arguments.children_ = path_children;
    TableFunctionCall scan_call = call;
    scan_call.arguments_ = &path_arguments;
    OZ (create_function_scan_item(scan_call, item));
  }
  if (OB_SUCC(ret)) {
    for (size_t i = 0; OB_SUCC(ret) && i < columns.size(); ++i) {
      ObObjMeta meta;
      ObAccuracy accuracy;
      meta.set_type(to_obj_type(columns[i].type_));
      if (ColumnType::VARCHAR == columns[i].type_) {
        // A table function column is a plain character column: it needs a
        // resolved collation (type *and* level) and a length that is legal for
        // a column, otherwise resolving a reference to it fails with an
        // internal error long before the scan is planned.
        meta.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
        meta.set_collation_level(CS_LEVEL_IMPLICIT);
        accuracy.set_length(ObAccuracy::MAX_ACCURACY2[0][ObVarcharType].get_length());
      } else if (ColumnType::BOOLEAN == columns[i].type_) {
        accuracy.set_length(1);
      }
      ObString column_name;
      const ObString raw_name(static_cast<int32_t>(columns[i].name_.size()),
                              columns[i].name_.c_str());
      ColumnItem *column_item = nullptr;
      if (OB_FAIL(ob_write_string(*allocator_, raw_name, column_name))) {
      } else if (OB_FAIL(resolve_function_table_column_item(*item, meta, accuracy, column_name,
                                                            OB_APP_MIN_COLUMN_ID + i,
                                                            column_item))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    const std::string encoded = encode_options(options, path, columns, file_size, modify_time,
                                               files);
    ObString stored;
    const ObString raw_encoded(static_cast<int32_t>(encoded.size()), encoded.c_str());
    if (OB_FAIL(ob_write_string(*allocator_, raw_encoded, stored))) {
    } else {
      item->file_sql_options_ = stored;
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
