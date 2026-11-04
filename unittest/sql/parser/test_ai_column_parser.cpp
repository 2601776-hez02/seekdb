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

#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "lib/oblog/ob_log.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_mod_define.h"
#include "common/sql_mode/ob_sql_mode.h"
#include "sql/parser/ob_parser.h"
#include "sql/parser/ob_parser_utils.h"
#include "sql/parser/parse_define.h"
#include "sql/parser/parse_node.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace
{

// Parse tree layouts produced by the ai column grammar:
//   T_AI_COLUMN                   : children_[0] = expr, children_[1] = T_AI_COLUMN_OPTIONS or NULL
//   T_AI_COLUMN_OPTIONS           : num_child_ = option number, children_[i] = T_AI_COLUMN_OPTION
//   T_AI_COLUMN_OPTION            : children_[0] = name (T_IDENT), children_[1] = value (T_INT / T_VARCHAR)
//   T_SHOW_AI_COLUMNS             : children_[0] = relation, children_[1] = T_FROM_LIST or NULL
//   T_ALTER_TABLE_RETRY_AI_COLUMN : children_[0] = column name, children_[1] = T_INT(0/1), children_[2] = where or NULL

const int64_t TREE_BUF_LEN = 1 << 20;

class TestAiColumnParser : public ::testing::Test
{
public:
  TestAiColumnParser() : allocator_(ObModIds::TEST) {}
  virtual ~TestAiColumnParser() {}
  virtual void SetUp() {}
  virtual void TearDown() {}
public:
  ObArenaAllocator allocator_;
};

int parse_stmt(ObIAllocator &allocator,
               const char *sql,
               ParseResult &parse_result,
               std::string &printed_tree)
{
  int ret = OB_SUCCESS;
  ObParser parser(allocator, SMO_DEFAULT);
  const ObString query = ObString::make_string(sql);
  printed_tree.clear();
  if (OB_SUCCESS != (ret = parser.parse(query, parse_result))) {
    // The error code and error message are kept for the caller: the negative
    // cases only require the parse to fail.
  } else if (OB_ISNULL(parse_result.result_tree_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    char *buf = static_cast<char *>(allocator.alloc(TREE_BUF_LEN));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      ObParserResultTreePrintWrapper wrapper(*parse_result.result_tree_);
      const int64_t pos = wrapper.to_string(buf, TREE_BUF_LEN);
      if (pos > 0) {
        printed_tree.assign(buf, static_cast<std::string::size_type>(pos));
      }
    }
  }
  return ret;
}

void collect_nodes(const ParseNode *node,
                   const ObItemType type,
                   std::vector<const ParseNode *> &out)
{
  if (NULL == node) {
  } else {
    if (type == node->type_) {
      out.push_back(node);
    }
    for (int64_t i = 0; i < node->num_child_; ++i) {
      collect_nodes(node->children_[i], type, out);
    }
  }
}

const ParseNode *find_first_node(const ParseNode *node, const ObItemType type)
{
  const ParseNode *found = NULL;
  if (NULL != node) {
    if (type == node->type_) {
      found = node;
    } else {
      for (int64_t i = 0; NULL == found && i < node->num_child_; ++i) {
        found = find_first_node(node->children_[i], type);
      }
    }
  }
  return found;
}

std::string node_string(const ParseNode *node)
{
  std::string str;
  if (NULL != node && NULL != node->str_value_ && node->str_len_ > 0) {
    str.assign(node->str_value_, static_cast<std::string::size_type>(node->str_len_));
  }
  return str;
}

// Fetches the T_AI_COLUMN nodes of a statement, in statement order.
void get_ai_columns(const ParseResult &parse_result, std::vector<const ParseNode *> &ai_columns)
{
  ai_columns.clear();
  collect_nodes(parse_result.result_tree_, T_AI_COLUMN, ai_columns);
}

const ParseNode *get_ai_column_option(const ParseNode *ai_column, const char *name)
{
  const ParseNode *option = NULL;
  const ParseNode *options = (NULL == ai_column) ? NULL : ai_column->children_[1];
  if (NULL != options && T_AI_COLUMN_OPTIONS == options->type_) {
    for (int64_t i = 0; NULL == option && i < options->num_child_; ++i) {
      const ParseNode *cur = options->children_[i];
      if (NULL != cur && T_AI_COLUMN_OPTION == cur->type_ && 2 == cur->num_child_
          && node_string(cur->children_[0]) == name) {
        option = cur;
      }
    }
  }
  return option;
}

TEST_F(TestAiColumnParser, create_table_single_ai_column)
{
  const char *sql = "CREATE TABLE docs ("
                    "id BIGINT PRIMARY KEY, "
                    "language VARCHAR(32) AI COLUMN ("
                    "AI_COMPLETE('doc_model', CONCAT('Return only the language name. Content: ', content))))";
  ParseResult parse_result;
  std::string tree;
  ASSERT_EQ(OB_SUCCESS, parse_stmt(allocator_, sql, parse_result, tree)) << tree;
  ASSERT_NE(std::string::npos, tree.find("T_AI_COLUMN"));
  std::vector<const ParseNode *> ai_columns;
  get_ai_columns(parse_result, ai_columns);
  ASSERT_EQ(1, static_cast<int>(ai_columns.size()));
  const ParseNode *ai_column = ai_columns.at(0);
  ASSERT_EQ(2, ai_column->num_child_);
  // children_[0]: the ai expression, whose text is copied for the resolver
  // (same convention as the generated column expression).
  ASSERT_TRUE(NULL != ai_column->children_[0]);
  ASSERT_EQ(T_FUN_SYS, ai_column->children_[0]->type_);
  ASSERT_EQ("AI_COMPLETE",
            node_string(ai_column->children_[0]->children_[0]));
  ASSERT_EQ("AI_COMPLETE('doc_model', CONCAT('Return only the language name. Content: ', content))",
            node_string(ai_column->children_[0]));
  // children_[1]: no WITH options
  ASSERT_TRUE(NULL == ai_column->children_[1]);
}

TEST_F(TestAiColumnParser, create_table_multi_ai_columns)
{
  const char *sql = "CREATE TABLE docs ("
                    "id BIGINT PRIMARY KEY, "
                    "content VARCHAR(8192), "
                    "language VARCHAR(32) AI COLUMN ("
                    "AI_COMPLETE('doc_model', CONCAT('language: ', content))) "
                    "WITH (sync_mode='async', max_retries=3), "
                    "tags JSON AI COLUMN ("
                    "AI_COMPLETE('doc_model', CONCAT('tags: ', content))) "
                    "WITH (sync_mode='async', max_retries=5), "
                    "emb VECTOR(4) AI COLUMN ("
                    "AI_EMBED('embed_model', content, 4)) WITH (max_retries=1))";
  ParseResult parse_result;
  std::string tree;
  ASSERT_EQ(OB_SUCCESS, parse_stmt(allocator_, sql, parse_result, tree)) << tree;
  std::vector<const ParseNode *> ai_columns;
  get_ai_columns(parse_result, ai_columns);
  ASSERT_EQ(3, static_cast<int>(ai_columns.size()));

  const ParseNode *language = ai_columns.at(0);
  ASSERT_TRUE(NULL != language->children_[1]);
  ASSERT_EQ(T_AI_COLUMN_OPTIONS, language->children_[1]->type_);
  ASSERT_EQ(2, language->children_[1]->num_child_);
  const ParseNode *sync_mode = get_ai_column_option(language, "sync_mode");
  ASSERT_TRUE(NULL != sync_mode);
  ASSERT_EQ("async", node_string(sync_mode->children_[1]));
  const ParseNode *max_retries = get_ai_column_option(language, "max_retries");
  ASSERT_TRUE(NULL != max_retries);
  ASSERT_EQ(T_INT, max_retries->children_[1]->type_);
  ASSERT_EQ(3, max_retries->children_[1]->value_);

  const ParseNode *tags = ai_columns.at(1);
  ASSERT_TRUE(NULL != get_ai_column_option(tags, "sync_mode"));
  ASSERT_EQ(5, get_ai_column_option(tags, "max_retries")->children_[1]->value_);

  const ParseNode *emb = ai_columns.at(2);
  ASSERT_EQ(T_FUN_SYS, emb->children_[0]->type_);
  ASSERT_EQ("AI_EMBED", node_string(emb->children_[0]->children_[0]));
  ASSERT_EQ(1, emb->children_[1]->num_child_);
  ASSERT_TRUE(NULL == get_ai_column_option(emb, "sync_mode"));
  ASSERT_EQ(1, get_ai_column_option(emb, "max_retries")->children_[1]->value_);
}

TEST_F(TestAiColumnParser, ai_column_option_forms)
{
  const char *sqls[] = {
      "CREATE TABLE t (c VARCHAR(32) AI COLUMN (AI_COMPLETE('m', content)))",
      "CREATE TABLE t (c VARCHAR(32) AI COLUMN (AI_COMPLETE('m', content)) WITH (sync_mode='async'))",
      "CREATE TABLE t (c VARCHAR(32) AI COLUMN (AI_COMPLETE('m', content)) WITH (max_retries=5))",
      "CREATE TABLE t (c VARCHAR(32) AI COLUMN (AI_COMPLETE('m', content)) "
      "WITH (sync_mode='async', max_retries=5))",
  };
  const int64_t expect_option_num[] = {0, 1, 1, 2};
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(expect_option_num) / sizeof(int64_t)); ++i) {
    ParseResult parse_result;
    std::string tree;
    ASSERT_EQ(OB_SUCCESS, parse_stmt(allocator_, sqls[i], parse_result, tree)) << sqls[i] << tree;
    std::vector<const ParseNode *> ai_columns;
    get_ai_columns(parse_result, ai_columns);
    ASSERT_EQ(1, static_cast<int>(ai_columns.size()));
    const ParseNode *options = ai_columns.at(0)->children_[1];
    if (0 == expect_option_num[i]) {
      ASSERT_TRUE(NULL == options);
    } else {
      ASSERT_TRUE(NULL != options);
      ASSERT_EQ(T_AI_COLUMN_OPTIONS, options->type_);
      ASSERT_EQ(expect_option_num[i], static_cast<int64_t>(options->num_child_));
    }
  }
}

