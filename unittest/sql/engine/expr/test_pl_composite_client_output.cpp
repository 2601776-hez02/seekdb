/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#include <gtest/gtest.h>
#include <string>
#include "sql/engine/expr/pl_composite_client_output.h"
#include "sql/pl/ob_pl.h"
#include "sql/pl/ob_pl_user_type.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
namespace sql
{
namespace
{
using namespace common;
using namespace pl;

class OutputTypeNamespace final : public ObPLINS
{
public:
  int get_user_type(uint64_t id, const ObUserDefinedType *&type,
                    ObIAllocator *allocator = nullptr) const override
  {
    UNUSED(allocator);
    type = nullptr;
    for (int64_t i = 0; i < types_.count(); ++i) {
      if (types_.at(i)->get_user_type_id() == id) {
        type = types_.at(i);
      }
    }
    return nullptr == type ? OB_ENTRY_NOT_EXIST : OB_SUCCESS;
  }
  ObSEArray<const ObUserDefinedType *, 4> types_;
};

class PlCompositeOutputTest : public ::testing::Test
{
protected:
  ObObj collection(ObPLCollection &collection)
  {
    ObObj value;
    value.set_extend(reinterpret_cast<int64_t>(&collection), collection.get_type());
    return value;
  }
  ObObj record(ObPLRecord &record)
  {
    ObObj value;
    value.set_extend(reinterpret_cast<int64_t>(&record), PL_RECORD_TYPE);
    return value;
  }
  void expect_json(ObObj value, const std::string &expected)
  {
    ASSERT_EQ(OB_SUCCESS, PlCompositeClientOutput::convert(
        value, allocator_, &types_, session_, nullptr));
    ASSERT_TRUE(value.is_varchar());
    EXPECT_EQ(CS_TYPE_UTF8MB4_BIN, value.get_collation_type());
    EXPECT_EQ(expected, std::string(value.get_string_ptr(), value.get_string_len()));
  }

