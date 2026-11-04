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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/table_function/table_function.h"
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/parser/parse_node.h"
#include "lib/charset/ob_charset.h"

namespace oceanbase {
namespace sql {
using namespace common;
namespace {
const TableFunctionDescriptor *system_definitions = nullptr;
int64_t system_definition_count = 0;
int bind_unnest(ObDMLResolver &resolver, const TableFunctionCall &call, TableItem *&item)
{ return resolver.bind_unnest_table_function(call, item); }
int bind_generator(ObDMLResolver &resolver, const TableFunctionCall &call, TableItem *&item)
{ return resolver.bind_generator_table_function(call, item); }
int bind_json_table(ObDMLResolver &resolver, const TableFunctionCall &call, TableItem *&item)
{ return resolver.bind_json_table_function(call, item); }
int bind_hybrid(ObDMLResolver &resolver, const TableFunctionCall &call, TableItem *&item)
{ return resolver.bind_hybrid_table_function(call, item); }
int bind_file(ObDMLResolver &resolver, const TableFunctionCall &call, TableItem *&item)
{ return resolver.bind_file_table_function(call, item); }
TableFunctionColumn generator_column()
{
  TableFunctionColumn result;
  result.name_ = "generator";
  result.id_ = OB_APP_MIN_COLUMN_ID;
  result.type_.set_int();
  result.nullable_ = false;
  return result;
}
const TableFunctionColumn GENERATOR_COLUMNS[] = {generator_column()};
const TableFunctionDescriptor BUILTINS[] = {
  {UNNEST_FUNCTION_ID, "unnest", "unnest(collection, ...)",
   "Each collection element becomes a row; records supply top-level fields; inputs zip with NULL padding.",
   1, -1, nullptr, 0, bind_unnest, create_collection_table_scan},
  {GENERATOR_FUNCTION_ID, "generator", "generator(n)", "Integers from 1 to n.",
   1, 1, GENERATOR_COLUMNS, 1, bind_generator, create_generator_table_scan},
  {JSON_TABLE_FUNCTION_ID, "json_table", "json_table(document, path COLUMNS (...))",
   "Columns are declared by COLUMNS.", 0, -1, nullptr, 0, bind_json_table, create_json_table_scan, nullptr, nullptr, 0, compile_json_table_options, true},
  {HYBRID_SEARCH_FUNCTION_ID, "hybrid_search", "hybrid_search(table_name, configuration)",
   "Columns come from the bound search query.", 2, 2, nullptr, 0, bind_hybrid, nullptr},
  {READ_CSV_FUNCTION_ID, "read_csv", "read_csv(path [, option = value, ...])",
   "Reads one local CSV file; columns and types are inferred from the whole file.",
   1, -1, nullptr, 0, bind_file, create_file_table_scan, nullptr, nullptr, 0,
   compile_file_table_options},
  {READ_JSON_FUNCTION_ID, "read_json", "read_json(path [, option = value, ...])",
   "Reads one local JSON document file with one flat object per line.",
   1, -1, nullptr, 0, bind_file, create_file_table_scan, nullptr, nullptr, 0,
   compile_file_table_options},
  {READ_NDJSON_FUNCTION_ID, "read_ndjson", "read_ndjson(path [, option = value, ...])",
   "Newline delimited JSON; the same reader as read_json.",
   1, -1, nullptr, 0, bind_file, create_file_table_scan, nullptr, nullptr, 0,
   compile_file_table_options},
  {READ_PARQUET_FUNCTION_ID, "read_parquet", "read_parquet(path)",
   "Reads one local Parquet file; columns come from the file footer schema.",
   1, 1, nullptr, 0, bind_file, create_file_table_scan, nullptr, nullptr, 0,
   compile_file_table_options},
  {GLOB_FUNCTION_ID, "glob", "glob(pattern)",
   "Lists the local files matching a glob pattern; the only column is file.",
   1, 1, nullptr, 0, bind_file, create_file_table_scan, nullptr, nullptr, 0,
   compile_file_table_options},
};
}

int bind_fixed_table_function(ObDMLResolver &resolver, const TableFunctionCall &call, TableItem *&item)
{ return resolver.bind_fixed_table_function(call, item); }

int TableFunctionRegistry::register_functions(const TableFunctionDescriptor *definitions, int64_t count)
{
  int ret = OB_SUCCESS;
  if (system_definitions == definitions && definitions != nullptr && system_definition_count == count) { return OB_SUCCESS; }
  if (system_definitions != nullptr || definitions == nullptr || count < 0) {
    ret = OB_INVALID_ARGUMENT;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    const TableFunctionDescriptor &entry = definitions[i];
    if (entry.name_ == nullptr || entry.name_[0] == 0 || entry.bind_ == nullptr ||
        entry.signature_ == nullptr || entry.description_ == nullptr ||
        entry.min_arguments_ < 0 || entry.max_arguments_ < -1 ||
        (entry.max_arguments_ >= 0 && entry.max_arguments_ < entry.min_arguments_) ||
        entry.column_count_ < 0 || find(entry.id_) != nullptr) { ret = OB_INVALID_ARGUMENT; }
    for (const auto &builtin : BUILTINS) {
      if (OB_SUCC(ret) && ObCharset::case_insensitive_equal(
          ObString::make_string(entry.name_), ObString::make_string(builtin.name_)) &&
          STRCMP(entry.signature_, builtin.signature_) == 0) { ret = OB_INVALID_ARGUMENT; }
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < i; ++j) {
      if (entry.id_ == definitions[j].id_ ||
          (ObCharset::case_insensitive_equal(ObString::make_string(entry.name_),
           ObString::make_string(definitions[j].name_)) &&
           STRCMP(entry.signature_, definitions[j].signature_) == 0)) {
        ret = OB_INVALID_ARGUMENT;
      }
    }
    for (int64_t c = 0; OB_SUCC(ret) && c < entry.column_count_; ++c) {
      if (entry.columns_ == nullptr || entry.columns_[c].name_ == nullptr || entry.columns_[c].name_[0] == 0) {
        ret = OB_INVALID_ARGUMENT;
      }
      for (int64_t d = 0; OB_SUCC(ret) && d < c; ++d) {
        if (entry.columns_[c].id_ == entry.columns_[d].id_ || ObCharset::case_insensitive_equal(
            ObString::make_string(entry.columns_[c].name_), ObString::make_string(entry.columns_[d].name_))) {
          ret = OB_INVALID_ARGUMENT;
        }
      }
    }
  }
  if (OB_SUCC(ret)) { system_definitions = definitions; system_definition_count = count; }
  return ret;
}

int TableFunctionRegistry::get_overloads(const ObString &name,
    ObIArray<const TableFunctionDescriptor *> &definitions)
{
  int ret = OB_SUCCESS;
  for (const auto &entry : BUILTINS) {
    if (OB_SUCC(ret) && ObCharset::case_insensitive_equal(name, ObString::make_string(entry.name_))) {
      OZ (definitions.push_back(&entry));
    }
  }
  const int64_t count = system_definition_count;
  const TableFunctionDescriptor *system = system_definitions;
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    if (ObCharset::case_insensitive_equal(name, ObString::make_string(system[i].name_))) {
      OZ (definitions.push_back(system + i));
    }
  }
  return ret;
}

int TableFunctionRegistry::match(const ObString &name, int64_t argument_count,
    const TableFunctionDescriptor *&definition)
{
  int ret = OB_SUCCESS;
  definition = nullptr;
  ObSEArray<const TableFunctionDescriptor *, 4> candidates;
  OZ (get_overloads(name, candidates));
  if (OB_SUCC(ret) && candidates.empty()) {
    ret = OB_ERR_FUNCTION_UNKNOWN;
    LOG_USER_ERROR(OB_ERR_FUNCTION_UNKNOWN, "TABLE FUNCTION", name.length(), name.ptr());
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < candidates.count(); ++i) {
    const TableFunctionDescriptor &candidate = *candidates.at(i);
    if (argument_count >= candidate.min_arguments_ &&
        (candidate.max_arguments_ == -1 || argument_count <= candidate.max_arguments_)) {
      if (definition == nullptr) { definition = &candidate; }
      else {
        ret = OB_ERR_FUNC_DUP;
        definition = nullptr;
        LOG_USER_ERROR(OB_ERR_FUNC_DUP, name.length(), name.ptr());
      }
    }
  }
  if (OB_SUCC(ret) && definition == nullptr) {
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, name.length(), name.ptr());
  }
  return ret;
}

const TableFunctionDescriptor *TableFunctionRegistry::find(uint64_t id)
{
  const TableFunctionDescriptor *result = nullptr;
  for (const auto &entry : BUILTINS) {
    if (id == entry.id_) { result = &entry; break; }
  }
  const int64_t count = system_definition_count;
  const TableFunctionDescriptor *system = system_definitions;
  for (int64_t i = 0; result == nullptr && i < count; ++i) {
    if (id == system[i].id_) { result = &system[i]; }
  }
  return result;
}

int TableFunctionRegistry::get_all(ObIArray<const TableFunctionDescriptor *> &definitions)
{
  int ret = OB_SUCCESS;
  for (const auto &entry : BUILTINS) { OZ (definitions.push_back(&entry)); }
  const int64_t count = system_definition_count;
  const TableFunctionDescriptor *system = system_definitions;
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) { OZ (definitions.push_back(system + i)); }
  return ret;
}
} // namespace sql
} // namespace oceanbase