TEST_F(TestAiColumnParser, ai_column_option_negative_value)
{
  // The grammar accepts a negative integer (it is a signed literal); rejecting an
  // out of range value is the resolver's job, not a syntax error.
  const char *sql = "CREATE TABLE t_ai_ddl_bad (id BIGINT PRIMARY KEY, "
                    "c VARCHAR(16) AI COLUMN (AI_COMPLETE('mock_doc_model', 'x')) "
                    "WITH (max_retries=-1))";
  ParseResult parse_result;
  std::string tree;
  ASSERT_EQ(OB_SUCCESS, parse_stmt(allocator_, sql, parse_result, tree)) << tree;
  std::vector<const ParseNode *> ai_columns;
  get_ai_columns(parse_result, ai_columns);
  ASSERT_EQ(1, static_cast<int>(ai_columns.size()));
  const ParseNode *max_retries = get_ai_column_option(ai_columns.at(0), "max_retries");
  ASSERT_TRUE(NULL != max_retries);
  ASSERT_EQ(-1, max_retries->children_[1]->value_);
}

TEST_F(TestAiColumnParser, alter_table_add_ai_column)
{
  const char *sql = "ALTER TABLE docs ADD COLUMN summary VARCHAR(2048) AI COLUMN ("
                    "AI_COMPLETE('doc_model', CONCAT('Summarize: ', content))) "
                    "WITH (sync_mode='async', max_retries=3)";
  ParseResult parse_result;
  std::string tree;
  ASSERT_EQ(OB_SUCCESS, parse_stmt(allocator_, sql, parse_result, tree)) << tree;
  std::vector<const ParseNode *> ai_columns;
  get_ai_columns(parse_result, ai_columns);
  ASSERT_EQ(1, static_cast<int>(ai_columns.size()));
  ASSERT_TRUE(NULL != find_first_node(parse_result.result_tree_, T_COLUMN_ADD));
  ASSERT_TRUE(NULL != find_first_node(parse_result.result_tree_, T_COLUMN_DEFINITION));
  ASSERT_EQ(2, ai_columns.at(0)->children_[1]->num_child_);
}

