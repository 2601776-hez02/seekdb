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
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_array_expr_utils.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/pl/ob_pl_user_type.h"

namespace oceanbase {
namespace sql {
using namespace common;
namespace {
class GeneratorTableScan final : public ITableFunctionScan
{
public:
  explicit GeneratorTableScan(const TableFunctionScanContext &context)
    : arguments_(context.arguments_), projection_(context.column_ids_), limit_(0), position_(0) {}
  int open() override { return rescan(arguments_); }
  int rescan(const ObIArray<ObObj> &arguments) override
  {
    int ret = OB_SUCCESS;
    position_ = 0;
    if (arguments.count() != 1 || arguments.at(0).is_null()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "generator requires a non-NULL integer");
    } else {
      limit_ = arguments.at(0).get_int();
      row_.cells_ = &value_;
      row_.count_ = projection_.count();
    }
    return ret;
  }
  int get_next_row(ObNewRow *&row) override
  {
    int ret = OB_SUCCESS;
    if (position_ >= limit_) {
      ret = OB_ITER_END;
    } else {
      value_.set_int(++position_);
      row = &row_;
    }
    return ret;
  }
  int close() override { position_ = 0; return OB_SUCCESS; }
private:
  const ObIArray<ObObj> &arguments_;
  const ObIArray<uint64_t> &projection_;
  int64_t limit_;
  int64_t position_;
  ObObj value_;
  ObNewRow row_;
};

class CollectionTableScan final : public ITableFunctionScan
{
  struct Input {
    Input() : array_(nullptr), collection_(nullptr), position_(0), columns_(0) {}
    ObIArrayType *array_;
    pl::ObPLCollection *collection_;
    int64_t position_;
    int64_t columns_;
  };
public:
  explicit CollectionTableScan(const TableFunctionScanContext &context)
    : exec_ctx_(context.exec_ctx_), arguments_(context.arguments_), projection_(context.column_ids_),
      column_counts_(context.argument_column_counts_), input_allocator_("TableFuncInput"),
      row_allocator_("TableFuncRow") {}
  int open() override { return rescan(arguments_); }
  int rescan(const ObIArray<ObObj> &arguments) override
  {
    int ret = OB_SUCCESS;
    inputs_.reuse();
    values_.reuse();
    cells_.reuse();
    input_allocator_.reuse();
    row_allocator_.reuse();
    if (arguments.count() != column_counts_.count()) { ret = OB_ERR_UNEXPECTED; }
    int64_t total_columns = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < arguments.count(); ++i) {
      Input input;
      input.columns_ = column_counts_.at(i);
      total_columns += input.columns_;
      const ObObj &argument = arguments.at(i);
      if (argument.is_null()) {
      } else if (argument.is_collection_sql_type()) {
        ObCollectionTypeBase *type = nullptr;
        ObString bytes = argument.get_string();
        OZ (ObArrayExprUtils::get_coll_type_by_subschema_id(&exec_ctx_, argument.get_udt_subschema_id(), type));
        OZ (ObArrayTypeObjFactory::construct(input_allocator_, *type, input.array_));
        OZ (ObTextStringHelper::read_real_string_data(exec_ctx_, &input_allocator_,
            ObCollectionSQLType, CS_TYPE_BINARY, true, bytes));
        OZ (input.array_->init(bytes));
      } else if (argument.is_pl_extend()) {
        auto *composite = reinterpret_cast<pl::ObPLComposite *>(argument.get_ext());
        if (composite == nullptr) { // typed NULL composite
        } else if (!composite->is_collection()) { ret = OB_ERR_INVALID_TYPE_FOR_ARGUMENT; }
        else if (!composite->is_null()) { input.collection_ = static_cast<pl::ObPLCollection *>(composite); }
      } else { ret = OB_ERR_INVALID_TYPE_FOR_ARGUMENT; }
      OZ (inputs_.push_back(input));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < total_columns; ++i) { OZ (values_.push_back(ObObj())); }
    for (int64_t i = 0; OB_SUCC(ret) && i < projection_.count(); ++i) { OZ (cells_.push_back(ObObj())); }
    return ret;
  }
  int get_next_row(ObNewRow *&row) override
  {
    int ret = OB_SUCCESS;
    bool any = false;
    row_allocator_.reuse();
    for (int64_t i = 0; i < values_.count(); ++i) { values_.at(i).set_null(); }
    int64_t offset = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < inputs_.count(); ++i) {
      Input &input = inputs_.at(i);
      if (input.array_ != nullptr && input.position_ < input.array_->size()) {
        any = true;
        const int64_t position = input.position_++;
        if (!input.array_->is_null(position)) {
          if (input.array_->get_format() == Nested_Array) {
            const ObCollectionArrayType *type = static_cast<const ObCollectionArrayType *>(input.array_->get_array_type());
            ObIArrayType *child = nullptr;
            ObString bytes;
            OZ (ObArrayTypeObjFactory::construct(row_allocator_, *type->element_type_, child));
            OZ (input.array_->at(position, *child));
            OZ (ObArrayExprUtils::set_array_res(child, child->get_raw_binary_len(), row_allocator_, bytes));
            if (OB_SUCC(ret)) {
              values_.at(offset).set_varchar(bytes);
              values_.at(offset).set_type(ObCollectionSQLType);
              values_.at(offset).set_has_lob_header();
            }
          } else { OZ (input.array_->elem_at(position, values_.at(offset))); }
        }
      } else if (input.collection_ != nullptr && input.collection_->is_inited()) {
        pl::ObPLCollection &collection = *input.collection_;
        ObObj *elements = static_cast<ObObj *>(collection.get_data());
        // Deleted PL collection elements are holes, not NULL elements.
        while (input.position_ < collection.get_count() &&
               elements[input.position_].get_type() == ObMaxType) { ++input.position_; }
        if (input.position_ < collection.get_count()) {
          any = true;
          const ObObj &element = elements[input.position_++];
          if (element.is_null() || (element.is_pl_extend() && element.get_ext() == 0)) {
          } else if (element.is_pl_extend() && element.get_ext() != 0 &&
                     reinterpret_cast<pl::ObPLComposite *>(element.get_ext())->is_record()) {
            auto *record = reinterpret_cast<pl::ObPLRecord *>(element.get_ext());
            if (!record->is_null() && record->get_count() != input.columns_) { ret = OB_ERR_UNEXPECTED; }
            for (int64_t j = 0; OB_SUCC(ret) && !record->is_null() && record->is_inited() && j < input.columns_; ++j) {
              OZ (record->get_element(j, values_.at(offset + j)));
            }
          } else if (input.columns_ == 1) {
            values_.at(offset) = element;
          } else { ret = OB_ERR_UNEXPECTED; }
        }
      }
      offset += input.columns_;
    }
    if (OB_SUCC(ret) && !any) { ret = OB_ITER_END; }
    for (int64_t i = 0; OB_SUCC(ret) && i < projection_.count(); ++i) {
      const int64_t position = projection_.at(i) - OB_APP_MIN_COLUMN_ID;
      if (position < 0 || position >= values_.count()) { ret = OB_ERR_UNEXPECTED; }
      else { cells_.at(i) = values_.at(position); }
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
    inputs_.reuse();
    input_allocator_.reuse();
    row_allocator_.reuse();
    return OB_SUCCESS;
  }
private:
  ObExecContext &exec_ctx_;
  const ObIArray<ObObj> &arguments_;
  const ObIArray<uint64_t> &projection_;
  const ObIArray<int64_t> &column_counts_;
  ObArenaAllocator input_allocator_;
  ObArenaAllocator row_allocator_;
  ObSEArray<Input, 4> inputs_;
  ObSEArray<ObObj, 8> values_;
  ObSEArray<ObObj, 8> cells_;
  ObNewRow row_;
};
}

int create_generator_table_scan(const TableFunctionScanContext &context, ITableFunctionScan *&scan)
{
  void *memory = context.allocator_.alloc(sizeof(GeneratorTableScan));
  if (memory == nullptr) { return OB_ALLOCATE_MEMORY_FAILED; }
  scan = new (memory) GeneratorTableScan(context);
  return OB_SUCCESS;
}

int create_collection_table_scan(const TableFunctionScanContext &context, ITableFunctionScan *&scan)
{
  void *memory = context.allocator_.alloc(sizeof(CollectionTableScan));
  if (memory == nullptr) { return OB_ALLOCATE_MEMORY_FAILED; }
  scan = new (memory) CollectionTableScan(context);
  return OB_SUCCESS;
}
} // namespace sql
} // namespace oceanbase
