/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#include <gtest/gtest.h>
#include <string>
#include "sql/engine/expr/ob_expr_output_pack.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/ob_sql_context.h"
#include "sql/pl/ob_pl.h"
#include "sql/pl/ob_pl_stmt.h"
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

class PlCompositeOutputPackTest : public ::testing::Test
{
protected:
  PlCompositeOutputPackTest()
      : exec_ctx_(allocator_), eval_ctx_(exec_ctx_), extra_info_(allocator_, T_OP_OUTPUT_PACK)
  {}

  void SetUp() override
  {
    ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
    ASSERT_EQ(OB_SUCCESS, session_.test_init(0, 1, &allocator_));
    ASSERT_EQ(OB_SUCCESS, session_.ensure_sys_var_loaded(share::SYS_VAR_CHARACTER_SET_RESULTS));
    session_.set_pl_stack_ctx(&pl_context_);
    exec_ctx_.set_my_session(&session_);
    sql_ctx_.schema_guard_ = &schema_guard_;
    exec_ctx_.set_sql_ctx(&sql_ctx_);
    eval_ctx_.frames_ = frames_;
    for (int i = 0; i < 5; ++i) {
      frames_[i] = frame_storage_[i];
      MEMSET(frames_[i], 0, sizeof(frame_storage_[i]));
      expressions_[i].frame_idx_ = i;
      expressions_[i].datum_off_ = 0;
      expressions_[i].eval_info_off_ = 32;
      expressions_[i].dyn_buf_header_offset_ = 64;
      expressions_[i].res_buf_off_ = 128;
      expressions_[i].res_buf_len_ = sizeof(frame_storage_[i]) - 128;
      expressions_[i].get_eval_info(eval_ctx_).evaluated_ = true;
      expressions_[i].locate_expr_datum(eval_ctx_).ptr_ = frames_[i] + 128;
    }
    for (int i = 0; i < 4; ++i) {
      args_[i] = &expressions_[i];
    }
    output_ = &expressions_[4];
    output_->args_ = args_;
    output_->arg_cnt_ = 4;
    output_->extra_info_ = &extra_info_;
    output_->datum_meta_.type_ = ObVarcharType;
    ASSERT_EQ(OB_SUCCESS, extra_info_.param_fields_.prepare_allocate(3));
  }

  void TearDown() override
  {
    session_.set_pl_stack_ctx(nullptr);
    pl_context_.get_exec_stack().reset();
    if (nullptr != pl_state_) {
      pl_state_->~ObPLExecState();
    }
    if (nullptr != function_) {
      function_->~ObPLFunction();
    }
    if (nullptr != function_context_.ref_context()) {
      DESTROY_CONTEXT(function_context_);
    }
  }

  void set_record_namespace(const ObRecordType &record_type)
  {
    lib::ContextParam param;
    ASSERT_EQ(OB_SUCCESS, CURRENT_CONTEXT->CREATE_CONTEXT(function_context_, param));
    void *memory = allocator_.alloc(sizeof(ObPLFunction));
    ASSERT_NE(nullptr, memory);
    function_ = new (memory) ObPLFunction(function_context_);
    ObPLUserTypeTable types;
    ASSERT_EQ(OB_SUCCESS, types.add_type(&record_type));
    ASSERT_EQ(OB_SUCCESS, function_->set_types(types));
    memory = allocator_.alloc(sizeof(ObPLExecState));
    ASSERT_NE(nullptr, memory);
    pl_state_ = new (memory) ObPLExecState(allocator_, allocator_, exec_ctx_,
                                         package_guard_, *function_, pl_result_, pl_status_);
    ASSERT_EQ(OB_SUCCESS, pl_context_.get_exec_stack().push_back(pl_state_));
  }

  void set_arg(int index, const ObObj &value)
  {
    ObExpr &expr = expressions_[index];
    expr.obj_meta_ = value.get_meta();
    expr.datum_meta_.type_ = value.get_type();
    expr.obj_datum_map_ = ObDatum::get_obj_datum_map_type(value.get_type());
    ASSERT_EQ(OB_SUCCESS, expr.locate_expr_datum(eval_ctx_).from_obj(value));
    if (index > 0) {
      // Deliberately retain logical metadata here. Binary output must not cast
      // the converted JSON cell back to this internal PL type.
      extra_info_.param_fields_.at(index - 1).type_ = value;
    }
  }

  void expect_payload(bool binary, const std::string &expected)
  {
    ObObj protocol;
    protocol.set_int(binary ? 0 : 1);
    set_arg(0, protocol);
    ObDatum result;
    ASSERT_EQ(OB_SUCCESS, ObExprOutputPack::eval_output_pack(*output_, eval_ctx_, result));
    ASSERT_GE(result.len_, 16);
    // The real Rust encoder produces a 16-byte packed-row envelope followed by
    // the MySQL row payload. Verify both its size and the actual wire bytes.
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(result.ptr_);
    EXPECT_EQ("SRWB", std::string(result.ptr_, 4));
    EXPECT_EQ(1, bytes[4]);
    EXPECT_EQ(16, bytes[6]);
    EXPECT_EQ(0, bytes[7]);
    uint64_t payload_length = 0;
    for (int i = 0; i < 8; ++i) {
      payload_length |= static_cast<uint64_t>(bytes[8 + i]) << (8 * i);
    }
    EXPECT_EQ(expected.size(), payload_length);
    EXPECT_EQ(expected, std::string(result.ptr_ + 16, result.len_ - 16));
  }