TEST_F(TestAiColumnParser, alter_table_modify_ai_column)
{
  // MODIFY (and CHANGE) reuse column_definition, so the ai column attribute must
  // be reachable there too.
  const char *sqls[] = {
      "ALTER TABLE docs MODIFY COLUMN summary VARCHAR(4096) AI COLUMN ("
      "AI_COMPLETE('doc_model', CONCAT('Summarize in one paragraph: ', content))) "
      "WITH (sync_mode='async', max_retries=5)",
      "ALTER TABLE docs CHANGE COLUMN summary new_summary VARCHAR(4096) AI COLUMN ("
      "AI_COMPLETE('doc_model', CONCAT('Summarize: ', content))) WITH (max_retries=2)",
  };
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(sqls) / sizeof(sqls[0])); ++i) {
    ParseResult parse_result;
    std::string tree;
    ASSERT_EQ(OB_SUCCESS, parse_stmt(allocator_, sqls[i], parse_result, tree)) << sqls[i] << tree;
    std::vector<const ParseNode *> ai_columns;
    get_ai_columns(parse_result, ai_columns);
    ASSERT_EQ(1, static_cast<int>(ai_columns.size()));
    ASSERT_TRUE(NULL != find_first_node(parse_result.result_tree_, T_COLUMN_DEFINITION));
  }
}

