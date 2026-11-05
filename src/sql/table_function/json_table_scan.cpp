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

#define USING_LOG_PREFIX SQL_ENG
#include "sql/table_function/table_function.h"
#include "sql/engine/basic/ob_json_table_op.h"
#include "sql/resolver/dml/ob_dml_stmt.h"

namespace oceanbase {
namespace sql {
using namespace common;

int compile_json_table_options(const TableItem &table, ObIAllocator &allocator, ObString &options)
{
  int ret = OB_SUCCESS;
  if (table.json_table_def_ == nullptr) { ret = OB_ERR_UNEXPECTED; }
  ObSEArray<ObJtColInfo, 16> columns;
  int64_t length = 0;
  int64_t position = 0;
  int64_t output_position = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < table.json_table_def_->all_cols_.count(); ++i) {
    ObJtColInfo column;
    OZ (column.from_JtColBaseInfo(*table.json_table_def_->all_cols_.at(i)));
    if (column.col_type_ != NESTED_COL_TYPE) { column.output_column_idx_ = output_position++; }
    length += column.get_serialize_size();
    OZ (columns.push_back(column));
  }
  length += serialization::encoded_length_vi64(columns.count());
  char *buffer = nullptr;
  if (OB_SUCC(ret) && (buffer = static_cast<char *>(allocator.alloc(length))) == nullptr) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  OZ (serialization::encode_vi64(buffer, length, position, columns.count()));
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    OZ (columns.at(i).serialize(buffer, length, position));
  }
  if (OB_SUCC(ret)) { options.assign_ptr(buffer, position); }
  return ret;
}

namespace {
// This is an evaluator implementation owned by a function scan, not a second
// relation or optimizer path. The existing JSON path/default/nesting evaluator
// consumes expression slots compiled by the common function scan.
class JsonTableScan final : public ITableFunctionScan
{
public:
  explicit JsonTableScan(const TableFunctionScanContext &context)
    : context_(context), specification_(context.allocator_, PHY_JSON_TABLE),
      evaluator_(nullptr), eval_ctx_(context.exec_ctx_), initialized_(false), closed_(false) {}
  ~JsonTableScan() override
  {
    (void) close();
    if (evaluator_ != nullptr) { evaluator_->~ObJsonTableOp(); }
  }
  int open() override
  {
    int ret = OB_SUCCESS;
    int64_t position = 0;
    int64_t count = 0;
    const ObString &options = context_.options_;
    OZ (serialization::decode_vi64(options.ptr(), options.length(), position, &count));
    if (OB_SUCC(ret) && (count <= 0 || count > options.length() || context_.argument_exprs_.empty())) {
      ret = OB_ERR_UNEXPECTED;
    }
    OZ (specification_.cols_def_.init(count));
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      void *memory = context_.allocator_.alloc(sizeof(ObJtColInfo));
      if (memory == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
      else {
        ObJtColInfo *column = new (memory) ObJtColInfo();
        OZ (column->deserialize(options.ptr(), options.length(), position));
        OZ (specification_.cols_def_.push_back(column));
      }
    }
    OZ (specification_.value_exprs_.init(1));
    OZ (specification_.value_exprs_.push_back(context_.argument_exprs_.at(0)));
    OZ (specification_.column_exprs_.assign(context_.column_exprs_));
    OZ (specification_.emp_default_exprs_.assign(context_.argument_exprs_));
    OZ (specification_.err_default_exprs_.assign(context_.argument_exprs_));
    specification_.has_correlated_expr_ = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < context_.column_exprs_.count(); ++i) {
      OZ (cells_.push_back(ObObj()));
    }
    if (OB_SUCC(ret)) {
      void *memory = context_.allocator_.alloc(sizeof(ObJsonTableOp));
      if (memory == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
      else {
        evaluator_ = new (memory) ObJsonTableOp(context_.exec_ctx_, specification_, nullptr);
        OZ (evaluator_->inner_open());
        if (OB_SUCC(ret)) { initialized_ = true; }
      }
    }
    return ret;
  }
  int rescan(const ObIArray<ObObj> &arguments) override
  {
    UNUSED(arguments);
    return initialized_ ? evaluator_->inner_rescan() : OB_NOT_INIT;
  }
  int get_next_row(ObNewRow *&row) override
  {
    int ret = OB_SUCCESS;
    // The common scan owns argument evaluation, including correlated rescans.
    // Its per-row flag reset must not make the JSON evaluator execute the
    // document expression again. Feed the invocation's saved values back into
    // the evaluator's expression slots, including its default-value slots.
    CK (context_.arguments_.count() == context_.argument_exprs_.count());
    for (int64_t i = 0; OB_SUCC(ret) && i < context_.argument_exprs_.count(); ++i) {
      ObExpr *expression = context_.argument_exprs_.at(i);
      ObDatum &datum = expression->locate_datum_for_write(eval_ctx_);
      OZ (datum.from_obj(context_.arguments_.at(i), expression->obj_datum_map_));
      if (OB_SUCC(ret)) { expression->set_evaluated_projected(eval_ctx_); }
    }
    OZ (evaluator_->inner_get_next_row());
    for (int64_t i = 0; OB_SUCC(ret) && i < context_.column_exprs_.count(); ++i) {
      ObExpr *expression = context_.column_exprs_.at(i);
      const ObDatum &value = expression->locate_expr_datum(eval_ctx_);
      OZ (value.to_obj(cells_.at(i), expression->obj_meta_, expression->obj_datum_map_));
    }
    if (OB_SUCC(ret)) {
      row_.cells_ = cells_.get_data();
      row_.count_ = cells_.count();
      row = &row_;
    }
    return ret;
  }
  int close() override
  {
    int ret = OB_SUCCESS;
    if (!closed_ && evaluator_ != nullptr) { ret = evaluator_->inner_close(); }
    closed_ = true;
    return ret;
  }
private:
  TableFunctionScanContext context_;
  ObJsonTableSpec specification_;
  ObJsonTableOp *evaluator_;
  ObEvalCtx eval_ctx_;
  bool initialized_;
  bool closed_;
  ObSEArray<ObObj, 8> cells_;
  ObNewRow row_;
};
}

int create_json_table_scan(const TableFunctionScanContext &context, ITableFunctionScan *&scan)
{
  void *memory = context.allocator_.alloc(sizeof(JsonTableScan));
  if (memory == nullptr) { return OB_ALLOCATE_MEMORY_FAILED; }
  scan = new (memory) JsonTableScan(context);
  return OB_SUCCESS;
}
} // namespace sql
} // namespace oceanbase