  ObArenaAllocator allocator_;
  ObSQLSessionInfo session_;
  ObPLContext pl_context_;
  share::schema::ObSchemaGetterGuard schema_guard_;
  ObSqlCtx sql_ctx_;
  ObExecContext exec_ctx_;
  ObEvalCtx eval_ctx_;
  ObOutputPackInfo extra_info_;
  lib::MemoryContext function_context_;
  ObPLFunction *function_ = nullptr;
  ObPLExecState *pl_state_ = nullptr;
  ObPLPackageGuard package_guard_;
  ObObj pl_result_;
  int pl_status_ = OB_SUCCESS;
  ObExpr expressions_[5];
  ObExpr *args_[4];
  ObExpr *output_;
  alignas(uint64_t) char frame_storage_[5][4096];
  char *frames_[5];
};

TEST_F(PlCompositeOutputPackTest, text_and_binary_encode_null_empty_and_nested_collections)
{
  ObPLCollection null_list(PL_NESTED_TABLE_TYPE, 201);
  ObPLCollection empty_list(PL_VARRAY_TYPE, 202);
  empty_list.set_inited();
  ObObj elements[3];
  elements[0].set_int(1);
  elements[1].set_null();
  elements[2].set_extend(reinterpret_cast<int64_t>(&empty_list), PL_VARRAY_TYPE);
  ObPLCollection nested_list(PL_NESTED_TABLE_TYPE, 203);
  nested_list.set_data(elements, 3);
  nested_list.set_count(3);

  ObObj input;
  input.set_extend(reinterpret_cast<int64_t>(&null_list), PL_NESTED_TABLE_TYPE);
  set_arg(1, input);
  input.set_extend(reinterpret_cast<int64_t>(&empty_list), PL_VARRAY_TYPE);
  set_arg(2, input);
  input.set_extend(reinterpret_cast<int64_t>(&nested_list), PL_NESTED_TABLE_TYPE);
  set_arg(3, input);

  const std::string cells = std::string("\x02[]\x0b[1,null,[]]", 15);
  expect_payload(false, std::string(1, '\xfb') + cells);
  expect_payload(true, std::string("\x00\x04", 2) + cells);
  EXPECT_EQ(PL_VARRAY_TYPE, elements[2].get_meta().get_extend_type());
  EXPECT_TRUE(elements[1].is_null());
  EXPECT_EQ(3, nested_list.get_count());
}

TEST_F(PlCompositeOutputPackTest, record_names_come_from_the_active_pl_declaration)
{
  ObRecordType record_type;
  record_type.set_user_type_id(204);
  ASSERT_EQ(OB_SUCCESS, record_type.record_members_init(&allocator_, 1));
  ASSERT_EQ(OB_SUCCESS, record_type.add_record_member(
      ObString::make_string("label"), ObPLDataType(ObVarcharType)));
  set_record_namespace(record_type);
  ObObj member;
  member.set_varchar("a\"b");
  member.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ObPLRecord record;
  record.set_id(204);
  record.set_count(1);
  record.set_data(&member);
  ObObj value;
  value.set_extend(reinterpret_cast<int64_t>(&record), PL_RECORD_TYPE);
  set_arg(1, value);
  set_arg(2, value);
  value.set_extend(0, PL_RECORD_TYPE);
  set_arg(3, value);
  const std::string json = "{\"label\":\"a\\\"b\"}";
  const std::string cell = std::string(1, static_cast<char>(json.size())) + json;
  expect_payload(false, cell + cell + std::string(1, '\xfb'));
  expect_payload(true, std::string("\x00\x10", 2) + cell + cell);
  EXPECT_EQ(ObVarcharType, member.get_type());
  EXPECT_EQ(204, record.get_id());
}

TEST_F(PlCompositeOutputPackTest, lob_members_still_require_the_lob_read_service)
{
  ObObj member;
  member.set_lob_value(ObLongTextType, "x", 1);
  member.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ObPLCollection list(PL_NESTED_TABLE_TYPE, 205);
  list.set_data(&member, 1);
  list.set_count(1);
  ObObj value;
  value.set_extend(reinterpret_cast<int64_t>(&list), PL_NESTED_TABLE_TYPE);
  set_arg(1, value);
  set_arg(2, value);
  set_arg(3, value);
  for (int protocol : {0, 1}) {
    value.set_int(protocol);
    set_arg(0, value);
    ObDatum result;
    EXPECT_EQ(OB_NOT_INIT, ObExprOutputPack::eval_output_pack(*output_, eval_ctx_, result));
  }
  EXPECT_EQ(ObLongTextType, member.get_type());
  EXPECT_EQ(1, list.get_count());
}

} // namespace
} // namespace sql
} // namespace oceanbase
