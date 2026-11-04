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

#define USING_LOG_PREFIX SHARE

#include <gtest/gtest.h>
#include "lib/allocator/page_arena.h"
#include "lib/container/ob_se_array.h"
#include "lib/hash/ob_hashmap.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

using namespace common;

class TestAIColumnSchema : public ::testing::Test
{
public:
  TestAIColumnSchema() {}
  virtual ~TestAIColumnSchema() {}
  virtual void SetUp() {}
  virtual void TearDown() {}
  void build_ai_spec(ObAIColumnSpec &ai_spec);
  void build_ai_column(ObColumnSchemaV2 &column,
                       const uint64_t table_id,
                       const uint64_t column_id,
                       const char *name);
private:
  ObArenaAllocator allocator_;
};

void TestAIColumnSchema::build_ai_spec(ObAIColumnSpec &ai_spec)
{
  ai_spec.reset();
  ai_spec.def_version_ = 3;
  ai_spec.status_column_id_ = 1002;
  ai_spec.srcver_column_id_ = 1003;
  ai_spec.func_type_ = ObAIColumnFuncType::COMPLETE;
  ai_spec.option_.sync_mode_ = ObAIColumnOption::ASYNC;
  ai_spec.option_.max_retries_ = 5;
  ASSERT_EQ(OB_SUCCESS, ai_spec.set_model_name(ObString::make_string("doc_model"), allocator_));
  ASSERT_EQ(OB_SUCCESS, ai_spec.set_ai_function(
      ObString::make_string(OB_AI_COLUMN_FUNC_COMPLETE), allocator_));
  ASSERT_EQ(OB_SUCCESS, ai_spec.set_type_hint(
      ObString::make_string("Return only the value."), allocator_));
  ASSERT_EQ(OB_SUCCESS, ai_spec.add_dep_column_id(1000));
  ASSERT_EQ(OB_SUCCESS, ai_spec.add_dep_column_id(1001));
}

void TestAIColumnSchema::build_ai_column(ObColumnSchemaV2 &column,
                                         const uint64_t table_id,
                                         const uint64_t column_id,
                                         const char *name)
{
  ObAIColumnSpec ai_spec;
  column.reset();
  column.set_table_id(table_id);
  column.set_column_id(column_id);
  column.set_data_type(ObVarcharType);
  column.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  column.set_data_length(1024);
  ASSERT_EQ(OB_SUCCESS, column.set_column_name(name));
  build_ai_spec(ai_spec);
  ASSERT_EQ(OB_SUCCESS, column.set_ai_spec(ai_spec));
  column.set_ai_column();
}

TEST_F(TestAIColumnSchema, ai_column_flags)
{
  ObColumnSchemaV2 column;
  build_ai_column(column, 500001, 1001, "language");
  ASSERT_TRUE(column.is_ai_column());
  ASSERT_FALSE(column.is_ai_status_column());
  ASSERT_FALSE(column.is_generated_column());
  ASSERT_FALSE(column.is_virtual_generated_column());
  ASSERT_FALSE(column.is_stored_generated_column());
  ASSERT_TRUE(column.has_cascaded_column_id(1000));

  ObColumnSchemaV2 status_column;
  status_column.set_column_id(1002);
  status_column.set_data_type(ObTinyIntType);
  status_column.set_is_hidden(true);
  status_column.set_ai_status_column();
  ASSERT_TRUE(status_column.is_ai_status_column());
  ASSERT_TRUE(status_column.is_hidden());
  ASSERT_FALSE(status_column.is_ai_column());
  ASSERT_FALSE(status_column.is_user_visible_column());
}