TEST_F(TestAiColumnParser, show_ai_columns)
{
  struct ShowCase
  {
    const char *sql_;
    const char *table_name_;
    bool has_db_clause_;
  };
  const ShowCase cases[] = {
      {"SHOW AI COLUMNS FROM docs", "docs", false},
      {"SHOW AI COLUMNS IN docs", "docs", false},
      {"SHOW AI COLUMNS IN mysql.docs", "docs", false},
      {"SHOW AI COLUMNS FROM docs FROM mysql", "docs", true},
  };
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(cases) / sizeof(cases[0])); ++i) {
    ParseResult parse_result;
    std::string tree;
    ASSERT_EQ(OB_SUCCESS, parse_stmt(allocator_, cases[i].sql_, parse_result, tree))
        << cases[i].sql_ << tree;
    ASSERT_NE(std::string::npos, tree.find("T_SHOW_AI_COLUMNS"));
    const ParseNode *show_node = parse_result.result_tree_->children_[0];
    ASSERT_TRUE(NULL != show_node);
    ASSERT_EQ(T_SHOW_AI_COLUMNS, show_node->type_);
    ASSERT_EQ(2, show_node->num_child_);
    ASSERT_TRUE(NULL != show_node->children_[0]);
    ASSERT_EQ(T_RELATION_FACTOR, show_node->children_[0]->type_);
    ASSERT_EQ(cases[i].table_name_, node_string(show_node->children_[0]));
    if (cases[i].has_db_clause_) {
      ASSERT_TRUE(NULL != show_node->children_[1]);
      ASSERT_EQ(T_FROM_LIST, show_node->children_[1]->type_);
    } else {
      ASSERT_TRUE(NULL == show_node->children_[1]);
    }
  }
}

