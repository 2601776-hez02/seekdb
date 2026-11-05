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

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_,
                    function_id_, argument_exprs_, column_ids_, argument_column_counts_, lookup_exprs_, lookup_column_ids_, options_);

int ObFunctionTableOp::inner_open()
{
  // Correlated expressions are only available when the first row is requested.
  opened_ = false;
  scan_ = nullptr;
  return OB_SUCCESS;
}

int ObFunctionTableOp::evaluate_arguments()
{
  int ret = OB_SUCCESS;
  arguments_.reuse();
  lookups_.reuse();
  for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.argument_exprs_.count(); ++i) {
    ObExpr *expr = MY_SPEC.argument_exprs_.at(i);
    ObDatum *datum = nullptr;
    ObObj value;
    OZ (expr->eval(eval_ctx_, datum));
    OZ (datum->to_obj(value, expr->obj_meta_, expr->obj_datum_map_));
    OZ (arguments_.push_back(value));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.lookup_exprs_.count(); ++i) {
    ObExpr *expr = MY_SPEC.lookup_exprs_.at(i);
    ObDatum *datum = nullptr;
    TableFunctionLookup lookup;
    lookup.column_id_ = MY_SPEC.lookup_column_ids_.at(i);
    OZ (expr->eval(eval_ctx_, datum));
    OZ (datum->to_obj(lookup.value_, expr->obj_meta_, expr->obj_datum_map_));
    // Equality with NULL has no matches; retaining the SQL filter is sufficient.
    if (OB_SUCC(ret) && !lookup.value_.is_null()) { OZ (lookups_.push_back(lookup)); }
  }
  return ret;
}

int ObFunctionTableOp::open_scan()
{
  int ret = OB_SUCCESS;
  const TableFunctionDescriptor *descriptor = TableFunctionRegistry::find(MY_SPEC.function_id_);
  if (OB_ISNULL(descriptor) || OB_ISNULL(descriptor->scan_factory_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(evaluate_arguments())) {
  } else {
    TableFunctionScanContext context{ctx_, ctx_.get_allocator(), *descriptor,
      MY_SPEC.column_ids_, arguments_, MY_SPEC.argument_column_counts_, lookups_,
      MY_SPEC.options_, MY_SPEC.argument_exprs_, MY_SPEC.column_exprs_};
    OZ (descriptor->scan_factory_(context, scan_));
    CK (scan_ != nullptr);
    OZ (scan_->open());
    if (OB_SUCC(ret)) { opened_ = true; }
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  OZ (ObOperator::inner_rescan());
  if (OB_SUCC(ret) && scan_ != nullptr) {
    clear_evaluated_flag();
    OZ (evaluate_arguments());
    OZ (scan_->rescan(arguments_));
    if (OB_SUCC(ret)) { opened_ = true; }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  OZ (ctx_.check_status());
  if (OB_SUCC(ret) && !opened_) { OZ (open_scan()); }
  ObNewRow *row = nullptr;
  OZ (scan_->get_next_row(row));
  if (OB_SUCC(ret) && (row == nullptr || row->count_ != MY_SPEC.column_exprs_.count())) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
    ObExpr *expr = MY_SPEC.column_exprs_.at(i);
    ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
    OZ (datum.from_obj(row->cells_[i], expr->obj_datum_map_));
    if (OB_SUCC(ret) && is_lob_storage(row->cells_[i].get_type())) {
      OZ (ob_adjust_lob_datum(ctx_, row->cells_[i], expr->obj_meta_, expr->obj_datum_map_,
                            ctx_.get_allocator(), datum));
    }
    if (OB_SUCC(ret)) { expr->set_evaluated_projected(eval_ctx_); }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  if (scan_ != nullptr) { ret = scan_->close(); }
  opened_ = false;
  return ret;
}

void ObFunctionTableOp::destroy()
{
  if (scan_ != nullptr) {
    (void) scan_->close();
    scan_->~ITableFunctionScan();
    scan_ = nullptr;
  }
  ObOperator::destroy();
}

} // namespace sql
} // namespace oceanbase