TEST_F(TestAIColumnSchema, ai_column_spec_serialize)
{
  ObAIColumnSpec ai_spec;
  build_ai_spec(ai_spec);
  char buf[4096];
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, ai_spec.serialize(buf, sizeof(buf), pos));
  ASSERT_EQ(ai_spec.get_serialize_size(), pos);

  ObAIColumnSpec deserialized;
  deserialized.set_allocator(&allocator_);
  int64_t deserialize_pos = 0;
  ASSERT_EQ(OB_SUCCESS, deserialized.deserialize(buf, pos, deserialize_pos));
  ASSERT_EQ(pos, deserialize_pos);
  ASSERT_EQ(ai_spec.def_version_, deserialized.def_version_);
  ASSERT_EQ(ai_spec.status_column_id_, deserialized.status_column_id_);
  ASSERT_EQ(ai_spec.srcver_column_id_, deserialized.srcver_column_id_);
  ASSERT_EQ(ai_spec.func_type_, deserialized.func_type_);
  ASSERT_EQ(0, ai_spec.model_name_.compare(deserialized.model_name_));
  ASSERT_EQ(0, ai_spec.ai_function_.compare(deserialized.ai_function_));
  ASSERT_EQ(0, ai_spec.type_hint_.compare(deserialized.type_hint_));
  ASSERT_EQ(ai_spec.option_.max_retries_, deserialized.option_.max_retries_);
  ASSERT_EQ(static_cast<int64_t>(ai_spec.option_.sync_mode_),
            static_cast<int64_t>(deserialized.option_.sync_mode_));
  ASSERT_EQ(ai_spec.dep_column_ids_.count(), deserialized.dep_column_ids_.count());
  for (int64_t i = 0; i < ai_spec.dep_column_ids_.count(); ++i) {
    ASSERT_EQ(ai_spec.dep_column_ids_.at(i), deserialized.dep_column_ids_.at(i));
  }
  ASSERT_TRUE(deserialized.is_valid());
}

TEST_F(TestAIColumnSchema, ai_column_spec_deep_copy)
{
  ObAIColumnSpec ai_spec;
  build_ai_spec(ai_spec);
  ObAIColumnSpec copied;
  copied.reset();
  ASSERT_EQ(OB_SUCCESS, copied.assign(ai_spec, allocator_));
  // the copied strings must not alias the source buffers
  ASSERT_NE(ai_spec.model_name_.ptr(), copied.model_name_.ptr());
  ASSERT_EQ(0, ai_spec.model_name_.compare(copied.model_name_));
  ASSERT_EQ(0, ai_spec.type_hint_.compare(copied.type_hint_));
  ASSERT_EQ(ai_spec.dep_column_ids_.count(), copied.dep_column_ids_.count());

  ObColumnSchemaV2 column;
  build_ai_column(column, 500001, 1001, "language");
  ASSERT_EQ(0, column.get_ai_spec().model_name_.compare("doc_model"));
  ASSERT_EQ(0, column.get_ai_spec().ai_function_.compare(OB_AI_COLUMN_FUNC_COMPLETE));
  ASSERT_EQ(3, column.get_ai_spec().def_version_);
  ASSERT_EQ(5, column.get_ai_spec().option_.max_retries_);
}

TEST_F(TestAIColumnSchema, ai_column_serialize_column_schema)
{
  ObColumnSchemaV2 column;
  build_ai_column(column, 500001, 1001, "language");
  char buf[8192];
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, column.serialize(buf, sizeof(buf), pos));
  ObColumnSchemaV2 deserialized;
  int64_t deserialize_pos = 0;
  ASSERT_EQ(OB_SUCCESS, deserialized.deserialize(buf, pos, deserialize_pos));
  ASSERT_EQ(pos, deserialize_pos);
  ASSERT_TRUE(deserialized.is_ai_column());
  ASSERT_EQ(3, deserialized.get_ai_spec().def_version_);
  ASSERT_EQ(1002, deserialized.get_ai_spec().status_column_id_);
  ASSERT_EQ(1003, deserialized.get_ai_spec().srcver_column_id_);
  ASSERT_EQ(0, deserialized.get_ai_spec().model_name_.compare("doc_model"));
  ASSERT_EQ(2, deserialized.get_ai_spec().dep_column_ids_.count());
}

