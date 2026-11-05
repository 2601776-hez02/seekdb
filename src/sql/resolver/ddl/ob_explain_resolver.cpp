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
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_node.h"
#include "sql/table_function/file_sql.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;

namespace oceanbase
{
namespace sql
{
namespace
{
// Type name reported by DESCRIBE SELECT, in the spelling the file formats use.
const char *describe_type_name(const file_sql::ColumnType type)
{
  const char *name = "VARCHAR";
  switch (type) {
    case file_sql::ColumnType::SQL_NULL: name = "VARCHAR"; break;
    case file_sql::ColumnType::BOOLEAN: name = "BOOLEAN"; break;
    case file_sql::ColumnType::BIGINT: name = "BIGINT"; break;
    case file_sql::ColumnType::DOUBLE: name = "DOUBLE"; break;
    case file_sql::ColumnType::DATE: name = "DATE"; break;
    case file_sql::ColumnType::DATETIME: name = "DATETIME"; break;
    case file_sql::ColumnType::VARCHAR: name = "VARCHAR"; break;
    default: name = "VARCHAR"; break;
  }
  return name;
}

int append_sql_text(ObSqlString &sql, const std::string &text)
{
  int ret = OB_SUCCESS;
  std::string escaped;
  escaped.reserve(text.size() + 2);
  escaped.push_back('\'');
  for (size_t i = 0; i < text.size(); ++i) {
    if ('\'' == text[i]) {
      escaped.push_back('\'');
    }
    escaped.push_back(text[i]);
  }
  escaped.push_back('\'');
  if (OB_FAIL(sql.append(escaped.c_str(), static_cast<int64_t>(escaped.size())))) {
  }
  return ret;
}
}
}
}

// DESCRIBE over a file table function reports the inferred column list instead of
// a plan: the schema only exists at bind time, so it is rendered as a plain
// SELECT of (column_name, column_type, null, key, default, extra) rows.
int ObExplainResolver::resolve_file_table_describe(ObStmt *child_stmt, bool &done)
{
  int ret = OB_SUCCESS;
  done = false;
  ObDMLStmt *dml_stmt = NULL;
  const TableItem *file_item = NULL;
  if (OB_ISNULL(child_stmt)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("child stmt is null", K(ret));
  } else if (!child_stmt->is_dml_stmt()) {
    // EXPLAIN of a non-DML statement keeps its normal behaviour.
  } else {
    dml_stmt = static_cast<ObDMLStmt *>(child_stmt);
    const ObIArray<TableItem *> &items = dml_stmt->get_table_items();
    for (int64_t i = 0; NULL == file_item && i < items.count(); ++i) {
      const TableItem *item = items.at(i);
      if (NULL != item && !item->file_sql_options_.empty()) {
        file_item = item;
      }
    }
  }
  if (OB_SUCC(ret) && NULL != file_item) {
    file_sql::Options options;
    std::string path;
    std::vector<file_sql::Column> columns;
    std::vector<std::string> files;
    int64_t file_size = 0;
    int64_t modify_time = 0;
    const file_sql::Status status = file_sql::decode_options(
        file_item->file_sql_options_.ptr(), file_item->file_sql_options_.length(), options, path,
        columns, file_size, modify_time, files);
    if (file_sql::Status::OK != status || columns.empty()) {
      LOG_WARN("failed to decode the file table function options, fall back to a plan",
               K(ret), K(status), K(file_item->file_sql_options_.length()));
    } else if (OB_ISNULL(params_.allocator_) || OB_ISNULL(session_info_)) {
      ret = OB_NOT_INIT;
    } else {
      ObSqlString sql;
      for (size_t i = 0; OB_SUCC(ret) && i < columns.size(); ++i) {
        const file_sql::Column &column = columns[i];
        if (0 != i && OB_FAIL(sql.append(" UNION ALL "))) {
        } else if (OB_FAIL(sql.append("SELECT "))) {
        } else if (OB_FAIL(append_sql_text(sql, column.name_))) {
        } else if (OB_FAIL(sql.append(" AS column_name, "))) {
        } else if (OB_FAIL(append_sql_text(sql, std::string(describe_type_name(column.type_))))) {
        } else if (OB_FAIL(sql.append(" AS column_type, "))) {
        } else if (OB_FAIL(append_sql_text(sql, std::string(column.nullable_ ? "YES" : "NO")))) {
        } else if (OB_FAIL(sql.append(" AS `null`, NULL AS `key`, NULL AS `default`, "
                                      "NULL AS `extra`"))) {
        }
      }
      if (OB_SUCC(ret)) {
        ParseResult parse_result;
        ObParser parser(*params_.allocator_, session_info_->get_sql_mode());
        if (OB_FAIL(parser.parse(sql.string(), parse_result))) {
          LOG_WARN("failed to parse the describe column list", K(ret), K(sql));
        } else if (OB_ISNULL(parse_result.result_tree_) ||
                   parse_result.result_tree_->num_child_ != 1 ||
                   OB_ISNULL(parse_result.result_tree_->children_[0])) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected describe parse tree", K(ret));
        } else {
          ObSelectResolver select_resolver(params_);
          if (OB_FAIL(select_resolver.resolve(*parse_result.result_tree_->children_[0]))) {
            LOG_WARN("failed to resolve the describe column list", K(ret), K(sql));
          } else if (OB_ISNULL(select_resolver.get_select_stmt())) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("describe column list produced no select stmt", K(ret));
          } else {
            set_basic_stmt(select_resolver.get_select_stmt());
            session_info_->set_stmt_type(stmt::T_SELECT);
            done = true;
          }
        }
      }
    }
  }
  return ret;
}

int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      bool described = false;
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else if (T_SELECT == child_node->type_ &&
                 NULL == parse_tree.children_[EXPLAIN_FORMAT] &&
                 NULL == parse_tree.children_[EXPLAIN_INTO_TABLE] &&
                 OB_FAIL(resolve_file_table_describe(child_stmt, described))) {
      } else if (described) {
        // The statement is a plain SELECT that renders the inferred column list,
        // the explain_stmt created above is not used in this case.
        LOG_TRACE("describe over a file table function returns the column list");
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret) && NULL != explain_stmt) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node && NULL != explain_stmt) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node && NULL != explain_stmt) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}
