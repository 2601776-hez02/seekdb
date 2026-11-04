/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#include <gtest/gtest.h>
#include "sql/table_function/table_function.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/pl/ob_pl_user_type.h"

namespace oceanbase {
namespace sql {
namespace {
using namespace common;
using namespace pl;

TEST(TableFunctionRegistryTest, arity_overloads_are_discoverable_and_ambiguity_is_an_error)
{
  static const TableFunctionDescriptor definitions[] = {
    {900001, "registry_test", "registry_test()", "zero inputs", 0, 0,
      nullptr, 0, bind_fixed_table_function, create_generator_table_scan},
    {900002, "registry_test", "registry_test(value)", "one input", 1, 1,
      nullptr, 0, bind_fixed_table_function, create_generator_table_scan},
    {900003, "registry_test", "registry_test(a, b, ...)", "variadic", 2, -1,
      nullptr, 0, bind_fixed_table_function, create_generator_table_scan},
    {900004, "registry_test", "registry_test(a, b, c)", "ambiguous at three", 3, 3,
      nullptr, 0, bind_fixed_table_function, create_generator_table_scan}
  };
  ASSERT_EQ(OB_SUCCESS, TableFunctionRegistry::register_functions(definitions, 4));
  EXPECT_EQ(OB_SUCCESS, TableFunctionRegistry::register_functions(definitions, 4));
  ObSEArray<const TableFunctionDescriptor *, 4> overloads;
  ASSERT_EQ(OB_SUCCESS, TableFunctionRegistry::get_overloads(ObString("REGISTRY_TEST"), overloads));
  ASSERT_EQ(4, overloads.count());
  const TableFunctionDescriptor *matched = nullptr;
  ASSERT_EQ(OB_SUCCESS, TableFunctionRegistry::match(ObString("registry_test"), 0, matched));
  EXPECT_EQ(&definitions[0], matched);
  ASSERT_EQ(OB_SUCCESS, TableFunctionRegistry::match(ObString("registry_test"), 1, matched));
  EXPECT_EQ(&definitions[1], matched);
  ASSERT_EQ(OB_SUCCESS, TableFunctionRegistry::match(ObString("registry_test"), 20, matched));
  EXPECT_EQ(&definitions[2], matched);
  EXPECT_EQ(OB_ERR_FUNC_DUP, TableFunctionRegistry::match(ObString("registry_test"), 3, matched));
  EXPECT_EQ(nullptr, matched);
  EXPECT_EQ(OB_ERR_PARAM_SIZE, TableFunctionRegistry::match(ObString("generator"), 2, matched));
  EXPECT_EQ(OB_ERR_FUNCTION_UNKNOWN, TableFunctionRegistry::match(ObString("unregistered"), 0, matched));
  ObSEArray<const TableFunctionDescriptor *, 16> all;
  ASSERT_EQ(OB_SUCCESS, TableFunctionRegistry::get_all(all));
  // Built-in providers: unnest, generator, json_table, hybrid_search plus the
  // five file SQL providers (read_csv, read_json, read_ndjson, read_parquet,
  // glob).
  EXPECT_EQ(13, all.count());
  const TableFunctionDescriptor *file_provider = nullptr;
  ASSERT_EQ(OB_SUCCESS, TableFunctionRegistry::match(ObString("read_csv"), 1, file_provider));
  EXPECT_EQ(READ_CSV_FUNCTION_ID, file_provider->id_);
  ASSERT_EQ(OB_SUCCESS, TableFunctionRegistry::match(ObString("read_csv"), 3, file_provider));
  EXPECT_EQ(READ_CSV_FUNCTION_ID, file_provider->id_);
  ASSERT_EQ(OB_SUCCESS, TableFunctionRegistry::match(ObString("read_json"), 1, file_provider));
  EXPECT_EQ(READ_JSON_FUNCTION_ID, file_provider->id_);
  ASSERT_EQ(OB_SUCCESS, TableFunctionRegistry::match(ObString("read_ndjson"), 1, file_provider));
  EXPECT_EQ(READ_NDJSON_FUNCTION_ID, file_provider->id_);
  ASSERT_EQ(OB_SUCCESS, TableFunctionRegistry::match(ObString("read_parquet"), 1, file_provider));
  EXPECT_EQ(READ_PARQUET_FUNCTION_ID, file_provider->id_);
  ASSERT_EQ(OB_SUCCESS, TableFunctionRegistry::match(ObString("glob"), 1, file_provider));
  EXPECT_EQ(GLOB_FUNCTION_ID, file_provider->id_);
  EXPECT_EQ(OB_ERR_PARAM_SIZE, TableFunctionRegistry::match(ObString("read_parquet"), 2, file_provider));
  EXPECT_EQ(OB_ERR_PARAM_SIZE, TableFunctionRegistry::match(ObString("glob"), 2, file_provider));
}

class TableFunctionScanTest : public ::testing::Test
{
protected:
  TableFunctionScanTest() : exec_ctx_(allocator_), scan_(nullptr) {}
  void TearDown() override
  {
    if (scan_ != nullptr) {
      EXPECT_EQ(OB_SUCCESS, scan_->close());
      scan_->~ITableFunctionScan();
    }
  }
  ObObj composite(ObPLComposite &value)
  {
    ObObj obj;
    obj.set_extend(reinterpret_cast<int64_t>(&value), value.get_type());
    return obj;
  }
  void set_elements(ObPLCollection &collection, ObObj *values, int64_t size)
  {
    collection.set_data(values, size);
    collection.set_count(size);
  }
  int open(uint64_t id = UNNEST_FUNCTION_ID)
  {
    const TableFunctionDescriptor *descriptor = TableFunctionRegistry::find(id);
    if (descriptor == nullptr) { return OB_ERR_UNEXPECTED; }
    const TableFunctionScanContext context{exec_ctx_, allocator_, *descriptor, projection_,
        arguments_, column_counts_, lookups_, options_, expressions_, expressions_};
    int ret = descriptor->scan_factory_(context, scan_);
    if (OB_SUCCESS == ret) { ret = scan_->open(); }
    return ret;
  }
  void project(std::initializer_list<int64_t> positions)
  {
    for (int64_t position : positions) {
      ASSERT_EQ(OB_SUCCESS, projection_.push_back(OB_APP_MIN_COLUMN_ID + position));
    }
  }
  ObArenaAllocator allocator_;
  ObExecContext exec_ctx_;
  ITableFunctionScan *scan_;
  ObSEArray<ObObj, 4> arguments_;
  ObSEArray<int64_t, 4> column_counts_;
  ObSEArray<uint64_t, 8> projection_;
  ObSEArray<TableFunctionLookup, 1> lookups_;
  ObSEArray<ObExpr *, 1> expressions_;
  ObString options_;
};

TEST_F(TableFunctionScanTest, null_empty_and_uninitialized_collections_produce_no_rows)
{
  ObPLCollection values(PL_NESTED_TABLE_TYPE, 100);
  ASSERT_EQ(OB_SUCCESS, arguments_.push_back(composite(values)));
  ASSERT_EQ(OB_SUCCESS, column_counts_.push_back(1));
  project({0});
  ASSERT_EQ(OB_SUCCESS, open());
  ObNewRow *row = nullptr;
  EXPECT_EQ(OB_ITER_END, scan_->get_next_row(row));
  values.set_inited();
  ASSERT_EQ(OB_SUCCESS, scan_->rescan(arguments_));
  EXPECT_EQ(OB_ITER_END, scan_->get_next_row(row));
  values.set_null();
  ASSERT_EQ(OB_SUCCESS, scan_->rescan(arguments_));
  EXPECT_EQ(OB_ITER_END, scan_->get_next_row(row));
  // A bound, typed NULL argument has the same column contract as a collection.
  arguments_.at(0).set_null();
  ASSERT_EQ(OB_SUCCESS, scan_->rescan(arguments_));
  EXPECT_EQ(OB_ITER_END, scan_->get_next_row(row));
  arguments_.at(0).set_extend(0, PL_NESTED_TABLE_TYPE);
  ASSERT_EQ(OB_SUCCESS, scan_->rescan(arguments_));
  EXPECT_EQ(OB_ITER_END, scan_->get_next_row(row));
}

TEST_F(TableFunctionScanTest, zip_skips_deleted_slots_but_preserves_null_elements)
{
  ObObj left_values[4];
  left_values[0].set_int(1);
  left_values[1].set_type(ObMaxType);
  left_values[2].set_null();
  left_values[3].set_int(3);
  ObPLCollection left(PL_NESTED_TABLE_TYPE, 101);
  set_elements(left, left_values, 4);
  ObObj right_value;
  right_value.set_int(9);
  ObPLCollection right(PL_VARRAY_TYPE, 102);
  set_elements(right, &right_value, 1);
  ASSERT_EQ(OB_SUCCESS, arguments_.push_back(composite(left)));
  ASSERT_EQ(OB_SUCCESS, arguments_.push_back(composite(right)));
  ASSERT_EQ(OB_SUCCESS, column_counts_.push_back(1));
  ASSERT_EQ(OB_SUCCESS, column_counts_.push_back(1));
  project({0, 1});
  ASSERT_EQ(OB_SUCCESS, open());
  ObNewRow *row = nullptr;
  ASSERT_EQ(OB_SUCCESS, scan_->get_next_row(row));
  ASSERT_EQ(2, row->count_);
  EXPECT_EQ(1, row->cells_[0].get_int());
  EXPECT_EQ(9, row->cells_[1].get_int());
  ASSERT_EQ(OB_SUCCESS, scan_->get_next_row(row));
  EXPECT_TRUE(row->cells_[0].is_null());
  EXPECT_TRUE(row->cells_[1].is_null());
  ASSERT_EQ(OB_SUCCESS, scan_->get_next_row(row));
  EXPECT_EQ(3, row->cells_[0].get_int());
  EXPECT_TRUE(row->cells_[1].is_null());
  EXPECT_EQ(OB_ITER_END, scan_->get_next_row(row));
  // Rescan observes the new invocation's values and resets both input positions.
  left_values[0].set_int(5);
  ASSERT_EQ(OB_SUCCESS, scan_->rescan(arguments_));
  ASSERT_EQ(OB_SUCCESS, scan_->get_next_row(row));
  EXPECT_EQ(5, row->cells_[0].get_int());
  EXPECT_EQ(9, row->cells_[1].get_int());
  EXPECT_EQ(ObMaxType, left_values[1].get_type());
}

TEST_F(TableFunctionScanTest, records_expand_once_and_preserve_nested_composite_cells)
{
  ObObj nested_member;
  nested_member.set_int(7);
  ObPLRecord nested_record;
  nested_record.set_count(1);
  nested_record.set_data(&nested_member);
  ObPLCollection nested_list(PL_VARRAY_TYPE, 103);
  set_elements(nested_list, &nested_member, 1);
  ObObj members[3];
  members[0].set_int(42);
  members[1] = composite(nested_record);
  members[2] = composite(nested_list);
  ObPLRecord record;
  record.set_count(3);
  record.set_data(members);
  ObPLRecord null_record;
  null_record.set_count(3);
  null_record.set_null();
  ObObj elements[4] = {composite(record), composite(null_record), ObObj(), ObObj()};
  elements[2].set_null();
  elements[3].set_extend(0, PL_RECORD_TYPE);
  ObPLCollection records(PL_NESTED_TABLE_TYPE, 104);
  set_elements(records, elements, 4);
  ASSERT_EQ(OB_SUCCESS, arguments_.push_back(composite(records)));
  ASSERT_EQ(OB_SUCCESS, column_counts_.push_back(3));
  project({0, 1, 2});
  ASSERT_EQ(OB_SUCCESS, open());
  ObNewRow *row = nullptr;
  ASSERT_EQ(OB_SUCCESS, scan_->get_next_row(row));
  ASSERT_EQ(3, row->count_);
  EXPECT_EQ(42, row->cells_[0].get_int());
  EXPECT_EQ(members[1].get_ext(), row->cells_[1].get_ext());
  EXPECT_EQ(PL_RECORD_TYPE, row->cells_[1].get_meta().get_extend_type());
  EXPECT_EQ(members[2].get_ext(), row->cells_[2].get_ext());
  EXPECT_EQ(PL_VARRAY_TYPE, row->cells_[2].get_meta().get_extend_type());
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(OB_SUCCESS, scan_->get_next_row(row));
    ASSERT_EQ(3, row->count_);
    for (int j = 0; j < 3; ++j) { EXPECT_TRUE(row->cells_[j].is_null()); }
  }
  EXPECT_EQ(OB_ITER_END, scan_->get_next_row(row));
}

TEST_F(TableFunctionScanTest, nested_collection_is_one_cell_and_projection_uses_bound_order)
{
  ObObj scalar;
  scalar.set_int(7);
  ObPLCollection inner(PL_VARRAY_TYPE, 105);
  set_elements(inner, &scalar, 1);
  ObObj nested_value = composite(inner);
  ObPLCollection outer(PL_NESTED_TABLE_TYPE, 106);
  set_elements(outer, &nested_value, 1);
  ObPLCollection other(PL_VARRAY_TYPE, 107);
  set_elements(other, &scalar, 1);
  ASSERT_EQ(OB_SUCCESS, arguments_.push_back(composite(outer)));
  ASSERT_EQ(OB_SUCCESS, arguments_.push_back(composite(other)));
  ASSERT_EQ(OB_SUCCESS, column_counts_.push_back(1));
  ASSERT_EQ(OB_SUCCESS, column_counts_.push_back(1));
  project({1, 0});
  ASSERT_EQ(OB_SUCCESS, open());
  ObNewRow *row = nullptr;
  ASSERT_EQ(OB_SUCCESS, scan_->get_next_row(row));
  ASSERT_EQ(2, row->count_);
  EXPECT_EQ(7, row->cells_[0].get_int());
  EXPECT_EQ(nested_value.get_ext(), row->cells_[1].get_ext());
  EXPECT_EQ(OB_ITER_END, scan_->get_next_row(row));
  projection_.reuse(); // COUNT(*) needs cardinality, without any output cells.
  ASSERT_EQ(OB_SUCCESS, scan_->rescan(arguments_));
  ASSERT_EQ(OB_SUCCESS, scan_->get_next_row(row));
  EXPECT_EQ(0, row->count_);
  EXPECT_EQ(OB_ITER_END, scan_->get_next_row(row));
}

TEST_F(TableFunctionScanTest, generator_rescan_and_early_close_do_not_keep_position)
{
  ObObj limit;
  limit.set_int(3);
  ASSERT_EQ(OB_SUCCESS, arguments_.push_back(limit));
  project({0});
  ASSERT_EQ(OB_SUCCESS, open(GENERATOR_FUNCTION_ID));
  ObNewRow *row = nullptr;
  ASSERT_EQ(OB_SUCCESS, scan_->get_next_row(row));
  EXPECT_EQ(1, row->cells_[0].get_int());
  ASSERT_EQ(OB_SUCCESS, scan_->close()); // LIMIT can stop before exhaustion.
  ASSERT_EQ(OB_SUCCESS, scan_->close());
  arguments_.at(0).set_int(1);
  ASSERT_EQ(OB_SUCCESS, scan_->rescan(arguments_));
  ASSERT_EQ(OB_SUCCESS, scan_->get_next_row(row));
  EXPECT_EQ(1, row->cells_[0].get_int());
  EXPECT_EQ(OB_ITER_END, scan_->get_next_row(row));
  for (int64_t limit : {0, -1}) {
    arguments_.at(0).set_int(limit);
    ASSERT_EQ(OB_SUCCESS, scan_->rescan(arguments_));
    EXPECT_EQ(OB_ITER_END, scan_->get_next_row(row));
  }
  arguments_.at(0).set_null();
  EXPECT_EQ(OB_INVALID_ARGUMENT, scan_->rescan(arguments_));
}

} // namespace
} // namespace sql
} // namespace oceanbase
