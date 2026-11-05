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

#include <cstdint>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

// Local file formats understood by the file SQL table functions.
enum class FileFormat : int32_t
{
  CSV = 1,
  JSON = 2,
  NDJSON = 3,
  PARQUET = 4,
};

// Determinate logical type of one inferred output column.  The order encodes
// the widening direction: a merge of two types yields the later one, except for
// the pairs handled explicitly by merge_column_type().
enum class ColumnType : int32_t
{
  SQL_NULL = 0,
  BOOLEAN = 1,
  BIGINT = 2,
  DOUBLE = 3,
  DATE = 4,
  DATETIME = 5,
  VARCHAR = 6,
};

enum class Status : int32_t
{
  OK = 0,
  FILE_NOT_FOUND,
  OPEN_FAILED,
  PARSE_ERROR,
  INVALID_OPTION,
  UNSUPPORTED,
  CORRUPTED_FILE,
  TOO_LARGE,
};

// Hard limits that keep one scan bounded regardless of the input file.
constexpr int64_t MAX_FILE_COLUMNS = 1024;
constexpr int64_t MAX_RECORD_SIZE = 16LL * 1024 * 1024;
constexpr int64_t READ_CHUNK_SIZE = 256 * 1024;
constexpr int64_t MAX_GLOB_FILES = 65536;

struct Options
{
  FileFormat format_ = FileFormat::CSV;
  char delimiter_ = ',';
  char quote_ = '"';
  char escape_ = '"';
  bool header_ = true;
  std::string null_string_;
  int64_t skip_rows_ = 0;
  // -1 means "infer from the whole file", the only supported value so far.
  int64_t sample_size_ = -1;
};

struct Column
{
  std::string name_;
  ColumnType type_ = ColumnType::SQL_NULL;
};

// One decoded cell.  Every logical type also fills text_, which keeps the
// canonical text form needed by the SQL layer for VARCHAR, DATE and DATETIME.
struct Cell
{
  bool is_null_ = true;
  int64_t int_value_ = 0;
  double double_value_ = 0.0;
  bool bool_value_ = false;
  std::string text_;
};

struct Row
{
  std::vector<Cell> cells_;
  int64_t row_number_ = 0;
};

// Streaming reader over one local file.  Implementations never materialize the
// whole input: only the current record is held in memory.
class IRecordReader
{
public:
  virtual ~IRecordReader() {}
  virtual Status open(std::string &error) = 0;
  // Returns Status::OK with eof == true once the input is exhausted.
  virtual Status next(Row &row, bool &eof, std::string &error) = 0;
  virtual void close() = 0;
};

const char *format_name(FileFormat format);
bool parse_format_name(const char *name, int64_t length, FileFormat &format);
const char *column_type_name(ColumnType type);
bool parse_column_type_name(const char *name, int64_t length, ColumnType &type);
ColumnType merge_column_type(ColumnType left, ColumnType right);

// True when the text contains glob meta characters ('*', '?' or '[').
bool is_glob_pattern(const std::string &path);

// Enumerates the regular files matching a glob pattern, sorted by name.
Status expand_glob(const std::string &pattern, std::vector<std::string> &files, std::string &error);

// realpath() based canonicalization; the path must exist.
Status canonicalize_path(const std::string &path, std::string &canonical, std::string &error);

// File size and modification time, used to detect changes between bind and
// execution.
Status stat_file(const std::string &path, int64_t &size, int64_t &modify_time, std::string &error);

// Scans the whole file and returns the ordered output columns.
Status infer_schema(const std::string &path, const Options &options, std::vector<Column> &columns,
                    std::string &error);

// Creates a streaming reader.  When needed_columns is empty every column is
// decoded; otherwise only the listed column indexes are filled and the other
// cells stay NULL, which is what column pruning needs.
Status create_reader(const std::string &path, const Options &options, const std::vector<Column> &columns,
                     const std::vector<int64_t> &needed_columns, IRecordReader *&reader,
                     std::string &error);
void destroy_reader(IRecordReader *reader);

// Reader over the file list produced by expand_glob(): one VARCHAR column.
Status create_glob_reader(const std::vector<std::string> &files, IRecordReader *&reader,
                          std::string &error);

// Bind-time snapshot of everything the scan needs, serialized into one blob so
// that plan generation never has to touch the file system again.
std::string encode_options(const Options &options, const std::string &path,
                           const std::vector<Column> &columns, int64_t file_size, int64_t modify_time,
                           const std::vector<std::string> &files);
Status decode_options(const char *data, int64_t length, Options &options, std::string &path,
                      std::vector<Column> &columns, int64_t &file_size, int64_t &modify_time,
                      std::vector<std::string> &files);

// Exposed for the unit tests; they implement the cross format value rules.
bool parse_integer_text(const char *text, int64_t length, int64_t &value);
bool parse_double_text(const char *text, int64_t length, double &value);
bool parse_boolean_text(const char *text, int64_t length, bool &value);
bool parse_date_text(const char *text, int64_t length, int32_t &year, int32_t &month, int32_t &day);
bool parse_datetime_text(const char *text, int64_t length, std::string &canonical);
ColumnType detect_text_type(const char *text, int64_t length);
// Formats a double the way the file formats print it: integral values keep an
// integer form so that a widened column stays readable.
std::string format_double(double value);

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
