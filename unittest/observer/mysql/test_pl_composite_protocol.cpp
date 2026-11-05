/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#include <gtest/gtest.h>
#include <string>
#include "observer/mysql/ob_mysql_result_set.h"
#include "observer/mysql/obsm_row.h"

namespace oceanbase
{
namespace observer
{
namespace
{
using namespace common;
using namespace obmysql;

TEST(PlCompositeProtocolTest, metadata_preserves_names_without_mutating_logical_types)
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  const pl::ObPLType types[] = {pl::PL_RECORD_TYPE, pl::PL_NESTED_TABLE_TYPE,
                                pl::PL_VARRAY_TYPE, pl::PL_ASSOCIATIVE_ARRAY_TYPE};
  for (auto type : types) {
    for (auto charset : {CS_TYPE_INVALID, CS_TYPE_BINARY, CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI}) {
      ObField logical;
      logical.type_.set_extend(0, type);
      logical.cname_ = ObString::make_string("selected_value");
      logical.org_cname_ = ObString::make_string("stored_value");
      logical.tname_ = ObString::make_string("u");
      logical.org_tname_ = ObString::make_string("users");
      logical.charsetnr_ = charset;
      ObMySQLField client;
      ASSERT_EQ(OB_SUCCESS, ObMySQLResultSet::to_mysql_field(logical, client));
      EXPECT_EQ(MYSQL_TYPE_VAR_STRING, client.type_);
      EXPECT_EQ(logical.cname_, client.cname_);
      EXPECT_EQ(logical.org_cname_, client.org_cname_);
      EXPECT_EQ(logical.tname_, client.tname_);
      EXPECT_EQ(logical.org_tname_, client.org_tname_);
      EXPECT_EQ(charset == CS_TYPE_UTF8MB4_GENERAL_CI ? charset : CS_TYPE_UTF8MB4_BIN, client.charsetnr_);
      EXPECT_EQ(OB_MAX_LONGTEXT_LENGTH, client.length_);
      EXPECT_EQ(ObExtendType, logical.type_.get_type());
      EXPECT_EQ(type, logical.type_.get_meta().get_extend_type());
    }
  }
}

TEST(PlCompositeProtocolTest, ordinary_text_and_binary_writers_keep_json_as_single_cells)
{
  ObArenaAllocator allocator;
  sql::ObSQLSessionInfo session;
  const ObDataTypeCastParams cast_params;
  const std::string record = "{\"id\":1,\"profile\":{\"city\":\"杭州\"},\"tags\":[\"AI\",null]}";
  ObObj cells[3];
  cells[0].set_varchar(ObString(record.size(), record.data()));
  cells[0].set_collation_type(CS_TYPE_UTF8MB4_BIN);
  cells[1].set_null();
  cells[2].set_varchar("[]");
  cells[2].set_collation_type(CS_TYPE_UTF8MB4_BIN);
  // Final conversion preserves the projection and number of cells. Exercise
  // the ordinary writer's projector as well as both protocol cell builders.
  int32_t projector[] = {2, 0, 1};
  ObNewRow row;
  row.cells_ = cells;
  row.count_ = 3;
  row.projector_ = projector;
  row.projector_size_ = 3;
  for (auto protocol : {MYSQL_PROTOCOL_TYPE::TEXT, MYSQL_PROTOCOL_TYPE::BINARY}) {
    ObSMRow writer(protocol, row, cast_params, session);
    EXPECT_EQ(3, writer.get_cells_count());
    ObMySQLCellValue cell;
    ASSERT_EQ(OB_SUCCESS, writer.build_cell_value(0, allocator, cell));
    EXPECT_EQ(ObMySQLCellValueKind::LENENC_BYTES, cell.kind_);
    EXPECT_EQ("[]", std::string(cell.get_bytes(), cell.get_bytes_len()));
    ASSERT_EQ(OB_SUCCESS, writer.build_cell_value(1, allocator, cell));
    EXPECT_EQ(ObMySQLCellValueKind::LENENC_BYTES, cell.kind_);
    EXPECT_EQ(record, std::string(cell.get_bytes(), cell.get_bytes_len()));
    ASSERT_EQ(OB_SUCCESS, writer.build_cell_value(2, allocator, cell));
    EXPECT_EQ(ObMySQLCellValueKind::NULL_VALUE, cell.kind_);
  }
}

} // namespace
} // namespace observer
} // namespace oceanbase
