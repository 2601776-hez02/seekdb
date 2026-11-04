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

#include <gtest/gtest.h>
#include "lib/allocator/page_arena.h"
#include "lib/container/ob_se_array.h"
#include "share/schema/ob_table_schema.h"
#include "sql/resolver/ddl/ob_ddl_resolver.h"

namespace oceanbase
{
namespace sql
{

using namespace common;
using namespace share::schema;

class TestAIColumnResolver : public ::testing::Test
{
public:
  TestAIColumnResolver() {}
  virtual ~TestAIColumnResolver() {}
  virtual void SetUp() {}
  virtual void TearDown() {}
  void build_ai_column(ObColumnSchemaV2 &column,
                       const uint64_t column_id,
                       const char *name);
  ParseNode *build_column_definition(ParseNode *ai_node);
  ParseNode *build_ai_node();
private:
  ObArenaAllocator allocator_;
};

void TestAIColumnResolver::build_ai_column(ObColumnSchemaV2 &column,
                                           const uint64_t column_id,
                                           const char *name)
{
  ObAIColumnSpec ai_spec;
  column.reset();
  column.set_table_id(500001);
  column.set_column_id(column_id);
  column.set_data_type(ObVarcharType);
  column.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  column.set_data_length(1024);
  ASSERT_EQ(OB_SUCCESS, column.set_column_name(name));
  ai_spec.def_version_ = 1;
  ai_spec.func_type_ = ObAIColumnFuncType::COMPLETE;
  ASSERT_EQ(OB_SUCCESS, ai_spec.set_model_name(
      ObString::make_string("doc_model"), allocator_));
  ASSERT_EQ(OB_SUCCESS, ai_spec.set_ai_function(
      ObString::make_string(OB_AI_COLUMN_FUNC_COMPLETE), allocator_));
  ASSERT_EQ(OB_SUCCESS, ai_spec.set_type_hint(
      ObString::make_string("Return only the value."), allocator_));
  ASSERT_EQ(OB_SUCCESS, column.set_ai_spec(ai_spec));
  column.set_ai_column();
}

ParseNode *TestAIColumnResolver::build_ai_node()
{
  ParseNode *ai_node = static_cast<ParseNode *>(allocator_.alloc(sizeof(ParseNode)));
  if (NULL != ai_node) {
    memset(ai_node, 0, sizeof(ParseNode));
    ai_node->type_ = T_AI_COLUMN;
    ai_node->num_child_ = 2;
    ai_node->children_ = static_cast<ParseNode **>(
        allocator_.alloc(2 * sizeof(ParseNode *)));
    if (NULL != ai_node->children_) {
      ai_node->children_[0] = NULL;
      ai_node->children_[1] = NULL;
    }
  }
  return ai_node;
}

ParseNode *TestAIColumnResolver::build_column_definition(ParseNode *ai_node)
{
  ParseNode *column_definition = static_cast<ParseNode *>(
      allocator_.alloc(sizeof(ParseNode)));
  if (NULL != column_definition) {
    memset(column_definition, 0, sizeof(ParseNode));
    column_definition->type_ = T_COLUMN_DEFINITION;
    column_definition->num_child_ = 3;
    column_definition->children_ = static_cast<ParseNode **>(
        allocator_.alloc(3 * sizeof(ParseNode *)));
    if (NULL != column_definition->children_) {
      ParseNode *attrs_node = static_cast<ParseNode *>(
          allocator_.alloc(sizeof(ParseNode)));
      if (NULL != attrs_node) {
        memset(attrs_node, 0, sizeof(ParseNode));
        attrs_node->type_ = T_EMPTY;
        attrs_node->num_child_ = 1;
        attrs_node->children_ = static_cast<ParseNode **>(
            allocator_.alloc(1 * sizeof(ParseNode *)));
        if (NULL != attrs_node->children_) {
          attrs_node->children_[0] = ai_node;
        }
      }
      column_definition->children_[0] = NULL;
      column_definition->children_[1] = NULL;
      column_definition->children_[2] = attrs_node;
    }
  }
  return column_definition;
}

// The hidden companion columns of an AI column are named __ai_status_<c> and
// __ai_srcver_<c>, they are hidden and carry AI_STATUS_COLUMN_FLAG.
TEST_F(TestAIColumnResolver, build_hidden_columns)
{
  ObColumnSchemaV2 ai_column;
  build_ai_column(ai_column, 1001, "language");
  ObSEArray<ObColumnSchemaV2, 2> hidden_columns;
  ASSERT_EQ(OB_SUCCESS, ObDDLResolver::build_ai_column_hidden_columns(
      ai_column, 1002, 1003, allocator_, hidden_columns));
  ASSERT_EQ(2, hidden_columns.count());
  ASSERT_EQ(1002, ai_column.get_ai_spec().status_column_id_);
  ASSERT_EQ(1003, ai_column.get_ai_spec().srcver_column_id_);

  const ObColumnSchemaV2 &status_column = hidden_columns.at(0);
  ASSERT_EQ(0, status_column.get_column_name_str().compare("__ai_status_language"));
  ASSERT_EQ(1002, status_column.get_column_id());
  ASSERT_EQ(ObTinyIntType, status_column.get_data_type());
  ASSERT_TRUE(status_column.is_hidden());
  ASSERT_TRUE(status_column.is_ai_status_column());
  ASSERT_FALSE(status_column.is_ai_column());

  const ObColumnSchemaV2 &srcver_column = hidden_columns.at(1);
  ASSERT_EQ(0, srcver_column.get_column_name_str().compare("__ai_srcver_language"));
  ASSERT_EQ(1003, srcver_column.get_column_id());
  ASSERT_EQ(ObIntType, srcver_column.get_data_type());
  ASSERT_TRUE(srcver_column.is_hidden());
  ASSERT_TRUE(srcver_column.is_ai_status_column());
}

// An invalid (or non AI) column must be rejected instead of producing columns
// with invalid column ids.
TEST_F(TestAIColumnResolver, build_hidden_columns_invalid_argument)
{
  ObColumnSchemaV2 ai_column;
  build_ai_column(ai_column, 1001, "language");
  ObSEArray<ObColumnSchemaV2, 2> hidden_columns;
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObDDLResolver::build_ai_column_hidden_columns(
      ai_column, OB_INVALID_ID, 1003, allocator_, hidden_columns));
  ASSERT_EQ(0, hidden_columns.count());

