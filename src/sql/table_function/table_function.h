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

#pragma once

#include "common/object/ob_object.h"
#include "common/row/ob_row.h"
#include "lib/container/ob_iarray.h"

struct _ParseNode;
namespace oceanbase {
namespace sql {
class ObDMLResolver;
class ObExecContext;
class ObExpr;
class ObSQLSessionInfo;
struct TableItem;
struct TableFunctionDescriptor;

struct TableFunctionColumn
{
  const char *name_;
  common::ObDataType type_;
  uint64_t id_;
  bool nullable_ = true;
};

// All parser forms are adapted to this call before registry lookup. Options are
// a structured parser subtree owned by the statement arena (e.g. JSON COLUMNS).
struct TableFunctionCall
{
  common::ObString name_;
  const _ParseNode *arguments_ = nullptr;
  const _ParseNode *options_ = nullptr;
  const _ParseNode *alias_ = nullptr;
  const _ParseNode *column_aliases_ = nullptr;
  const TableFunctionDescriptor *descriptor_ = nullptr;
};

struct TableFunctionLookup
{
  uint64_t column_id_;
  common::ObObj value_;
};

struct TableFunctionScanContext
{
  ObExecContext &exec_ctx_;
  common::ObIAllocator &allocator_;
  const TableFunctionDescriptor &descriptor_;
  const common::ObIArray<uint64_t> &column_ids_;
  const common::ObIArray<common::ObObj> &arguments_;
  const common::ObIArray<int64_t> &argument_column_counts_;
  const common::ObIArray<TableFunctionLookup> &lookups_;
  const common::ObString &options_;
  const common::ObIArray<ObExpr *> &argument_exprs_;
  const common::ObIArray<ObExpr *> &column_exprs_;
};

class ITableFunctionScan
{
public:
  virtual ~ITableFunctionScan() = default;
  virtual int open() = 0;
  virtual int get_next_row(common::ObNewRow *&row) = 0;
  virtual int rescan(const common::ObIArray<common::ObObj> &arguments) = 0;
  virtual int close() = 0;
};

using TableFunctionBind = int (*)(ObDMLResolver &, const TableFunctionCall &, TableItem *&);
using TableFunctionCompileOptions = int (*)(const TableItem &, common::ObIAllocator &, common::ObString &);
using TableFunctionScanFactory = int (*)(const TableFunctionScanContext &, ITableFunctionScan *&);

struct TableFunctionDescriptor
{
  uint64_t id_;
  const char *name_;
  const char *signature_;
  const char *description_;
  int64_t min_arguments_;
  int64_t max_arguments_; // -1 means variadic
  const TableFunctionColumn *columns_;
  int64_t column_count_; // zero for a bind-dependent output
  TableFunctionBind bind_;
  TableFunctionScanFactory scan_factory_;
  const char *required_database_ = nullptr;
  const uint64_t *lookup_column_ids_ = nullptr;
  int64_t lookup_column_count_ = 0;
  TableFunctionCompileOptions compile_options_ = nullptr;
  bool requires_all_columns_ = false;
};

constexpr uint64_t UNNEST_FUNCTION_ID = 1;
constexpr uint64_t GENERATOR_FUNCTION_ID = 2;
constexpr uint64_t JSON_TABLE_FUNCTION_ID = 3;
constexpr uint64_t HYBRID_SEARCH_FUNCTION_ID = 4;
constexpr uint64_t READ_CSV_FUNCTION_ID = 5;
constexpr uint64_t READ_JSON_FUNCTION_ID = 6;
constexpr uint64_t READ_NDJSON_FUNCTION_ID = 7;
constexpr uint64_t READ_PARQUET_FUNCTION_ID = 8;
constexpr uint64_t GLOB_FUNCTION_ID = 9;

int compile_json_table_options(const TableItem &, common::ObIAllocator &, common::ObString &);
int create_json_table_scan(const TableFunctionScanContext &, ITableFunctionScan *&);

int create_collection_table_scan(const TableFunctionScanContext &, ITableFunctionScan *&);
int create_file_table_scan(const TableFunctionScanContext &, ITableFunctionScan *&);
int compile_file_table_options(const TableItem &, common::ObIAllocator &, common::ObString &);
int create_generator_table_scan(const TableFunctionScanContext &, ITableFunctionScan *&);

class TableFunctionRegistry
{
public:
  // Called once by observer startup before any SQL is served. Definitions live
  // for the process lifetime; registration never creates schema objects.
  static int register_functions(const TableFunctionDescriptor *definitions, int64_t count);
  static int get_overloads(const common::ObString &name,
      common::ObIArray<const TableFunctionDescriptor *> &definitions);
  static int match(const common::ObString &name, int64_t argument_count,
      const TableFunctionDescriptor *&definition);
  static const TableFunctionDescriptor *find(uint64_t id);
  static int get_all(common::ObIArray<const TableFunctionDescriptor *> &definitions);
};

// Fixed-output providers share the same binder; their execution factory only
// receives evaluated values and projected column IDs, never their own schema.
int bind_fixed_table_function(ObDMLResolver &, const TableFunctionCall &, TableItem *&);


} // namespace sql
} // namespace oceanbase
