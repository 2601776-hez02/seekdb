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

#define USING_LOG_PREFIX COMMON
#include "observer/virtual_table/ob_virtual_table_projector.h"
namespace oceanbase
{
namespace common
{
int ObVirtualTableProjector::project_row(const ObIArray<Column> &columns, ObNewRow &row, const bool full_columns)
{
  int ret = OB_SUCCESS;
  if (columns.count() < 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (output_column_ids_.count() > row.count_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(row.cells_) && row.count_ != 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(row.cells_)) {
    // just skip, won't project row
    // column not specified, eg: count(*)
  } else {
    int64_t cell_idx = 0;
    FOREACH_CNT_X(column_id, output_column_ids_, OB_SUCCESS == ret) {
      bool find = false;
      ObObj column_value = ObObj::make_nop_obj();
      FOREACH_CNT_X(column, columns, !find) {
        if (*column_id == column->column_id_) {
          column_value = column->column_value_;
          find = true;
        }
      }
      if (!find && full_columns) {
        ret = OB_ENTRY_NOT_EXIST;
      } else {
        row.cells_[cell_idx] = column_value;
        ++cell_idx;
      }
    }
  }
  return ret;
}

int ObVirtualTableProjector::check_column_exist(const share::schema::ObTableSchema *table,
                                                const char* column_name,
                                                bool &exist) const
{
  int ret = OB_SUCCESS;
  const share::schema::ObColumnSchemaV2 *column_schema = NULL;
  exist = false;
  if (OB_ISNULL(table) || OB_ISNULL(column_name)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(column_schema = table->get_column_schema(column_name))) {
    ret = OB_SCHEMA_ERROR;
  } else {
    uint64_t table_column_id = column_schema->get_column_id();
    FOREACH_CNT_X(column_id, output_column_ids_, OB_SUCCESS == ret) {
      if (*column_id == table_column_id) {
        exist = true;
        break;
      }
    }
  }
  return ret;
}
}//end namespace common
}//end namespace oceanbase
