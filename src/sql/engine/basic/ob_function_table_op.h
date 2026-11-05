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

#ifndef OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_
#define OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_chunk_datum_store.h"
#include "lib/charset/ob_charset.h"
#include "sql/table_function/table_function.h"

namespace oceanbase
{
namespace sql
{

class ObExpr;
class ObFunctionTableSpec : public ObOpSpec
{
OB_UNIS_VERSION_V(1);
public:
  ObFunctionTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type), value_expr_(nullptr), column_exprs_(alloc), has_correlated_expr_(false),
      function_id_(0), argument_exprs_(alloc), column_ids_(alloc), argument_column_counts_(alloc), lookup_exprs_(alloc), lookup_column_ids_(alloc)
  {}
  ObExpr *value_expr_;
  common::ObFixedArray<ObExpr*, common::ObIAllocator> column_exprs_;
  bool has_correlated_expr_;
  uint64_t function_id_;
  common::ObString options_;
  common::ObFixedArray<ObExpr*, common::ObIAllocator> argument_exprs_;
  common::ObFixedArray<uint64_t, common::ObIAllocator> column_ids_;
  common::ObFixedArray<int64_t, common::ObIAllocator> argument_column_counts_;
  common::ObFixedArray<ObExpr*, common::ObIAllocator> lookup_exprs_;
  common::ObFixedArray<uint64_t, common::ObIAllocator> lookup_column_ids_;
};

class ObFunctionTableOp : public ObOperator
{
public:
  ObFunctionTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
    scan_(nullptr), opened_(false), arguments_()
  {}

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  //virtual int inner_get_next_batch(int64_t max_row_cnt) override;
  virtual int inner_close() override;
  virtual void destroy() override;
private:
  int open_scan();
  int evaluate_arguments();
  ITableFunctionScan *scan_;
  bool opened_;
  common::ObSEArray<common::ObObj, 4> arguments_;
  common::ObSEArray<TableFunctionLookup, 4> lookups_;

};

} // end namespace sql
} // end namespace oceanbase

#endif /* OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_ */