TEST_F(TestAiColumnParser, alter_table_retry_ai_column)
{
  struct RetryCase
  {
    const char *sql_;
    const char *column_name_;
    int64_t failed_;
    bool has_where_;
  };
  const RetryCase cases[] = {
      {"ALTER TABLE docs RETRY AI COLUMN language", "language", 0, false},
      {"ALTER TABLE docs RETRY FAILED AI COLUMN tags", "tags", 1, false},
      {"ALTER TABLE docs RETRY FAILED AI COLUMN tags WHERE category = 'news'", "tags", 1, true},
      {"ALTER TABLE docs RETRY AI COLUMN language WHERE id BETWEEN 100 AND 200", "language", 0, true},
      {"ALTER TABLE docs RETRY FAILED AI COLUMN emb WHERE status = 3", "emb", 1, true},
      {"ALTER TABLE docs RETRY FAILED AI COLUMN emb WHERE `status` = 3", "emb", 1, true},
  };
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(cases) / sizeof(cases[0])); ++i) {
    ParseResult parse_result;
    std::string tree;
    ASSERT_EQ(OB_SUCCESS, parse_stmt(allocator_, cases[i].sql_, parse_result, tree))
        << cases[i].sql_ << tree;
    const ParseNode *retry_node =
        find_first_node(parse_result.result_tree_, T_ALTER_TABLE_RETRY_AI_COLUMN);
    ASSERT_TRUE(NULL != retry_node);
    ASSERT_EQ(3, retry_node->num_child_);
    // children_[0]: column name
    ASSERT_TRUE(NULL != retry_node->children_[0]);
    ASSERT_EQ(cases[i].column_name_, node_string(retry_node->children_[0]));
    // children_[1]: failed flag, T_INT with value_ 0/1
    ASSERT_TRUE(NULL != retry_node->children_[1]);
    ASSERT_EQ(T_INT, retry_node->children_[1]->type_);
    ASSERT_EQ(cases[i].failed_, retry_node->children_[1]->value_);
    // children_[2]: where clause or NULL
    if (cases[i].has_where_) {
      ASSERT_TRUE(NULL != retry_node->children_[2]);
      ASSERT_EQ(T_WHERE_CLAUSE, retry_node->children_[2]->type_);
    } else {
      ASSERT_TRUE(NULL == retry_node->children_[2]);
    }
  }
}

TEST_F(TestAiColumnParser, retry_and_failed_are_non_reserved_keywords)
{
  // RETRY/FAILED must stay usable as ordinary identifiers.
  const char *sqls[] = {
      "CREATE TABLE retry (failed INT, c1 INT)",
      "SELECT failed FROM retry",
      "ALTER TABLE retry ADD COLUMN retry BIGINT",
  };
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(sqls) / sizeof(sqls[0])); ++i) {
    ParseResult parse_result;
    std::string tree;
    ASSERT_EQ(OB_SUCCESS, parse_stmt(allocator_, sqls[i], parse_result, tree)) << sqls[i] << tree;
  }
}

TEST_F(TestAiColumnParser, invalid_ai_column_syntax)
{
  const char *sqls[] = {
      // no expression
      "CREATE TABLE t (c VARCHAR(32) AI COLUMN ())",
      // unterminated expression
      "CREATE TABLE t (c VARCHAR(32) AI COLUMN (AI_COMPLETE('m', content))",
      // missing option value
      "CREATE TABLE t (c VARCHAR(32) AI COLUMN (AI_COMPLETE('m', content)) WITH (max_retries=))",
      // missing option compare mark
      "CREATE TABLE t (c VARCHAR(32) AI COLUMN (AI_COMPLETE('m', content)) WITH (sync_mode))",
      // missing option parentheses
      "CREATE TABLE t (c VARCHAR(32) AI COLUMN (AI_COMPLETE('m', content)) WITH sync_mode='async')",
      // SHOW AI COLUMNS requires a relation
      "SHOW AI COLUMNS",
      // RETRY AI COLUMN requires a column name
      "ALTER TABLE t RETRY AI COLUMN",
      // RETRY FAILED AI COLUMN requires the AI COLUMN keywords
      "ALTER TABLE t RETRY FAILED",
      // WHERE requires an expression
      "ALTER TABLE t RETRY AI COLUMN c WHERE",
  };
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(sqls) / sizeof(sqls[0])); ++i) {
    ParseResult parse_result;
    std::string tree;
    ASSERT_NE(OB_SUCCESS, parse_stmt(allocator_, sqls[i], parse_result, tree)) << sqls[i];
  }
}

} // namespace

int main(int argc, char **argv)
{
  OB_LOGGER.set_log_level("INFO");
  int ret = ObCharset::init_charset();
  if (OB_SUCCESS != ret) {
    // continue anyway: the parser reports the failure per statement.
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