  ObColumnSchemaV2 ordinary_column;
  ordinary_column.set_column_id(1001);
  ASSERT_EQ(OB_SUCCESS, ordinary_column.set_column_name("c1"));
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObDDLResolver::build_ai_column_hidden_columns(
      ordinary_column, 1002, 1003, allocator_, hidden_columns));
  ASSERT_EQ(0, hidden_columns.count());
}

// find_ai_column_node is the only entry point which detects the AI attribute of
// a column definition, a column definition without the attribute is untouched.
TEST_F(TestAIColumnResolver, find_ai_column_node)
{
  ParseNode *ai_node = build_ai_node();
  ASSERT_TRUE(NULL != ai_node);
  ParseNode *column_definition = build_column_definition(ai_node);
  ASSERT_TRUE(NULL != column_definition);
  ASSERT_EQ(ai_node, ObDDLResolver::find_ai_column_node(*column_definition));

  ParseNode *plain_definition = build_column_definition(NULL);
  ASSERT_TRUE(NULL != plain_definition);
  ASSERT_TRUE(NULL == ObDDLResolver::find_ai_column_node(*plain_definition));

  ParseNode not_a_column_definition;
  memset(&not_a_column_definition, 0, sizeof(not_a_column_definition));
  not_a_column_definition.type_ = T_TABLE_ELEMENT_LIST;
  ASSERT_TRUE(NULL == ObDDLResolver::find_ai_column_node(not_a_column_definition));
}

// option validation matrix of the AI column options, see contract 2.1:
// only sync_mode='async' and max_retries in [0, 100] are accepted, the defaults
// are 'async' and 3.
TEST_F(TestAIColumnResolver, ai_column_option_default)
{
  ObAIColumnOption option;
  option.reset();
  ASSERT_EQ(static_cast<int64_t>(ObAIColumnOption::ASYNC),
            static_cast<int64_t>(option.sync_mode_));
  ASSERT_EQ(OB_DEFAULT_AI_COLUMN_MAX_RETRIES, option.max_retries_);
  ASSERT_EQ(3, OB_DEFAULT_AI_COLUMN_MAX_RETRIES);
  ASSERT_EQ(100, OB_MAX_AI_COLUMN_MAX_RETRIES);
  ObAIColumnOption copied;
  ASSERT_EQ(OB_SUCCESS, copied.assign(option));
  ASSERT_EQ(option.max_retries_, copied.max_retries_);
}

} // namespace sql
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