  ObArenaAllocator allocator_;
  ObSQLSessionInfo session_;
  OutputTypeNamespace types_;
};

TEST_F(PlCompositeOutputTest, null_empty_and_deleted_slots_are_distinct)
{
  ObPLCollection values(PL_NESTED_TABLE_TYPE, 100);
  ObObj output = collection(values);
  ASSERT_EQ(OB_SUCCESS, PlCompositeClientOutput::convert(
      output, allocator_, &types_, session_, nullptr));
  EXPECT_TRUE(output.is_null()); // uninitialized collection

  values.set_inited();
  expect_json(collection(values), "[]");
  ObObj elements[4];
  elements[0].set_int(1);
  elements[1].set_type(ObMaxType); // deleted slot, rather than a null element
  elements[2].set_null();
  elements[3].set_int(3);
  values.set_data(elements, 4);
  values.set_count(4);
  expect_json(collection(values), "[1,null,3]");
  EXPECT_TRUE(elements[1].is_invalid_type());
  EXPECT_TRUE(elements[2].is_null());
  EXPECT_EQ(3, elements[3].get_int());

  values.set_null();
  output = collection(values);
  ASSERT_EQ(OB_SUCCESS, PlCompositeClientOutput::convert(
      output, allocator_, &types_, session_, nullptr));
  EXPECT_TRUE(output.is_null());
}

TEST_F(PlCompositeOutputTest, scalar_and_sql_null_keep_their_existing_representation)
{
  ObObj value;
  value.set_int(42);
  ASSERT_EQ(OB_SUCCESS, PlCompositeClientOutput::convert(
      value, allocator_, &types_, session_, nullptr));
  EXPECT_TRUE(value.is_int());
  EXPECT_EQ(42, value.get_int());
  value.set_null();
  ASSERT_EQ(OB_SUCCESS, PlCompositeClientOutput::convert(
      value, allocator_, &types_, session_, nullptr));
  EXPECT_TRUE(value.is_null());
  value.set_extend(0, PL_RECORD_TYPE);
  ASSERT_EQ(OB_SUCCESS, PlCompositeClientOutput::convert(
      value, allocator_, &types_, session_, nullptr));
  EXPECT_TRUE(value.is_null());
}

TEST_F(PlCompositeOutputTest, declared_record_names_and_nested_values)
{
  ObPLDataType scalar;
  ObRecordType profile_type;
  profile_type.set_user_type_id(101);
  ASSERT_EQ(OB_SUCCESS, profile_type.record_members_init(&allocator_, 1));
  ASSERT_EQ(OB_SUCCESS, profile_type.add_record_member(ObString::make_string("city"), scalar));
  ASSERT_EQ(OB_SUCCESS, types_.types_.push_back(&profile_type));
  ObObj city;
  city.set_varchar("杭州");
  city.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ObPLRecord profile;
  profile.set_id(101);
  profile.set_count(1);
  profile.set_data(&city);

  ObObj tags[3];
  tags[0].set_varchar("数据库");
  tags[0].set_collation_type(CS_TYPE_UTF8MB4_BIN);
  tags[1].set_null();
  tags[2].set_varchar("AI");
  tags[2].set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ObPLCollection tag_list(PL_VARRAY_TYPE, 102);
  tag_list.set_count(3);
  tag_list.set_data(tags, 3);

  ObRecordType user_type;
  user_type.set_user_type_id(103);
  ASSERT_EQ(OB_SUCCESS, user_type.record_members_init(&allocator_, 3));
  ASSERT_EQ(OB_SUCCESS, user_type.add_record_member(ObString::make_string("id"), scalar));
  ASSERT_EQ(OB_SUCCESS, user_type.add_record_member(ObString::make_string("profile"), scalar));
  ASSERT_EQ(OB_SUCCESS, user_type.add_record_member(ObString::make_string("tags"), scalar));
  ASSERT_EQ(OB_SUCCESS, types_.types_.push_back(&user_type));
  ObObj members[3];
  members[0].set_int(1);
  members[1] = record(profile);
  members[2] = collection(tag_list);
  ObPLRecord user;
  user.set_id(103);
  user.set_count(3);
  user.set_data(members);

  expect_json(record(user), "{\"id\":1,\"profile\":{\"city\":\"杭州\"},\"tags\":[\"数据库\",null,\"AI\"]}");
  // Output never changes the original graph or its declared composite values.
  EXPECT_EQ(PL_RECORD_TYPE, members[1].get_meta().get_extend_type());
  EXPECT_EQ(PL_VARRAY_TYPE, members[2].get_meta().get_extend_type());
  EXPECT_EQ(3, tag_list.get_count());
}

TEST_F(PlCompositeOutputTest, escaping_is_json_escaping_not_sql_quoting)
{
  ObPLDataType scalar;
  ObRecordType type;
  type.set_user_type_id(104);
  ASSERT_EQ(OB_SUCCESS, type.record_members_init(&allocator_, 1));
  ASSERT_EQ(OB_SUCCESS, type.add_record_member(ObString::make_string("a\"\n"), scalar));
  ASSERT_EQ(OB_SUCCESS, types_.types_.push_back(&type));
  ObObj text;
  text.set_varchar("x\t\\\"y");
  text.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ObPLRecord value;
  value.set_id(104);
  value.set_count(1);
  value.set_data(&text);
  expect_json(record(value), "{\"a\\\"\\n\":\"x\\t\\\\\\\"y\"}");
}

TEST_F(PlCompositeOutputTest, declared_booleans_use_json_boolean_literals)
{
  ObRecordType type;
  type.set_user_type_id(108);
  ASSERT_EQ(OB_SUCCESS, type.record_members_init(&allocator_, 1));
  ASSERT_EQ(OB_SUCCESS, type.add_record_member(
      ObString::make_string("enabled"), ObPLDataType(ObTinyIntType)));
  ASSERT_EQ(OB_SUCCESS, types_.types_.push_back(&type));
  ObObj member;
  member.set_tinyint(1);
  ObPLRecord value;
  value.set_id(108);
  value.set_count(1);
  value.set_data(&member);
  expect_json(record(value), "{\"enabled\":true}");
  member.set_tinyint(0);
  expect_json(record(value), "{\"enabled\":false}");
}

TEST_F(PlCompositeOutputTest, scalar_datum_storage_covers_numeric_widths_and_borrowed_strings)
{
  ObObj leaf;
  ObPLCollection list(PL_NESTED_TABLE_TYPE, 109);
  list.set_count(1);
  list.set_data(&leaf, 1);
  leaf.set_int(INT64_MIN);
  expect_json(collection(list), "[-9223372036854775808]");
  leaf.set_uint64(UINT64_MAX);
  expect_json(collection(list), "[18446744073709551615]");
  leaf.set_float(1.25f);
  expect_json(collection(list), "[1.25]");
  leaf.set_double(2.5);
  expect_json(collection(list), "[2.5]");

  number::ObNumber number;
  ASSERT_EQ(OB_SUCCESS, number.from("123456789012345678901234567890.25", allocator_));
  leaf.set_number(number);
  leaf.set_scale(2);
  expect_json(collection(list), "[123456789012345678901234567890.25]");

  // Exercise even the largest DECIMAL datum layout, not just machine integers.
  ObDecimalIntBuilder decimal;
  decimal.from(int512_t(12345));
  for (int32_t width : {4, 8, 16, 32, 64}) {
    leaf.set_decimal_int(width, 2, const_cast<ObDecimalInt *>(decimal.get_decimal_int()));
    expect_json(collection(list), "[123.45]");
  }

  const std::string long_text(1024, 'x');
  leaf.set_varchar(ObString(long_text.size(), long_text.data()));
  leaf.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  expect_json(collection(list), "[\"" + long_text + "\"]");
}

TEST_F(PlCompositeOutputTest, unsupported_keyed_values_do_not_lose_keys)
{
  ObPLCollection keyed(PL_ASSOCIATIVE_ARRAY_TYPE, 105);
  keyed.set_inited();
  ObObj output = collection(keyed);
  EXPECT_EQ(OB_NOT_SUPPORTED, PlCompositeClientOutput::convert(
      output, allocator_, &types_, session_, nullptr));
  EXPECT_TRUE(output.is_ext());
}

TEST_F(PlCompositeOutputTest, missing_declaration_does_not_invent_member_names)
{
  ObObj member;
  member.set_int(1);
  ObPLRecord value;
  value.set_id(999);
  value.set_count(1);
  value.set_data(&member);
  ObObj output = record(value);
  EXPECT_EQ(OB_ENTRY_NOT_EXIST, PlCompositeClientOutput::convert(
      output, allocator_, &types_, session_, nullptr));
  EXPECT_TRUE(output.is_ext());
  value.set_null();
  ASSERT_EQ(OB_SUCCESS, PlCompositeClientOutput::convert(
      output, allocator_, &types_, session_, nullptr));
  EXPECT_TRUE(output.is_null()); // a NULL record needs no name lookup
}

TEST_F(PlCompositeOutputTest, cycles_and_unsupported_members_fail_without_mutation)
{
  ObPLCollection cycle(PL_NESTED_TABLE_TYPE, 106);
  ObObj child = collection(cycle);
  cycle.set_count(1);
  cycle.set_data(&child, 1);
  ObObj output = child;
  EXPECT_EQ(OB_ERR_JSON_OUT_OF_DEPTH, PlCompositeClientOutput::convert(
      output, allocator_, &types_, session_, nullptr));
  EXPECT_EQ(child.get_ext(), output.get_ext());

  child.set_extend(1, PL_CURSOR_TYPE);
  EXPECT_EQ(OB_NOT_SUPPORTED, PlCompositeClientOutput::convert(
      output, allocator_, &types_, session_, nullptr));
  EXPECT_TRUE(output.is_ext());
}

} // namespace
} // namespace sql
} // namespace oceanbase