TEST_F(TestAIColumnSchema, ai_column_convert_column_id)
{
  ObColumnSchemaV2 column;
  build_ai_column(column, 500001, 1001, "language");
  common::hash::ObHashMap<uint64_t, uint64_t> column_id_map;
  ASSERT_EQ(OB_SUCCESS, column_id_map.create(16, "AIColumnTest"));
  ASSERT_EQ(OB_SUCCESS, column_id_map.set_refactored(1000, 2000));
  ASSERT_EQ(OB_SUCCESS, column_id_map.set_refactored(1001, 2001));
  ASSERT_EQ(OB_SUCCESS, column_id_map.set_refactored(1002, 2002));
  ASSERT_EQ(OB_SUCCESS, column_id_map.set_refactored(1003, 2003));
  ASSERT_EQ(OB_SUCCESS, column.convert_column_id(column_id_map));
  ASSERT_EQ(2001, column.get_column_id());
  ASSERT_EQ(2002, column.get_ai_spec().status_column_id_);
  ASSERT_EQ(2003, column.get_ai_spec().srcver_column_id_);
  ASSERT_EQ(1, column.get_ai_spec().dep_column_ids_.count());
  ASSERT_EQ(2000, column.get_ai_spec().dep_column_ids_.at(0));
  ASSERT_TRUE(column.has_cascaded_column_id(2000));
  ASSERT_FALSE(column.has_cascaded_column_id(1000));
}

TEST_F(TestAIColumnSchema, ai_column_table_copy)
{
  ObTableSchema table_schema;
  table_schema.set_table_id(500001);
  ASSERT_EQ(OB_SUCCESS, table_schema.set_table_name("docs"));
  ObColumnSchemaV2 id_column;
  id_column.set_table_id(500001);
  id_column.set_column_id(1000);
  id_column.set_data_type(ObIntType);
  ASSERT_EQ(OB_SUCCESS, id_column.set_column_name("id"));
  ASSERT_EQ(OB_SUCCESS, table_schema.add_column(id_column));

  ObColumnSchemaV2 ai_column;
  build_ai_column(ai_column, 500001, 1001, "language");
  ASSERT_EQ(OB_SUCCESS, table_schema.add_column(ai_column));

  ObColumnSchemaV2 status_column;
  status_column.set_table_id(500001);
  status_column.set_column_id(1002);
  status_column.set_data_type(ObTinyIntType);
  status_column.set_is_hidden(true);
  status_column.set_ai_status_column();
  ASSERT_EQ(OB_SUCCESS, status_column.set_column_name("__ai_status_language"));
  ASSERT_EQ(OB_SUCCESS, table_schema.add_column(status_column));

  ASSERT_TRUE(table_schema.has_ai_column());
  ASSERT_EQ(3, table_schema.get_column_count());
  const ObColumnSchemaV2 *column = table_schema.get_column_schema(static_cast<uint64_t>(1001));
  ASSERT_TRUE(NULL != column);
  ASSERT_TRUE(column->is_ai_column());
  const ObColumnSchemaV2 *hidden = table_schema.get_column_schema_by_idx(2);
  ASSERT_TRUE(NULL != hidden);
  ASSERT_TRUE(hidden->is_ai_status_column());

  ObTableSchema copied_schema;
  ASSERT_EQ(OB_SUCCESS, copied_schema.assign(table_schema));
  ASSERT_TRUE(copied_schema.has_ai_column());
  const ObColumnSchemaV2 *copied_column =
      copied_schema.get_column_schema(static_cast<uint64_t>(1001));
  ASSERT_TRUE(NULL != copied_column);
  ASSERT_TRUE(copied_column->is_ai_column());
  ASSERT_EQ(3, copied_column->get_ai_spec().def_version_);
  ASSERT_EQ(0, copied_column->get_ai_spec().model_name_.compare("doc_model"));
  ASSERT_EQ(2, copied_column->get_ai_spec().dep_column_ids_.count());
}

} // namespace schema
} // namespace share
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
