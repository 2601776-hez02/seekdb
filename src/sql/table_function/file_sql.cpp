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

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "sql/table_function/file_sql.h"

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

namespace
{

char lower_char(char c)
{
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool text_iequal(const char *left, const int64_t left_length, const char *right)
{
  const int64_t right_length = static_cast<int64_t>(strlen(right));
  bool equal = left_length == right_length;
  for (int64_t i = 0; equal && i < left_length; ++i) {
    if (lower_char(left[i]) != lower_char(right[i])) {
      equal = false;
    }
  }
  return equal;
}

bool is_space(char c)
{
  return ' ' == c || '\t' == c || '\n' == c || '\r' == c || '\f' == c || '\v' == c;
}

int32_t days_in_month(const int32_t year, const int32_t month)
{
  static const int32_t DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int32_t days = 0;
  if (month >= 1 && month <= 12) {
    days = DAYS[month - 1];
    if (2 == month && ((0 == year % 4 && 0 != year % 100) || 0 == year % 400)) {
      days = 29;
    }
  }
  return days;
}

bool read_fixed_digits(const char *text, const int64_t length, const int64_t offset,
                       const int64_t digits, int32_t &value)
{
  bool ok = offset + digits <= length;
  int32_t result = 0;
  for (int64_t i = 0; ok && i < digits; ++i) {
    const char c = text[offset + i];
    if (c < '0' || c > '9') {
      ok = false;
    } else {
      result = result * 10 + (c - '0');
    }
  }
  if (ok) {
    value = result;
  }
  return ok;
}

// Sequential buffered reader.  Only the current chunk is resident, so a scan
// never materializes the whole file.
class FileStream
{
public:
  FileStream() {}
  ~FileStream() { close(); }

  FileStream(const FileStream &) = delete;
  FileStream &operator=(const FileStream &) = delete;

  Status open(const std::string &path, std::string &error)
  {
    Status status = Status::OK;
    close();
    fp_ = fopen(path.c_str(), "rb");
    if (nullptr == fp_) {
      const int saved = errno;
      error = std::string("cannot open file '") + path + "': " + strerror(saved);
      status = (ENOENT == saved) ? Status::FILE_NOT_FOUND : Status::OPEN_FAILED;
    } else {
      eof_ = false;
    }
    return status;
  }

  void close()
  {
    if (nullptr != fp_) {
      fclose(fp_);
      fp_ = nullptr;
    }
    buffer_.clear();
    cursor_ = 0;
    eof_ = true;
  }

  int64_t available() const { return static_cast<int64_t>(buffer_.size()) - cursor_; }

  const char *data() const { return buffer_.data() + cursor_; }

  bool exhausted() const { return eof_ && cursor_ >= static_cast<int64_t>(buffer_.size()); }

  // Appends one more chunk.  Returns false when no byte could be read.
  bool read_more(std::string &error)
  {
    bool has_more = false;
    if (!eof_) {
      if (available() > 0) {
        buffer_.erase(0, static_cast<size_t>(cursor_));
        cursor_ = 0;
      }
      const size_t old_size = buffer_.size();
      buffer_.resize(old_size + static_cast<size_t>(READ_CHUNK_SIZE));
      const size_t read_size = fread(&buffer_[old_size], 1, static_cast<size_t>(READ_CHUNK_SIZE), fp_);
      buffer_.resize(old_size + read_size);
      if (read_size < static_cast<size_t>(READ_CHUNK_SIZE)) {
        if (0 != ferror(fp_)) {
          error = std::string("read failed: ") + strerror(errno);
        }
        eof_ = true;
      }
      has_more = available() > 0;
    }
    return has_more;
  }

  // Ensures at least one buffered byte while data remains.
  bool refill(std::string &error)
  {
    bool ok = true;
    while (available() <= 0 && !eof_) {
      if (!read_more(error)) {
        if (!error.empty()) {
          ok = false;
        }
      }
    }
    return ok && available() > 0;
  }

  void consume(const int64_t count)
  {
    cursor_ += count;
    if (cursor_ > static_cast<int64_t>(buffer_.size())) {
      cursor_ = static_cast<int64_t>(buffer_.size());
    }
  }

private:
  FILE *fp_ = nullptr;
  std::string buffer_;
  int64_t cursor_ = 0;
  bool eof_ = true;
};

} // namespace

const char *format_name(const FileFormat format)
{
  const char *name = "csv";
  switch (format) {
    case FileFormat::CSV: name = "csv"; break;
    case FileFormat::JSON: name = "json"; break;
    case FileFormat::NDJSON: name = "ndjson"; break;
    case FileFormat::PARQUET: name = "parquet"; break;
    default: name = "unknown"; break;
  }
  return name;
}

bool parse_format_name(const char *name, const int64_t length, FileFormat &format)
{
  bool found = true;
  if (text_iequal(name, length, "csv")) {
    format = FileFormat::CSV;
  } else if (text_iequal(name, length, "json")) {
    format = FileFormat::JSON;
  } else if (text_iequal(name, length, "ndjson")) {
    format = FileFormat::NDJSON;
  } else if (text_iequal(name, length, "parquet")) {
    format = FileFormat::PARQUET;
  } else {
    found = false;
  }
  return found;
}

const char *column_type_name(const ColumnType type)
{
  const char *name = "varchar";
  switch (type) {
    case ColumnType::SQL_NULL: name = "null"; break;
    case ColumnType::BOOLEAN: name = "boolean"; break;
    case ColumnType::BIGINT: name = "bigint"; break;
    case ColumnType::DOUBLE: name = "double"; break;
    case ColumnType::DATE: name = "date"; break;
    case ColumnType::DATETIME: name = "datetime"; break;
    case ColumnType::VARCHAR: name = "varchar"; break;
    default: name = "varchar"; break;
  }
  return name;
}

bool parse_column_type_name(const char *name, const int64_t length, ColumnType &type)
{
  bool found = true;
  if (text_iequal(name, length, "null")) {
    type = ColumnType::SQL_NULL;
  } else if (text_iequal(name, length, "boolean") || text_iequal(name, length, "bool")) {
    type = ColumnType::BOOLEAN;
  } else if (text_iequal(name, length, "bigint") || text_iequal(name, length, "int64")) {
    type = ColumnType::BIGINT;
  } else if (text_iequal(name, length, "double") || text_iequal(name, length, "float64")) {
    type = ColumnType::DOUBLE;
  } else if (text_iequal(name, length, "date")) {
    type = ColumnType::DATE;
  } else if (text_iequal(name, length, "datetime") || text_iequal(name, length, "timestamp")) {
    type = ColumnType::DATETIME;
  } else if (text_iequal(name, length, "varchar") || text_iequal(name, length, "string")) {
    type = ColumnType::VARCHAR;
  } else {
    found = false;
  }
  return found;
}

ColumnType merge_column_type(const ColumnType left, const ColumnType right)
{
  ColumnType result = ColumnType::VARCHAR;
  if (ColumnType::SQL_NULL == left) {
    result = right;
  } else if (ColumnType::SQL_NULL == right) {
    result = left;
  } else if (left == right) {
    result = left;
  } else if ((ColumnType::BIGINT == left && ColumnType::DOUBLE == right) ||
             (ColumnType::DOUBLE == left && ColumnType::BIGINT == right)) {
    result = ColumnType::DOUBLE;
  } else if ((ColumnType::DATE == left && ColumnType::DATETIME == right) ||
             (ColumnType::DATETIME == left && ColumnType::DATE == right)) {
    result = ColumnType::DATETIME;
  } else {
    result = ColumnType::VARCHAR;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Scalar text parsing shared by every format
// ---------------------------------------------------------------------------

bool parse_integer_text(const char *text, const int64_t length, int64_t &value)
{
  bool ok = length > 0;
  int64_t index = 0;
  bool negative = false;
  uint64_t magnitude = 0;
  if (ok && ('+' == text[0] || '-' == text[0])) {
    negative = '-' == text[0];
    index = 1;
    ok = index < length;
  }
  // A negative value may reach one further than INT64_MAX because INT64_MIN has
  // no positive counterpart; both bounds must infer as BIGINT instead of DOUBLE.
  const uint64_t limit = negative ? static_cast<uint64_t>(INT64_MAX) + 1
                                  : static_cast<uint64_t>(INT64_MAX);
  for (; ok && index < length; ++index) {
    const char c = text[index];
    if (c < '0' || c > '9') {
      ok = false;
    } else {
      const uint64_t digit = static_cast<uint64_t>(c - '0');
      if (magnitude > (limit - digit) / 10) {
        ok = false;
      } else {
        magnitude = magnitude * 10 + digit;
      }
    }
  }
  if (ok) {
    if (negative) {
      // Negating 2^63 is not representable, so name INT64_MIN explicitly.
      value = (static_cast<uint64_t>(INT64_MAX) + 1 == magnitude)
                  ? INT64_MIN
                  : -static_cast<int64_t>(magnitude);
    } else {
      value = static_cast<int64_t>(magnitude);
    }
  }
  return ok;
}

bool parse_double_text(const char *text, const int64_t length, double &value)
{
  bool ok = length > 0;
  bool has_digit = false;
  for (int64_t i = 0; ok && i < length; ++i) {
    const char c = text[i];
    if (c >= '0' && c <= '9') {
      has_digit = true;
    } else if ('+' != c && '-' != c && '.' != c && 'e' != c && 'E' != c) {
      ok = false;
    }
  }
  if (ok && has_digit) {
    std::string buffer(text, static_cast<size_t>(length));
    char *end = nullptr;
    errno = 0;
    const double parsed = strtod(buffer.c_str(), &end);
    if (nullptr == end || end != buffer.c_str() + buffer.length() || ERANGE == errno ||
        !std::isfinite(parsed)) {
      ok = false;
    } else {
      value = parsed;
    }
  } else {
    ok = false;
  }
  return ok;
}

bool parse_boolean_text(const char *text, const int64_t length, bool &value)
{
  bool ok = true;
  if (text_iequal(text, length, "true")) {
    value = true;
  } else if (text_iequal(text, length, "false")) {
    value = false;
  } else {
    ok = false;
  }
  return ok;
}

bool parse_date_text(const char *text, const int64_t length, int32_t &year, int32_t &month,
                     int32_t &day)
{
  bool ok = 10 == length;
  if (ok) {
    ok = read_fixed_digits(text, length, 0, 4, year) && '-' == text[4] &&
         read_fixed_digits(text, length, 5, 2, month) && '-' == text[7] &&
         read_fixed_digits(text, length, 8, 2, day);
  }
  if (ok) {
    ok = year >= 1 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 &&
         day <= days_in_month(year, month);
  }
  return ok;
}

namespace
{

// Parses "HH:MM:SS[.ffffff]" at offset and returns the normalized fraction.
bool parse_time_part(const char *text, const int64_t length, const int64_t offset,
                     std::string &fraction)
{
  int32_t hour = 0;
  int32_t minute = 0;
  int32_t second = 0;
  bool ok = length >= offset + 8;
  if (ok) {
    ok = read_fixed_digits(text, length, offset, 2, hour) && ':' == text[offset + 2] &&
         read_fixed_digits(text, length, offset + 3, 2, minute) && ':' == text[offset + 5] &&
         read_fixed_digits(text, length, offset + 6, 2, second);
  }
  if (ok) {
    ok = hour <= 23 && minute <= 59 && second <= 59;
  }
  int64_t consumed = offset + 8;
  fraction.clear();
  if (ok && consumed < length) {
    if ('.' != text[consumed]) {
      ok = false;
    } else {
      fraction = ".";
      ++consumed;
      int64_t digits = 0;
      while (ok && consumed < length && text[consumed] >= '0' && text[consumed] <= '9') {
        if (digits < 6) {
          fraction.push_back(text[consumed]);
        }
        ++digits;
        ++consumed;
      }
      ok = digits > 0;
    }
  }
  return ok && consumed == length;
}

} // namespace

bool parse_datetime_text(const char *text, const int64_t length, std::string &canonical)
{
  int32_t year = 0;
  int32_t month = 0;
  int32_t day = 0;
  std::string fraction;
  bool ok = length >= 19 && parse_date_text(text, 10, year, month, day) &&
            (' ' == text[10] || 'T' == text[10]) && parse_time_part(text, length, 11, fraction);
  if (ok) {
    canonical.assign(text, static_cast<size_t>(10));
    canonical.push_back(' ');
    canonical.append(text + 11, static_cast<size_t>(8));
    canonical.append(fraction);
  }
  return ok;
}

std::string format_double(const double value)
{
  std::string text;
  char buffer[64];
  if (value == std::floor(value) && std::fabs(value) < 1e15) {
    const int printed = snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    if (printed > 0) {
      text.assign(buffer, static_cast<size_t>(printed));
    }
  } else {
    const int printed = snprintf(buffer, sizeof(buffer), "%.15g", value);
    if (printed > 0) {
      char *end = nullptr;
      const double round_trip = strtod(buffer, &end);
      if (round_trip == value) {
        text.assign(buffer, static_cast<size_t>(printed));
      } else {
        const int wide = snprintf(buffer, sizeof(buffer), "%.17g", value);
        if (wide > 0) {
          text.assign(buffer, static_cast<size_t>(wide));
        }
      }
    }
  }
  return text;
}

ColumnType detect_text_type(const char *text, const int64_t length)
{
  ColumnType type = ColumnType::VARCHAR;
  int64_t int_value = 0;
  double double_value = 0.0;
  bool bool_value = false;
  int32_t year = 0;
  int32_t month = 0;
  int32_t day = 0;
  std::string canonical;
  if (0 == length) {
    type = ColumnType::VARCHAR;
  } else if (parse_boolean_text(text, length, bool_value)) {
    type = ColumnType::BOOLEAN;
  } else if (parse_integer_text(text, length, int_value)) {
    type = ColumnType::BIGINT;
  } else if (parse_double_text(text, length, double_value)) {
    type = ColumnType::DOUBLE;
  } else if (parse_date_text(text, length, year, month, day)) {
    type = ColumnType::DATE;
  } else if (parse_datetime_text(text, length, canonical)) {
    type = ColumnType::DATETIME;
  } else {
    type = ColumnType::VARCHAR;
  }
  return type;
}

namespace
{

// Converts one raw text value into a cell of the requested column type.
Status convert_text(const char *text, const int64_t length, const bool is_null,
                    const ColumnType type, Cell &cell, std::string &error)
{
  Status status = Status::OK;
  cell.is_null_ = is_null;
  cell.int_value_ = 0;
  cell.double_value_ = 0.0;
  cell.bool_value_ = false;
  cell.text_.clear();
  if (is_null || ColumnType::SQL_NULL == type) {
    cell.is_null_ = true;
  } else if (ColumnType::VARCHAR == type) {
    cell.text_.assign(text, static_cast<size_t>(length));
  } else if (ColumnType::BIGINT == type) {
    if (parse_integer_text(text, length, cell.int_value_)) {
      cell.text_.assign(text, static_cast<size_t>(length));
    } else if (parse_double_text(text, length, cell.double_value_)) {
      cell.int_value_ = static_cast<int64_t>(cell.double_value_);
      cell.text_ = format_double(cell.double_value_);
    } else {
      status = Status::PARSE_ERROR;
      error = std::string("cannot convert '") + std::string(text, static_cast<size_t>(length)) +
              "' to bigint";
    }
  } else if (ColumnType::DOUBLE == type) {
    if (parse_double_text(text, length, cell.double_value_)) {
      cell.text_ = format_double(cell.double_value_);
    } else if (parse_integer_text(text, length, cell.int_value_)) {
      cell.double_value_ = static_cast<double>(cell.int_value_);
      cell.text_ = format_double(cell.double_value_);
    } else {
      status = Status::PARSE_ERROR;
      error = std::string("cannot convert '") + std::string(text, static_cast<size_t>(length)) +
              "' to double";
    }
  } else if (ColumnType::BOOLEAN == type) {
    if (parse_boolean_text(text, length, cell.bool_value_)) {
      cell.int_value_ = cell.bool_value_ ? 1 : 0;
      cell.text_ = cell.bool_value_ ? "true" : "false";
    } else {
      status = Status::PARSE_ERROR;
      error = std::string("cannot convert '") + std::string(text, static_cast<size_t>(length)) +
              "' to boolean";
    }
  } else if (ColumnType::DATE == type) {
    int32_t year = 0;
    int32_t month = 0;
    int32_t day = 0;
    if (parse_date_text(text, length, year, month, day)) {
      cell.text_.assign(text, static_cast<size_t>(length));
    } else {
      status = Status::PARSE_ERROR;
      error = std::string("cannot convert '") + std::string(text, static_cast<size_t>(length)) +
              "' to date";
    }
  } else if (ColumnType::DATETIME == type) {
    if (parse_datetime_text(text, length, cell.text_)) {
      // canonical form already stored in text_
    } else {
      int32_t year = 0;
      int32_t month = 0;
      int32_t day = 0;
      if (parse_date_text(text, length, year, month, day)) {
        cell.text_.assign(text, static_cast<size_t>(length));
        cell.text_.append(" 00:00:00");
      } else {
        status = Status::PARSE_ERROR;
        error = std::string("cannot convert '") + std::string(text, static_cast<size_t>(length)) +
                "' to datetime";
      }
    }
  }
  return status;
}

bool column_needed(const std::vector<int64_t> &needed, const int64_t index)
{
  if (needed.empty()) {
    return true;
  }
  return std::find(needed.begin(), needed.end(), index) != needed.end();
}

// Duplicates are rejected later by the binder, so make names unique here.
std::string unique_column_name(const std::vector<Column> &columns, const std::string &name,
                               const int64_t position)
{
  std::string base = name.empty() ? ("column" + std::to_string(position)) : name;
  std::string candidate = base;
  int64_t suffix = 1;
  bool clash = true;
  while (clash) {
    clash = false;
    for (size_t i = 0; i < columns.size() && !clash; ++i) {
      if (text_iequal(candidate.c_str(), static_cast<int64_t>(candidate.length()),
                      columns[i].name_.c_str())) {
        clash = true;
      }
    }
    if (clash) {
      candidate = base + "_" + std::to_string(suffix);
      ++suffix;
    }
  }
  return candidate;
}

// ---------------------------------------------------------------------------
// CSV
// ---------------------------------------------------------------------------

// Reads one logical record.  A quoted field may contain the delimiter and line
// breaks; a doubled quote (or escape + character) keeps the literal character.
Status read_csv_record(FileStream &stream, const Options &options, std::vector<std::string> &fields,
                       bool &eof, std::string &error)
{
  Status status = Status::OK;
  std::string field;
  bool in_quotes = false;
  bool field_quoted = false;
  bool started = false;
  fields.clear();
  eof = false;
  for (;;) {
    if (!stream.refill(error)) {
      if (!error.empty()) {
        return Status::OPEN_FAILED;
      }
      if (in_quotes) {
        error = "unterminated quoted field at end of file";
        return Status::PARSE_ERROR;
      }
      if (!started) {
        eof = true;
      } else {
        fields.push_back(field);
        if (1 == fields.size() && fields[0].empty()) {
          fields.clear();
          eof = true;
        }
      }
      return Status::OK;
    }
    const char *data = stream.data();
    const int64_t available = stream.available();
    int64_t index = 0;
    bool record_done = false;
    bool need_more = false;
    for (; index < available; ++index) {
      const char c = data[index];
      if (in_quotes) {
        if (options.escape_ != options.quote_ && c == options.escape_) {
          if (index + 1 >= available) {
            need_more = true;
            break;
          }
          field.push_back(data[index + 1]);
          ++index;
        } else if (c == options.quote_) {
          if (index + 1 >= available) {
            need_more = true;
            break;
          }
          if (data[index + 1] == options.quote_ && options.escape_ == options.quote_) {
            field.push_back(options.quote_);
            ++index;
          } else {
            in_quotes = false;
          }
        } else {
          field.push_back(c);
        }
      } else if (c == options.delimiter_) {
        fields.push_back(field);
        field.clear();
        field_quoted = false;
        started = true;
      } else if ('\n' == c) {
        ++index;
        record_done = true;
        break;
      } else if ('\r' == c) {
        if (index + 1 >= available) {
          need_more = true;
          break;
        }
        index += ('\n' == data[index + 1]) ? 2 : 1;
        record_done = true;
        break;
      } else if (c == options.quote_ && field.empty() && !field_quoted) {
        in_quotes = true;
        field_quoted = true;
        started = true;
      } else {
        field.push_back(c);
        started = true;
      }
    }
    stream.consume(index);
    if (record_done) {
      fields.push_back(field);
      if (1 == fields.size() && fields[0].empty()) {
        field.clear();
        field_quoted = false;
        started = false;
        continue;
      }
      return Status::OK;
    }
    if (need_more) {
      if (!stream.read_more(error)) {
        if (!error.empty()) {
          return Status::OPEN_FAILED;
        }
        if (in_quotes) {
          error = "unterminated quoted field at end of file";
          return Status::PARSE_ERROR;
        }
        fields.push_back(field);
        return Status::OK;
      }
    }
  }
  return status;
}

class CsvReader : public IRecordReader
{
public:
  CsvReader(const std::string &path, const Options &options, const std::vector<Column> &columns,
            const std::vector<int64_t> &needed)
      : path_(path), options_(options), columns_(columns), needed_(needed), row_number_(0)
  {
  }

  Status open(std::string &error) override
  {
    Status status = stream_.open(path_, error);
    row_number_ = 0;
    if (Status::OK == status) {
      for (int64_t i = 0; Status::OK == status && i < options_.skip_rows_; ++i) {
        std::vector<std::string> skipped;
        bool eof = false;
        status = next_record(skipped, eof, error);
        if (Status::OK == status && eof) {
          error = "file ended while skipping rows";
          status = Status::PARSE_ERROR;
        }
      }
    }
    if (Status::OK == status && options_.header_) {
      std::vector<std::string> header;
      bool eof = false;
      status = next_record(header, eof, error);
      if (Status::OK == status && eof) {
        error = "file has no header record";
        status = Status::PARSE_ERROR;
      } else if (Status::OK == status &&
                 static_cast<int64_t>(header.size()) != static_cast<int64_t>(columns_.size())) {
        error = "header record has " + std::to_string(header.size()) + " fields but " +
                std::to_string(columns_.size()) + " columns were inferred";
        status = Status::PARSE_ERROR;
      }
    }
    if (Status::OK != status) {
      stream_.close();
    }
    return status;
  }

  Status next(Row &row, bool &eof, std::string &error) override
  {
    std::vector<std::string> fields;
    Status status = next_record(fields, eof, error);
    if (Status::OK == status && !eof) {
      ++row_number_;
      if (static_cast<int64_t>(fields.size()) != static_cast<int64_t>(columns_.size())) {
        error = "row " + std::to_string(row_number_) + " has " + std::to_string(fields.size()) +
                " fields but " + std::to_string(columns_.size()) + " were expected";
        status = Status::PARSE_ERROR;
      }
    }
    if (Status::OK == status && !eof) {
      if (row.cells_.size() != columns_.size()) {
        row.cells_.resize(columns_.size());
      }
      row.row_number_ = row_number_;
      for (size_t i = 0; Status::OK == status && i < columns_.size(); ++i) {
        if (!column_needed(needed_, static_cast<int64_t>(i))) {
          row.cells_[i].is_null_ = true;
          continue;
        }
        const std::string &field = fields[i];
        const bool is_null = !options_.null_string_.empty() && field == options_.null_string_;
        status = convert_text(field.data(), static_cast<int64_t>(field.size()), is_null,
                              columns_[i].type_, row.cells_[i], error);
      }
    }
    if (Status::PARSE_ERROR == status) {
      error = std::string("CSV: ") + error;
    }
    return status;
  }

  void close() override { stream_.close(); }

private:
  Status next_record(std::vector<std::string> &fields, bool &eof, std::string &error)
  {
    return read_csv_record(stream_, options_, fields, eof, error);
  }

  std::string path_;
  Options options_;
  const std::vector<Column> &columns_;
  const std::vector<int64_t> &needed_;
  FileStream stream_;
  int64_t row_number_;
};

// ---------------------------------------------------------------------------
// JSON / NDJSON
// ---------------------------------------------------------------------------

struct JsonMember
{
  std::string name_;
  bool is_null_ = true;
  std::string text_;
};

// Minimal strict JSON reader for one flat object per line.  Nested objects and
// arrays are rejected by the task contract instead of being flattened.
class JsonCursor
{
public:
  JsonCursor(const char *data, const int64_t size) : data_(data), size_(size), pos_(0) {}

  Status parse_object(std::vector<JsonMember> &members, std::string &error)
  {
    Status status = Status::OK;
    members.clear();
    skip_space();
    if (!consume('{')) {
      error = "expected a JSON object";
      status = Status::PARSE_ERROR;
    }
    bool first = true;
    while (Status::OK == status) {
      skip_space();
      if (consume('}')) {
        break;
      }
      if (!first) {
        if (!consume(',')) {
          error = "expected ',' between object members";
          status = Status::PARSE_ERROR;
          break;
        }
        skip_space();
      }
      first = false;
      JsonMember member;
      if (!parse_string(member.name_, error)) {
        status = Status::PARSE_ERROR;
      } else if (!consume(':')) {
        error = "expected ':' after a member name";
        status = Status::PARSE_ERROR;
      } else if (!parse_value(member, error)) {
        status = Status::PARSE_ERROR;
      } else {
        members.push_back(member);
      }
    }
    if (Status::OK == status) {
      skip_space();
      if (pos_ != size_) {
        error = "unexpected trailing characters after the JSON object";
        status = Status::PARSE_ERROR;
      }
    }
    return status;
  }

private:
  void skip_space()
  {
    while (pos_ < size_ && is_space(data_[pos_])) {
      ++pos_;
    }
  }

  bool consume(const char expected)
  {
    skip_space();
    bool ok = pos_ < size_ && data_[pos_] == expected;
    if (ok) {
      ++pos_;
    }
    return ok;
  }

  bool parse_string(std::string &out, std::string &error)
  {
    out.clear();
    skip_space();
    bool ok = pos_ < size_ && '"' == data_[pos_];
    if (ok) {
      ++pos_;
      ok = false;
      while (pos_ < size_) {
        const char c = data_[pos_++];
        if ('"' == c) {
          ok = true;
          break;
        } else if ('\\' == c) {
          if (pos_ >= size_) {
            break;
          }
          const char escape = data_[pos_++];
          if ('"' == escape || '\\' == escape || '/' == escape) {
            out.push_back(escape);
          } else if ('b' == escape) {
            out.push_back('\b');
          } else if ('f' == escape) {
            out.push_back('\f');
          } else if ('n' == escape) {
            out.push_back('\n');
          } else if ('r' == escape) {
            out.push_back('\r');
          } else if ('t' == escape) {
            out.push_back('\t');
          } else if ('u' == escape) {
            uint32_t code = 0;
            if (!read_hex4(code)) {
              break;
            }
            if (code >= 0xD800 && code <= 0xDBFF) {
              if (pos_ + 1 >= size_ || '\\' != data_[pos_] || 'u' != data_[pos_ + 1]) {
                break;
              }
              pos_ += 2;
              uint32_t low = 0;
              if (!read_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                break;
              }
              code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            }
            append_utf8(code, out);
          } else {
            break;
          }
        } else {
          out.push_back(c);
        }
      }
    }
    if (!ok && error.empty()) {
      error = "invalid JSON string";
    }
    return ok;
  }

  bool read_hex4(uint32_t &value)
  {
    bool ok = pos_ + 4 <= size_;
    uint32_t result = 0;
    for (int i = 0; ok && i < 4; ++i) {
      const char c = data_[pos_ + i];
      uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<uint32_t>(c - 'A' + 10);
      } else {
        ok = false;
      }
      result = (result << 4) | digit;
    }
    if (ok) {
      pos_ += 4;
      value = result;
    }
    return ok;
  }

  static void append_utf8(const uint32_t code, std::string &out)
  {
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  bool parse_value(JsonMember &member, std::string &error)
  {
    bool ok = true;
    skip_space();
    if (pos_ >= size_) {
      error = "unexpected end of JSON document";
      ok = false;
    } else if ('"' == data_[pos_]) {
      member.is_null_ = false;
      ok = parse_string(member.text_, error);
    } else if ('{' == data_[pos_] || '[' == data_[pos_]) {
      error = "nested objects and arrays are not supported";
      ok = false;
    } else if ('t' == data_[pos_] || 'f' == data_[pos_]) {
      const int64_t start = pos_;
      while (pos_ < size_ && !is_space(data_[pos_]) && ',' != data_[pos_] && '}' != data_[pos_]) {
        ++pos_;
      }
      bool value = false;
      if (!parse_boolean_text(data_ + start, pos_ - start, value)) {
        error = "invalid JSON literal";
        ok = false;
      } else {
        member.is_null_ = false;
        member.text_ = value ? "true" : "false";
      }
    } else if ('n' == data_[pos_]) {
      const int64_t start = pos_;
      while (pos_ < size_ && !is_space(data_[pos_]) && ',' != data_[pos_] && '}' != data_[pos_]) {
        ++pos_;
      }
      if (4 != pos_ - start || 0 != strncmp(data_ + start, "null", 4)) {
        error = "invalid JSON literal";
        ok = false;
      } else {
        member.is_null_ = true;
        member.text_.clear();
      }
    } else {
      const int64_t start = pos_;
      while (pos_ < size_ && !is_space(data_[pos_]) && ',' != data_[pos_] && '}' != data_[pos_]) {
        ++pos_;
      }
      const int64_t length = pos_ - start;
      double double_value = 0.0;
      int64_t int_value = 0;
      if (parse_integer_text(data_ + start, length, int_value)) {
        member.is_null_ = false;
        member.text_.assign(data_ + start, static_cast<size_t>(length));
      } else if (parse_double_text(data_ + start, length, double_value)) {
        member.is_null_ = false;
        member.text_ = format_double(double_value);
      } else {
        error = "invalid JSON number";
        ok = false;
      }
    }
    return ok;
  }

  const char *data_;
  int64_t size_;
  int64_t pos_;
};

Status next_text_line(FileStream &stream, std::string &line, bool &eof, std::string &error)
{
  line.clear();
  eof = false;
  for (;;) {
    if (!stream.refill(error)) {
      if (!error.empty()) {
        return Status::OPEN_FAILED;
      }
      eof = true;
      return Status::OK;
    }
    const char *data = stream.data();
    const int64_t available = stream.available();
    int64_t index = 0;
    bool found = false;
    for (; index < available; ++index) {
      if ('\n' == data[index]) {
        found = true;
        break;
      }
    }
    line.append(data, static_cast<size_t>(found ? index : available));
    stream.consume(found ? index + 1 : available);
    if (line.size() > static_cast<size_t>(MAX_RECORD_SIZE)) {
      error = "record exceeds the maximum supported size";
      return Status::TOO_LARGE;
    }
    if (found) {
      if (!line.empty() && '\r' == line[line.size() - 1]) {
        line.erase(line.size() - 1);
      }
      return Status::OK;
    }
  }
}

class JsonLineReader : public IRecordReader
{
public:
  JsonLineReader(const std::string &path, const std::vector<Column> &columns,
                 const std::vector<int64_t> &needed)
      : path_(path), columns_(columns), needed_(needed), row_number_(0)
  {
  }

  Status open(std::string &error) override
  {
    row_number_ = 0;
    return stream_.open(path_, error);
  }

  Status next(Row &row, bool &eof, std::string &error) override
  {
    Status status = Status::OK;
    eof = false;
    std::string line;
    while (Status::OK == status) {
      status = next_text_line(stream_, line, eof, error);
      if (Status::OK != status || eof) {
        break;
      }
      if (line.empty()) {
        continue;
      }
      ++row_number_;
      std::vector<JsonMember> members;
      JsonCursor cursor(line.data(), static_cast<int64_t>(line.size()));
      status = cursor.parse_object(members, error);
      if (Status::OK == status) {
        if (row.cells_.size() != columns_.size()) {
          row.cells_.resize(columns_.size());
        }
        row.row_number_ = row_number_;
        for (size_t i = 0; Status::OK == status && i < columns_.size(); ++i) {
          Cell &cell = row.cells_[i];
          if (!column_needed(needed_, static_cast<int64_t>(i))) {
            cell.is_null_ = true;
            continue;
          }
          const JsonMember *found = nullptr;
          for (size_t m = members.size(); m > 0 && nullptr == found; --m) {
            if (members[m - 1].name_ == columns_[i].name_) {
              found = &members[m - 1];
            }
          }
          if (nullptr == found) {
            cell.is_null_ = true;
          } else {
            status = convert_text(found->text_.data(),
                                  static_cast<int64_t>(found->text_.size()), found->is_null_,
                                  columns_[i].type_, cell, error);
          }
        }
      }
      break;
    }
    if (Status::PARSE_ERROR == status) {
      error = "line " + std::to_string(row_number_) + ": " + error;
    }
    return status;
  }

  void close() override { stream_.close(); }

private:
  std::string path_;
  const std::vector<Column> &columns_;
  const std::vector<int64_t> &needed_;
  FileStream stream_;
  int64_t row_number_;
};

// ---------------------------------------------------------------------------
// Parquet
// ---------------------------------------------------------------------------

#ifdef _WIN32
#define FILE_SEEK _fseeki64
#define FILE_TELL _ftelli64
#else
#define FILE_SEEK fseeko
#define FILE_TELL ftello
#endif

struct ParquetSchemaElement
{
  std::string name_;
  int32_t physical_type_ = -1;
  int32_t type_length_ = 0;
  int32_t repetition_ = 0;
  int32_t converted_type_ = -1;
  int32_t num_children_ = 0;
};

struct ParquetColumnChunk
{
  int32_t physical_type_ = -1;
  int32_t codec_ = 0;
  int64_t num_values_ = 0;
  int64_t data_page_offset_ = -1;
  int64_t dictionary_page_offset_ = -1;
  int64_t total_compressed_size_ = 0;
};

struct ParquetRowGroup
{
  std::vector<ParquetColumnChunk> columns_;
  int64_t num_rows_ = 0;
};

struct ParquetMeta
{
  std::vector<ParquetSchemaElement> leaves_;
  std::vector<ColumnType> column_types_;
  std::vector<bool> column_optional_;
  std::vector<int32_t> column_datetime_scale_;
  std::vector<ParquetRowGroup> row_groups_;
  int64_t num_rows_ = 0;
};

// Thrift compact protocol reader, the serialization parquet metadata uses.
class CompactReader
{
public:
  CompactReader(const uint8_t *data, const int64_t size) : data_(data), size_(size), pos_(0) {}

  int64_t pos() const { return pos_; }

  bool read_byte(uint8_t &value)
  {
    bool ok = pos_ < size_;
    if (ok) {
      value = data_[pos_++];
    }
    return ok;
  }

  bool read_varint(uint64_t &value)
  {
    value = 0;
    int shift = 0;
    bool ok = true;
    for (;;) {
      if (pos_ >= size_ || shift > 63) {
        ok = false;
        break;
      }
      const uint8_t byte = data_[pos_++];
      value |= static_cast<uint64_t>(byte & 0x7F) << shift;
      if (0 == (byte & 0x80)) {
        break;
      }
      shift += 7;
    }
    return ok;
  }

  bool read_zigzag(int64_t &value)
  {
    uint64_t raw = 0;
    const bool ok = read_varint(raw);
    if (ok) {
      value = static_cast<int64_t>(raw >> 1) ^ -static_cast<int64_t>(raw & 1);
    }
    return ok;
  }

  bool read_binary(std::string &out)
  {
    uint64_t length = 0;
    bool ok = read_varint(length) && pos_ + static_cast<int64_t>(length) <= size_;
    if (ok) {
      out.assign(reinterpret_cast<const char *>(data_ + pos_), static_cast<size_t>(length));
      pos_ += static_cast<int64_t>(length);
    }
    return ok;
  }

  void enter_struct()
  {
    saved_ids_.push_back(last_field_id_);
    last_field_id_ = 0;
  }

  void leave_struct()
  {
    if (!saved_ids_.empty()) {
      last_field_id_ = saved_ids_.back();
      saved_ids_.pop_back();
    }
  }

  // Reads one field header; type 0 is STOP.
  bool read_field(int8_t &type, int16_t &id)
  {
    uint8_t byte = 0;
    bool ok = read_byte(byte);
    if (ok) {
      type = static_cast<int8_t>(byte & 0x0F);
      if (0 == type) {
        // A zero type byte terminates the struct and carries no field id.
        return true;
      }
      const int16_t delta = static_cast<int16_t>(byte >> 4);
      if (0 == delta) {
        int64_t explicit_id = 0;
        ok = read_zigzag(explicit_id);
        id = static_cast<int16_t>(explicit_id);
      } else {
        id = static_cast<int16_t>(last_field_id_ + delta);
      }
      if (ok) {
        last_field_id_ = id;
      }
    }
    return ok;
  }

  bool skip(const int8_t type)
  {
    bool ok = true;
    switch (type) {
      case 0:
      case 1:
      case 2: break;
      case 3: { uint8_t ignored = 0; ok = read_byte(ignored); break; }
      case 4:
      case 5:
      case 6: { uint64_t ignored = 0; ok = read_varint(ignored); break; }
      case 7: ok = pos_ + 8 <= size_; if (ok) { pos_ += 8; } break;
      case 8: { std::string ignored; ok = read_binary(ignored); break; }
      case 9:
      case 10: {
        uint8_t header = 0;
        ok = read_byte(header);
        if (ok) {
          const int8_t element_type = static_cast<int8_t>(header & 0x0F);
          int64_t count = header >> 4;
          if (15 == count) {
            uint64_t explicit_size = 0;
            ok = read_varint(explicit_size);
            count = static_cast<int64_t>(explicit_size);
          }
          for (int64_t i = 0; ok && i < count; ++i) {
            ok = skip(element_type);
          }
        }
        break;
      }
      case 11: {
        uint64_t count = 0;
        ok = read_varint(count);
        if (ok && count > 0) {
          uint8_t types = 0;
          ok = read_byte(types);
          if (ok) {
            const int8_t key_type = static_cast<int8_t>((types >> 4) & 0x0F);
            const int8_t value_type = static_cast<int8_t>(types & 0x0F);
            for (uint64_t i = 0; ok && i < count; ++i) {
              ok = skip(key_type) && skip(value_type);
            }
          }
        }
        break;
      }
      case 12: {
        enter_struct();
        int8_t field_type = 0;
        int16_t field_id = 0;
        for (;;) {
          ok = read_field(field_type, field_id);
          if (!ok || 0 == field_type) {
            break;
          }
          ok = skip(field_type);
          if (!ok) {
            break;
          }
        }
        leave_struct();
        break;
      }
      default: ok = false; break;
    }
    return ok;
  }

private:
  const uint8_t *data_;
  int64_t size_;
  int64_t pos_;
  int16_t last_field_id_ = 0;
  std::vector<int16_t> saved_ids_;
};

bool parse_schema_element(CompactReader &reader, ParquetSchemaElement &element)
{
  bool ok = true;
  int8_t type = 0;
  int16_t id = 0;
  reader.enter_struct();
  for (;;) {
    ok = reader.read_field(type, id);
    if (!ok || 0 == type) {
      break;
    }
    if (1 == id && 5 == type) {
      int64_t value = 0;
      ok = reader.read_zigzag(value);
      element.physical_type_ = static_cast<int32_t>(value);
    } else if (2 == id && 5 == type) {
      int64_t value = 0;
      ok = reader.read_zigzag(value);
      element.type_length_ = static_cast<int32_t>(value);
    } else if (3 == id && 5 == type) {
      int64_t value = 0;
      ok = reader.read_zigzag(value);
      element.repetition_ = static_cast<int32_t>(value);
    } else if (4 == id && 8 == type) {
      ok = reader.read_binary(element.name_);
    } else if (5 == id && 5 == type) {
      int64_t value = 0;
      ok = reader.read_zigzag(value);
      element.num_children_ = static_cast<int32_t>(value);
    } else if (6 == id && 5 == type) {
      int64_t value = 0;
      ok = reader.read_zigzag(value);
      element.converted_type_ = static_cast<int32_t>(value);
    } else {
      ok = reader.skip(type);
    }
    if (!ok) {
      break;
    }
  }
  reader.leave_struct();
  return ok;
}

bool parse_column_metadata(CompactReader &reader, ParquetColumnChunk &chunk)
{
  bool ok = true;
  int8_t type = 0;
  int16_t id = 0;
  reader.enter_struct();
  for (;;) {
    ok = reader.read_field(type, id);
    if (!ok || 0 == type) {
      break;
    }
    if (1 == id && 5 == type) {
      int64_t value = 0;
      ok = reader.read_zigzag(value);
      chunk.physical_type_ = static_cast<int32_t>(value);
    } else if (4 == id && 5 == type) {
      int64_t value = 0;
      ok = reader.read_zigzag(value);
      chunk.codec_ = static_cast<int32_t>(value);
    } else if (5 == id && 6 == type) {
      ok = reader.read_zigzag(chunk.num_values_);
    } else if (7 == id && 6 == type) {
      ok = reader.read_zigzag(chunk.total_compressed_size_);
    } else if (9 == id && 6 == type) {
      ok = reader.read_zigzag(chunk.data_page_offset_);
    } else if (11 == id && 6 == type) {
      ok = reader.read_zigzag(chunk.dictionary_page_offset_);
    } else {
      ok = reader.skip(type);
    }
    if (!ok) {
      break;
    }
  }
  reader.leave_struct();
  return ok;
}

bool parse_column_chunk(CompactReader &reader, ParquetColumnChunk &chunk)
{
  bool ok = true;
  int8_t type = 0;
  int16_t id = 0;
  reader.enter_struct();
  for (;;) {
    ok = reader.read_field(type, id);
    if (!ok || 0 == type) {
      break;
    }
    if (3 == id && 12 == type) {
      ok = parse_column_metadata(reader, chunk);
    } else {
      ok = reader.skip(type);
    }
    if (!ok) {
      break;
    }
  }
  reader.leave_struct();
  return ok;
}

bool parse_row_group(CompactReader &reader, ParquetRowGroup &group)
{
  bool ok = true;
  int8_t type = 0;
  int16_t id = 0;
  reader.enter_struct();
  for (;;) {
    ok = reader.read_field(type, id);
    if (!ok || 0 == type) {
      break;
    }
    if (1 == id && 9 == type) {
      uint8_t header = 0;
      ok = reader.read_byte(header);
      if (ok) {
        const int8_t element_type = static_cast<int8_t>(header & 0x0F);
        int64_t count = header >> 4;
        if (15 == count) {
          uint64_t explicit_size = 0;
          ok = reader.read_varint(explicit_size);
          count = static_cast<int64_t>(explicit_size);
        }
        for (int64_t i = 0; ok && i < count; ++i) {
          ParquetColumnChunk chunk;
          if (12 != element_type) {
            ok = reader.skip(element_type);
          } else {
            ok = parse_column_chunk(reader, chunk);
          }
          if (ok) {
            group.columns_.push_back(chunk);
          }
        }
      }
    } else if (3 == id && 6 == type) {
      ok = reader.read_zigzag(group.num_rows_);
    } else {
      ok = reader.skip(type);
    }
    if (!ok) {
      break;
    }
  }
  reader.leave_struct();
  return ok;
}

ColumnType parquet_column_type(const ParquetSchemaElement &element)
{
  ColumnType type = ColumnType::VARCHAR;
  switch (element.physical_type_) {
    case 0: type = ColumnType::BOOLEAN; break;
    case 1:
      type = (6 == element.converted_type_) ? ColumnType::DATE : ColumnType::BIGINT;
      break;
    case 2:
      type = (9 == element.converted_type_ || 10 == element.converted_type_) ? ColumnType::DATETIME
                                                                            : ColumnType::BIGINT;
      break;
    case 4:
    case 5: type = ColumnType::DOUBLE; break;
    case 6: type = ColumnType::VARCHAR; break;
    default: type = ColumnType::VARCHAR; break;
  }
  return type;
}

Status read_file_range(FILE *fp, const int64_t offset, const int64_t size, std::string &buffer,
                       std::string &error)
{
  Status status = Status::OK;
  buffer.clear();
  if (0 != FILE_SEEK(fp, offset, SEEK_SET)) {
    error = std::string("cannot seek in file: ") + strerror(errno);
    status = Status::CORRUPTED_FILE;
  } else {
    buffer.resize(static_cast<size_t>(size));
    if (size > 0) {
      const size_t read_size = fread(&buffer[0], 1, static_cast<size_t>(size), fp);
      if (static_cast<int64_t>(read_size) != size) {
        error = "unexpected end of file";
        status = Status::CORRUPTED_FILE;
      }
    }
  }
  return status;
}

bool parse_file_metadata(const std::string &metadata, ParquetMeta &meta)
{
  bool ok = true;
  CompactReader reader(reinterpret_cast<const uint8_t *>(metadata.data()),
                       static_cast<int64_t>(metadata.size()));
  int8_t type = 0;
  int16_t id = 0;
  reader.enter_struct();
  for (;;) {
    ok = reader.read_field(type, id);
    if (!ok || 0 == type) {
      break;
    }
    if (2 == id && 9 == type) {
      uint8_t header = 0;
      ok = reader.read_byte(header);
      if (ok) {
        const int8_t element_type = static_cast<int8_t>(header & 0x0F);
        int64_t count = header >> 4;
        if (15 == count) {
          uint64_t explicit_size = 0;
          ok = reader.read_varint(explicit_size);
          count = static_cast<int64_t>(explicit_size);
        }
        for (int64_t i = 0; ok && i < count; ++i) {
          ParquetSchemaElement element;
          if (12 != element_type) {
            ok = reader.skip(element_type);
          } else {
            ok = parse_schema_element(reader, element);
          }
          if (ok) {
            meta.leaves_.push_back(element);
          }
        }
      }
    } else if (3 == id && 6 == type) {
      ok = reader.read_zigzag(meta.num_rows_);
    } else if (4 == id && 9 == type) {
      uint8_t header = 0;
      ok = reader.read_byte(header);
      if (ok) {
        const int8_t element_type = static_cast<int8_t>(header & 0x0F);
        int64_t count = header >> 4;
        if (15 == count) {
          uint64_t explicit_size = 0;
          ok = reader.read_varint(explicit_size);
          count = static_cast<int64_t>(explicit_size);
        }
        for (int64_t i = 0; ok && i < count; ++i) {
          ParquetRowGroup group;
          if (12 != element_type) {
            ok = reader.skip(element_type);
          } else {
            ok = parse_row_group(reader, group);
          }
          if (ok) {
            meta.row_groups_.push_back(group);
          }
        }
      }
    } else {
      ok = reader.skip(type);
    }
    if (!ok) {
      break;
    }
  }
  reader.leave_struct();
  return ok;
}

Status load_parquet_meta(FILE *fp, ParquetMeta &meta, std::vector<Column> *columns,
                         std::string &error)
{
  Status status = Status::OK;
  std::string footer;
  if (0 != FILE_SEEK(fp, 0, SEEK_END)) {
    error = "cannot determine the parquet file size";
    status = Status::CORRUPTED_FILE;
  } else {
    const int64_t file_size = static_cast<int64_t>(FILE_TELL(fp));
    if (file_size < 12) {
      error = "file is too small to be a parquet file";
      status = Status::CORRUPTED_FILE;
    } else {
      status = read_file_range(fp, file_size - 8, 8, footer, error);
    }
    if (Status::OK == status) {
      if (0 != footer.compare(4, 4, "PAR1")) {
        error = "missing parquet footer magic";
        status = Status::CORRUPTED_FILE;
      } else {
        const uint32_t metadata_length = static_cast<uint32_t>(static_cast<uint8_t>(footer[0])) |
            (static_cast<uint32_t>(static_cast<uint8_t>(footer[1])) << 8) |
            (static_cast<uint32_t>(static_cast<uint8_t>(footer[2])) << 16) |
            (static_cast<uint32_t>(static_cast<uint8_t>(footer[3])) << 24);
        const int64_t metadata_size = static_cast<int64_t>(metadata_length);
        if (metadata_size <= 0 || metadata_size > file_size - 12) {
          error = "invalid parquet metadata length";
          status = Status::CORRUPTED_FILE;
        } else {
          std::string metadata;
          status = read_file_range(fp, file_size - 8 - metadata_size, metadata_size, metadata,
                                   error);
          if (Status::OK == status && !parse_file_metadata(metadata, meta)) {
            error = "cannot parse parquet metadata";
            status = Status::CORRUPTED_FILE;
          }
        }
      }
    }
  }
  if (Status::OK == status) {
    if (meta.leaves_.empty()) {
      error = "parquet file has no schema";
      status = Status::CORRUPTED_FILE;
    }
    // leaves_[0] is the root group; the flat leaf schema follows it.
    for (size_t i = 1; Status::OK == status && i < meta.leaves_.size(); ++i) {
      const ParquetSchemaElement &element = meta.leaves_[i];
      if (element.num_children_ > 0) {
        error = "nested parquet schemas are not supported";
        status = Status::UNSUPPORTED;
      } else if (2 == element.repetition_) {
        error = "repeated parquet columns are not supported";
        status = Status::UNSUPPORTED;
      } else if (0 != element.physical_type_ && 3 == element.physical_type_) {
        error = "INT96 parquet columns are not supported";
        status = Status::UNSUPPORTED;
      } else if (0 != element.physical_type_ && 7 == element.physical_type_) {
        error = "FIXED_LEN_BYTE_ARRAY parquet columns are not supported";
        status = Status::UNSUPPORTED;
      } else if (-1 == element.physical_type_) {
        error = "parquet leaf has no physical type";
        status = Status::CORRUPTED_FILE;
      } else {
        meta.column_types_.push_back(parquet_column_type(element));
        meta.column_optional_.push_back(1 == element.repetition_);
        int32_t scale = 0;
        if (ColumnType::DATETIME == meta.column_types_.back()) {
          scale = (9 == element.converted_type_) ? 1 : (10 == element.converted_type_ ? 2 : 0);
        }
        meta.column_datetime_scale_.push_back(scale);
        if (nullptr != columns) {
          Column column;
          column.name_ = unique_column_name(*columns, element.name_,
                                            static_cast<int64_t>(columns->size()));
          column.type_ = meta.column_types_.back();
          columns->push_back(column);
        }
      }
    }
    if (Status::OK == status && nullptr != columns && columns->empty()) {
      error = "parquet file has no columns";
      status = Status::CORRUPTED_FILE;
    }
  }
  return status;
}

// Snappy raw block decompression, the Parquet default codec.  Snappy is not a
// dependency of this tree and the block format is small enough to decode.
bool snappy_uncompress(const uint8_t *source, const int64_t source_size, std::string &output,
                       std::string &error)
{
  bool ok = true;
  output.clear();
  int64_t pos = 0;
  uint64_t expected = 0;
  int shift = 0;
  for (;;) {
    if (pos >= source_size || shift > 31) {
      ok = false;
      break;
    }
    const uint8_t byte = source[pos++];
    expected |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if (0 == (byte & 0x80)) {
      break;
    }
    shift += 7;
  }
  if (!ok) {
    error = "invalid snappy block header";
    return false;
  }
  output.reserve(static_cast<size_t>(expected));
  while (ok && pos < source_size) {
    const uint8_t tag = source[pos++];
    const int type = tag & 0x03;
    if (0 == type) {
      int64_t length = tag >> 2;
      if (length < 60) {
        length += 1;
      } else {
        const int extra = static_cast<int>(length) - 59;
        if (pos + extra > source_size) {
          ok = false;
          break;
        }
        length = 0;
        for (int i = 0; i < extra; ++i) {
          length |= static_cast<int64_t>(source[pos + i]) << (8 * i);
        }
        length += 1;
        pos += extra;
      }
      if (pos + length > source_size ||
          static_cast<int64_t>(output.size()) + length > static_cast<int64_t>(expected)) {
        ok = false;
        break;
      }
      output.append(reinterpret_cast<const char *>(source + pos), static_cast<size_t>(length));
      pos += length;
    } else {
      int64_t length = 0;
      int64_t offset = 0;
      if (1 == type) {
        if (pos >= source_size) {
          ok = false;
          break;
        }
        length = 4 + ((tag >> 2) & 0x07);
        offset = (static_cast<int64_t>(tag >> 5) << 8) | source[pos];
        pos += 1;
      } else if (2 == type) {
        if (pos + 2 > source_size) {
          ok = false;
          break;
        }
        length = 1 + (tag >> 2);
        offset = static_cast<int64_t>(source[pos]) | (static_cast<int64_t>(source[pos + 1]) << 8);
        pos += 2;
      } else {
        if (pos + 4 > source_size) {
          ok = false;
          break;
        }
        length = 1 + (tag >> 2);
        offset = static_cast<int64_t>(source[pos]) | (static_cast<int64_t>(source[pos + 1]) << 8) |
                 (static_cast<int64_t>(source[pos + 2]) << 16) |
                 (static_cast<int64_t>(source[pos + 3]) << 24);
        pos += 4;
      }
      if (offset <= 0 || offset > static_cast<int64_t>(output.size()) ||
          static_cast<int64_t>(output.size()) + length > static_cast<int64_t>(expected)) {
        ok = false;
        break;
      }
      const int64_t start = static_cast<int64_t>(output.size()) - offset;
      for (int64_t i = 0; i < length; ++i) {
        output.push_back(output[static_cast<size_t>(start + i)]);
      }
    }
  }
  if (ok && static_cast<int64_t>(output.size()) != static_cast<int64_t>(expected)) {
    ok = false;
  }
  if (!ok && error.empty()) {
    error = "invalid snappy block";
  }
  return ok;
}

// Decodes the RLE / bit-packed hybrid encoding used for levels and dictionary
// indexes.  Bit packing is little endian, least significant bit first.
bool decode_hybrid(const uint8_t *data, const int64_t available, const int32_t width,
                   const int64_t count, std::vector<uint32_t> &values, int64_t &consumed)
{
  bool ok = true;
  int64_t pos = 0;
  values.clear();
  if (0 == width) {
    values.assign(static_cast<size_t>(count), 0);
    consumed = 0;
    return true;
  }
  while (ok && static_cast<int64_t>(values.size()) < count) {
    uint32_t header = 0;
    int shift = 0;
    for (;;) {
      if (pos >= available || shift > 28) {
        ok = false;
        break;
      }
      const uint8_t byte = data[pos++];
      header |= static_cast<uint32_t>(byte & 0x7F) << shift;
      if (0 == (byte & 0x80)) {
        break;
      }
      shift += 7;
    }
    if (!ok) {
      break;
    }
    if (header & 1) {
      const int64_t groups = header >> 1;
      const int64_t bytes = groups * width;
      if (pos + bytes > available) {
        ok = false;
        break;
      }
      int64_t bit = 0;
      for (int64_t group = 0; group < groups && ok; ++group) {
        for (int64_t item = 0; item < 8; ++item) {
          uint32_t value = 0;
          for (int32_t b = 0; b < width; ++b) {
            const int64_t byte_index = pos + (bit >> 3);
            const int bit_index = static_cast<int>(bit & 7);
            if (0 != (data[byte_index] & (1u << bit_index))) {
              value |= (1u << b);
            }
            ++bit;
          }
          if (static_cast<int64_t>(values.size()) < count) {
            values.push_back(value);
          }
        }
      }
      pos += bytes;
    } else {
      const int64_t run = header >> 1;
      const int64_t bytes = (width + 7) / 8;
      if (pos + bytes > available) {
        ok = false;
        break;
      }
      uint32_t value = 0;
      for (int64_t i = 0; i < bytes; ++i) {
        value |= static_cast<uint32_t>(data[pos + i]) << (8 * i);
      }
      pos += bytes;
      for (int64_t i = 0; i < run && static_cast<int64_t>(values.size()) < count; ++i) {
        values.push_back(value);
      }
    }
  }
  consumed = pos;
  return ok;
}

void civil_from_days(const int64_t days, int32_t &year, int32_t &month, int32_t &day)
{
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const int64_t m = mp < 10 ? mp + 3 : mp - 9;
  year = static_cast<int32_t>(m <= 2 ? y + 1 : y);
  month = static_cast<int32_t>(m);
  day = static_cast<int32_t>(d);
}

std::string format_date_from_days(const int64_t days)
{
  int32_t year = 0;
  int32_t month = 0;
  int32_t day = 0;
  char buffer[32];
  civil_from_days(days, year, month, day);
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year, month, day);
  return std::string(buffer);
}

std::string format_datetime_from_epoch(const int64_t value, const int32_t scale)
{
  int64_t per_second = 1;
  int64_t fraction = 0;
  int64_t seconds = value;
  if (1 == scale) {
    per_second = 1000;
    seconds = value >= 0 ? value / 1000 : -(((-value) + 999) / 1000);
    fraction = value - seconds * 1000;
  } else if (2 == scale) {
    per_second = 1000000;
    seconds = value >= 0 ? value / 1000000 : -(((-value) + 999999) / 1000000);
    fraction = value - seconds * 1000000;
  }
  (void)per_second;
  const int64_t days = seconds >= 0 ? seconds / 86400 : -(((-seconds) + 86399) / 86400);
  const int64_t second_of_day = seconds - days * 86400;
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%s %02d:%02d:%02d", format_date_from_days(days).c_str(),
           static_cast<int>(second_of_day / 3600), static_cast<int>((second_of_day % 3600) / 60),
           static_cast<int>(second_of_day % 60));
  std::string text(buffer);
  if (0 != fraction) {
    char frac[16];
    if (1 == scale) {
      snprintf(frac, sizeof(frac), ".%03lld", static_cast<long long>(fraction));
    } else {
      snprintf(frac, sizeof(frac), ".%06lld", static_cast<long long>(fraction));
    }
    text.append(frac);
  }
  return text;
}

// Decodes one PLAIN encoded value page into canonical text values.
bool decode_plain_values(const uint8_t *data, const int64_t size, const int32_t physical_type,
                         const ColumnType column_type, const int32_t datetime_scale,
                         const int64_t count, std::vector<std::string> &texts)
{
  bool ok = true;
  texts.clear();
  texts.reserve(static_cast<size_t>(count));
  int64_t pos = 0;
  if (0 == physical_type) {
    for (int64_t i = 0; ok && i < count; ++i) {
      const int64_t byte_index = i >> 3;
      if (byte_index >= size) {
        ok = false;
        break;
      }
      const bool value = 0 != (data[byte_index] & (1u << (i & 7)));
      texts.push_back(value ? "true" : "false");
    }
  } else if (1 == physical_type) {
    for (int64_t i = 0; ok && i < count; ++i) {
      if (pos + 4 > size) {
        ok = false;
        break;
      }
      const uint32_t raw = static_cast<uint32_t>(data[pos]) |
                           (static_cast<uint32_t>(data[pos + 1]) << 8) |
                           (static_cast<uint32_t>(data[pos + 2]) << 16) |
                           (static_cast<uint32_t>(data[pos + 3]) << 24);
      pos += 4;
      const int32_t value = static_cast<int32_t>(raw);
      if (ColumnType::DATE == column_type) {
        texts.push_back(format_date_from_days(value));
      } else {
        texts.push_back(std::to_string(value));
      }
    }
  } else if (2 == physical_type) {
    for (int64_t i = 0; ok && i < count; ++i) {
      if (pos + 8 > size) {
        ok = false;
        break;
      }
      uint64_t raw = 0;
      for (int b = 0; b < 8; ++b) {
        raw |= static_cast<uint64_t>(data[pos + b]) << (8 * b);
      }
      pos += 8;
      const int64_t value = static_cast<int64_t>(raw);
      if (ColumnType::DATETIME == column_type) {
        texts.push_back(format_datetime_from_epoch(value, datetime_scale));
      } else {
        texts.push_back(std::to_string(value));
      }
    }
  } else if (4 == physical_type) {
    for (int64_t i = 0; ok && i < count; ++i) {
      if (pos + 4 > size) {
        ok = false;
        break;
      }
      uint32_t raw = 0;
      for (int b = 0; b < 4; ++b) {
        raw |= static_cast<uint32_t>(data[pos + b]) << (8 * b);
      }
      pos += 4;
      float value = 0.0f;
      memcpy(&value, &raw, sizeof(value));
      texts.push_back(format_double(static_cast<double>(value)));
    }
  } else if (5 == physical_type) {
    for (int64_t i = 0; ok && i < count; ++i) {
      if (pos + 8 > size) {
        ok = false;
        break;
      }
      uint64_t raw = 0;
      for (int b = 0; b < 8; ++b) {
        raw |= static_cast<uint64_t>(data[pos + b]) << (8 * b);
      }
      pos += 8;
      double value = 0.0;
      memcpy(&value, &raw, sizeof(value));
      texts.push_back(format_double(value));
    }
  } else if (6 == physical_type) {
    for (int64_t i = 0; ok && i < count; ++i) {
      if (pos + 4 > size) {
        ok = false;
        break;
      }
      const uint32_t length = static_cast<uint32_t>(static_cast<uint8_t>(data[pos])) |
                              (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 1])) << 8) |
                              (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 2])) << 16) |
                              (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 3])) << 24);
      pos += 4;
      if (pos + static_cast<int64_t>(length) > size) {
        ok = false;
        break;
      }
      texts.push_back(std::string(reinterpret_cast<const char *>(data + pos), length));
      pos += static_cast<int64_t>(length);
    }
  } else {
    ok = false;
  }
  return ok;
}

bool decompress_page(const int32_t codec, const std::string &input, std::string &output,
                     std::string &error)
{
  bool ok = true;
  if (0 == codec) {
    output = input;
  } else if (1 == codec) {
    ok = snappy_uncompress(reinterpret_cast<const uint8_t *>(input.data()),
                           static_cast<int64_t>(input.size()), output, error);
  } else {
    error = "parquet compression codec is not supported, only uncompressed and snappy are";
    ok = false;
  }
  return ok;
}

struct ParquetPage
{
  int32_t type_ = -1;
  int32_t uncompressed_size_ = 0;
  int32_t compressed_size_ = 0;
  int32_t num_values_ = 0;
  int32_t encoding_ = 0;
  int32_t definition_levels_byte_length_ = 0;
  int32_t repetition_levels_byte_length_ = 0;
  bool is_compressed_ = true;
  int64_t header_length_ = 0;
};

bool parse_page_header(const std::string &bytes, ParquetPage &page)
{
  bool ok = true;
  CompactReader reader(reinterpret_cast<const uint8_t *>(bytes.data()),
                       static_cast<int64_t>(bytes.size()));
  int8_t type = 0;
  int16_t id = 0;
  reader.enter_struct();
  for (;;) {
    ok = reader.read_field(type, id);
    if (!ok || 0 == type) {
      break;
    }
    if (1 == id && 5 == type) {
      int64_t value = 0;
      ok = reader.read_zigzag(value);
      page.type_ = static_cast<int32_t>(value);
    } else if (2 == id && 5 == type) {
      int64_t value = 0;
      ok = reader.read_zigzag(value);
      page.uncompressed_size_ = static_cast<int32_t>(value);
    } else if (3 == id && 5 == type) {
      int64_t value = 0;
      ok = reader.read_zigzag(value);
      page.compressed_size_ = static_cast<int32_t>(value);
    } else if (5 == id && 12 == type) {
      reader.enter_struct();
      int8_t inner_type = 0;
      int16_t inner_id = 0;
      for (;;) {
        if (!reader.read_field(inner_type, inner_id) || 0 == inner_type) {
          break;
        }
        if (1 == inner_id && 5 == inner_type) {
          int64_t value = 0;
          ok = reader.read_zigzag(value);
          page.num_values_ = static_cast<int32_t>(value);
        } else if (2 == inner_id && 5 == inner_type) {
          int64_t value = 0;
          ok = reader.read_zigzag(value);
          page.encoding_ = static_cast<int32_t>(value);
        } else if (!reader.skip(inner_type)) {
          ok = false;
          break;
        }
      }
      reader.leave_struct();
    } else if (7 == id && 12 == type) {
      reader.enter_struct();
      int8_t inner_type = 0;
      int16_t inner_id = 0;
      for (;;) {
        if (!reader.read_field(inner_type, inner_id) || 0 == inner_type) {
          break;
        }
        if (1 == inner_id && 5 == inner_type) {
          int64_t value = 0;
          ok = reader.read_zigzag(value);
          page.num_values_ = static_cast<int32_t>(value);
        } else if (!reader.skip(inner_type)) {
          ok = false;
          break;
        }
      }
      reader.leave_struct();
    } else if (8 == id && 12 == type) {
      reader.enter_struct();
      int8_t inner_type = 0;
      int16_t inner_id = 0;
      for (;;) {
        if (!reader.read_field(inner_type, inner_id) || 0 == inner_type) {
          break;
        }
        if (1 == inner_id && 5 == inner_type) {
          int64_t value = 0;
          ok = reader.read_zigzag(value);
          page.num_values_ = static_cast<int32_t>(value);
        } else if (4 == inner_id && 5 == inner_type) {
          int64_t value = 0;
          ok = reader.read_zigzag(value);
          page.encoding_ = static_cast<int32_t>(value);
        } else if (5 == inner_id && 5 == inner_type) {
          int64_t value = 0;
          ok = reader.read_zigzag(value);
          page.definition_levels_byte_length_ = static_cast<int32_t>(value);
        } else if (6 == inner_id && 5 == inner_type) {
          int64_t value = 0;
          ok = reader.read_zigzag(value);
          page.repetition_levels_byte_length_ = static_cast<int32_t>(value);
        } else if (7 == inner_id && (1 == inner_type || 2 == inner_type)) {
          page.is_compressed_ = (1 == inner_type);
        } else if (!reader.skip(inner_type)) {
          ok = false;
          break;
        }
      }
      reader.leave_struct();
    } else if (!reader.skip(type)) {
      ok = false;
      break;
    }
    if (!ok) {
      break;
    }
  }
  reader.leave_struct();
  page.header_length_ = reader.pos();
  return ok;
}

class ParquetReader : public IRecordReader
{
public:
  ParquetReader(const std::string &path, const std::vector<Column> &columns,
                const std::vector<int64_t> &needed)
      : path_(path), columns_(columns), needed_(needed), fp_(nullptr), row_group_index_(-1),
        row_in_group_(0)
  {
  }

  ~ParquetReader() override { close(); }

  Status open(std::string &error) override
  {
    close();
    Status status = Status::OK;
    fp_ = fopen(path_.c_str(), "rb");
    if (nullptr == fp_) {
      const int saved = errno;
      error = std::string("cannot open file '") + path_ + "': " + strerror(saved);
      status = (ENOENT == saved) ? Status::FILE_NOT_FOUND : Status::OPEN_FAILED;
    } else {
      status = load_parquet_meta(fp_, meta_, nullptr, error);
      if (Status::OK == status &&
          static_cast<int64_t>(meta_.column_types_.size()) !=
              static_cast<int64_t>(columns_.size())) {
        error = "parquet schema does not match the inferred schema";
        status = Status::CORRUPTED_FILE;
      }
    }
    if (Status::OK != status) {
      close();
    }
    return status;
  }

  Status next(Row &row, bool &eof, std::string &error) override
  {
    Status status = Status::OK;
    eof = false;
    while (Status::OK == status) {
      if (row_group_index_ < 0 ||
          row_in_group_ >= meta_.row_groups_[static_cast<size_t>(row_group_index_)].num_rows_) {
        ++row_group_index_;
        row_in_group_ = 0;
        if (row_group_index_ >= static_cast<int64_t>(meta_.row_groups_.size())) {
          eof = true;
          break;
        }
        status = load_row_group(error);
        if (Status::OK != status) {
          break;
        }
      }
      if (Status::OK == status) {
        if (row.cells_.size() != columns_.size()) {
          row.cells_.resize(columns_.size());
        }
        row.row_number_ = row_in_group_ + 1;
        for (size_t i = 0; Status::OK == status && i < columns_.size(); ++i) {
          Cell &cell = row.cells_[i];
          if (!column_needed(needed_, static_cast<int64_t>(i))) {
            cell.is_null_ = true;
            continue;
          }
          const RawValues &values = group_values_.at(i);
          const size_t index = static_cast<size_t>(row_in_group_);
          if (index >= values.texts_.size()) {
            cell.is_null_ = true;
          } else {
            status = convert_text(values.texts_[index].data(),
                                  static_cast<int64_t>(values.texts_[index].size()),
                                  0 != values.null_flags_[index], columns_[i].type_, cell, error);
          }
        }
        ++row_in_group_;
        break;
      }
    }
    if (Status::PARSE_ERROR == status) {
      error = std::string("parquet: ") + error;
    }
    return status;
  }

  void close() override
  {
    if (nullptr != fp_) {
      fclose(fp_);
      fp_ = nullptr;
    }
    meta_ = ParquetMeta();
    group_values_.clear();
    row_group_index_ = -1;
    row_in_group_ = 0;
  }

private:
  struct RawValues
  {
    std::vector<uint8_t> null_flags_;
    std::vector<std::string> texts_;
  };

  Status load_row_group(std::string &error)
  {
    Status status = Status::OK;
    group_values_.clear();
    if (row_group_index_ < 0 ||
        row_group_index_ >= static_cast<int64_t>(meta_.row_groups_.size())) {
      error = "invalid parquet row group";
      status = Status::CORRUPTED_FILE;
    } else {
      const ParquetRowGroup &group = meta_.row_groups_[static_cast<size_t>(row_group_index_)];
      group_values_.resize(columns_.size());
      for (size_t i = 0; Status::OK == status && i < columns_.size(); ++i) {
        if (!column_needed(needed_, static_cast<int64_t>(i))) {
          continue;
        }
        if (i + 1 >= meta_.leaves_.size() || i >= group.columns_.size()) {
          error = "parquet row group is missing a column";
          status = Status::CORRUPTED_FILE;
        } else {
          status = decode_column_chunk(meta_.leaves_[i + 1], group.columns_[i],
                                       meta_.column_types_[i],
                                       meta_.column_datetime_scale_[i], group_values_[i], error);
        }
      }
    }
    return status;
  }

  Status decode_column_chunk(const ParquetSchemaElement &leaf, const ParquetColumnChunk &chunk,
                             const ColumnType column_type, const int32_t datetime_scale,
                             RawValues &values, std::string &error)
  {
    Status status = Status::OK;
    values.null_flags_.clear();
    values.texts_.clear();
    int64_t offset = chunk.data_page_offset_;
    if (chunk.dictionary_page_offset_ > 0 &&
        (offset < 0 || chunk.dictionary_page_offset_ < offset)) {
      offset = chunk.dictionary_page_offset_;
    }
    if (offset < 0) {
      error = "parquet column chunk has no page offset";
      return Status::CORRUPTED_FILE;
    }
    const int64_t end = offset + chunk.total_compressed_size_;
    const bool optional = 1 == leaf.repetition_;
    std::vector<std::string> dictionary;
    bool has_dictionary = false;
    while (Status::OK == status && offset < end) {
      ParquetPage page;
      int64_t header_length = 0;
      bool parsed = false;
      for (int64_t window = 64; !parsed && Status::OK == status; window *= 8) {
        std::string header_bytes;
        status = read_file_range(fp_, offset, window, header_bytes, error);
        if (Status::OK != status) {
          break;
        }
        parsed = parse_page_header(header_bytes, page);
        header_length = page.header_length_;
        if (!parsed && window >= (1 << 20)) {
          error = "cannot parse parquet page header";
          status = Status::CORRUPTED_FILE;
        }
      }
      if (Status::OK != status) {
        break;
      }
      if (!parsed || page.compressed_size_ <= 0 || header_length <= 0) {
        error = "invalid parquet page header";
        status = Status::CORRUPTED_FILE;
        break;
      }
      std::string page_bytes;
      status = read_file_range(fp_, offset + header_length, page.compressed_size_, page_bytes, error);
      if (Status::OK != status) {
        break;
      }
      offset += header_length + page.compressed_size_;
      if (2 == page.type_) {
        std::string decoded;
        if (!decompress_page(chunk.codec_, page_bytes, decoded, error)) {
          status = Status::UNSUPPORTED;
        } else if (!decode_plain_values(reinterpret_cast<const uint8_t *>(decoded.data()),
                                        static_cast<int64_t>(decoded.size()),
                                        chunk.physical_type_, column_type, datetime_scale,
                                        page.num_values_, dictionary)) {
          error = "cannot decode parquet dictionary page";
          status = Status::CORRUPTED_FILE;
        } else {
          has_dictionary = true;
        }
        continue;
      }
      if (0 != page.type_ && 3 != page.type_) {
        continue;
      }
      std::string levels;
      std::string payload;
      if (0 == page.type_) {
        std::string decoded;
        if (!decompress_page(chunk.codec_, page_bytes, decoded, error)) {
          status = Status::UNSUPPORTED;
          break;
        }
        if (optional) {
          if (decoded.size() < 4) {
            error = "invalid parquet definition level prefix";
            status = Status::CORRUPTED_FILE;
            break;
          }
          const uint32_t length = static_cast<uint32_t>(static_cast<uint8_t>(decoded[0])) |
                                  (static_cast<uint32_t>(static_cast<uint8_t>(decoded[1])) << 8) |
                                  (static_cast<uint32_t>(static_cast<uint8_t>(decoded[2])) << 16) |
                                  (static_cast<uint32_t>(static_cast<uint8_t>(decoded[3])) << 24);
          if (static_cast<int64_t>(length) + 4 > static_cast<int64_t>(decoded.size())) {
            error = "invalid parquet definition level length";
            status = Status::CORRUPTED_FILE;
            break;
          }
          levels.assign(decoded.data() + 4, length);
          payload.assign(decoded.data() + 4 + length, decoded.size() - 4 - length);
        } else {
          payload = decoded;
        }
      } else {
        const int64_t prefix = static_cast<int64_t>(page.repetition_levels_byte_length_) +
                               page.definition_levels_byte_length_;
        if (prefix < 0 || prefix > static_cast<int64_t>(page_bytes.size())) {
          error = "invalid parquet v2 page";
          status = Status::CORRUPTED_FILE;
          break;
        }
        if (optional) {
          levels.assign(page_bytes.data() + page.repetition_levels_byte_length_,
                        page.definition_levels_byte_length_);
        }
        const std::string body(page_bytes.data() + prefix, page_bytes.size() - prefix);
        if (!page.is_compressed_ || 0 == chunk.codec_) {
          payload = body;
        } else if (!snappy_uncompress(reinterpret_cast<const uint8_t *>(body.data()),
                                      static_cast<int64_t>(body.size()), payload, error)) {
          status = Status::UNSUPPORTED;
          break;
        }
      }
      std::vector<uint32_t> definitions;
      int64_t present = page.num_values_;
      if (optional) {
        int64_t consumed = 0;
        if (!decode_hybrid(reinterpret_cast<const uint8_t *>(levels.data()),
                           static_cast<int64_t>(levels.size()), 1, page.num_values_, definitions,
                           consumed)) {
          error = "cannot decode parquet definition levels";
          status = Status::CORRUPTED_FILE;
          break;
        }
        present = 0;
        for (size_t i = 0; i < definitions.size(); ++i) {
          if (definitions[i] >= 1) {
            ++present;
          }
        }
      }
      std::vector<std::string> texts;
      if (has_dictionary && (2 == page.encoding_ || 8 == page.encoding_)) {
        if (payload.empty()) {
          error = "invalid parquet dictionary index page";
          status = Status::CORRUPTED_FILE;
          break;
        }
        const int32_t width = static_cast<uint8_t>(payload[0]);
        std::vector<uint32_t> indexes;
        int64_t consumed = 0;
        if (!decode_hybrid(reinterpret_cast<const uint8_t *>(payload.data()) + 1,
                           static_cast<int64_t>(payload.size()) - 1, width, present, indexes,
                           consumed)) {
          error = "cannot decode parquet dictionary indexes";
          status = Status::CORRUPTED_FILE;
          break;
        }
        for (size_t i = 0; Status::OK == status && i < indexes.size(); ++i) {
          if (indexes[i] >= dictionary.size()) {
            error = "parquet dictionary index out of range";
            status = Status::CORRUPTED_FILE;
          } else {
            texts.push_back(dictionary[indexes[i]]);
          }
        }
      } else if (0 == page.encoding_) {
        if (!decode_plain_values(reinterpret_cast<const uint8_t *>(payload.data()),
                                 static_cast<int64_t>(payload.size()), chunk.physical_type_,
                                 column_type, datetime_scale, present, texts)) {
          error = "cannot decode parquet plain values";
          status = Status::CORRUPTED_FILE;
        }
      } else if (3 == page.encoding_ && ColumnType::BOOLEAN == column_type) {
        // Boolean values may use the RLE hybrid encoding with a one bit width.
        // A data page prefixes the run data with its four byte length.
        int64_t prefix = 0;
        if (payload.size() >= 4) {
          const uint32_t length = static_cast<uint32_t>(static_cast<uint8_t>(payload[0])) |
                                  (static_cast<uint32_t>(static_cast<uint8_t>(payload[1])) << 8) |
                                  (static_cast<uint32_t>(static_cast<uint8_t>(payload[2])) << 16) |
                                  (static_cast<uint32_t>(static_cast<uint8_t>(payload[3])) << 24);
          if (length > 0 && static_cast<int64_t>(length) + 4 <= static_cast<int64_t>(payload.size())) {
            prefix = 4;
          }
        }
        std::vector<uint32_t> bits;
        int64_t consumed = 0;
        if (!decode_hybrid(reinterpret_cast<const uint8_t *>(payload.data()) + prefix,
                           static_cast<int64_t>(payload.size()) - prefix, 1, present, bits,
                           consumed)) {
          error = "cannot decode parquet RLE boolean values";
          status = Status::CORRUPTED_FILE;
        } else {
          for (size_t i = 0; i < bits.size(); ++i) {
            texts.push_back(0 != bits[i] ? "true" : "false");
          }
        }
      } else {
        error = "parquet value encoding " + std::to_string(page.encoding_) +
                " is not supported (dictionary=" + (has_dictionary ? "yes" : "no") + ")";
        status = Status::UNSUPPORTED;
      }
      if (Status::OK != status) {
        break;
      }
      size_t text_index = 0;
      for (int64_t i = 0; i < page.num_values_; ++i) {
        const bool is_null = optional && definitions[static_cast<size_t>(i)] < 1;
        values.null_flags_.push_back(is_null ? 1 : 0);
        if (is_null) {
          values.texts_.push_back(std::string());
        } else if (text_index < texts.size()) {
          values.texts_.push_back(texts[text_index++]);
        } else {
          error = "parquet page has fewer values than its row count";
          status = Status::CORRUPTED_FILE;
          break;
        }
      }
    }
    return status;
  }

  std::string path_;
  const std::vector<Column> &columns_;
  const std::vector<int64_t> &needed_;
  FILE *fp_;
  ParquetMeta meta_;
  std::vector<RawValues> group_values_;
  int64_t row_group_index_;
  int64_t row_in_group_;
};

// ---------------------------------------------------------------------------
// glob
// ---------------------------------------------------------------------------

bool wildcard_match(const char *pattern, const char *text)
{
  while ('\0' != *pattern) {
    if ('*' == *pattern) {
      while ('*' == *pattern) {
        ++pattern;
      }
      if ('\0' == *pattern) {
        return true;
      }
      for (const char *rest = text; ; ++rest) {
        if (wildcard_match(pattern, rest)) {
          return true;
        }
        if ('\0' == *rest) {
          return false;
        }
      }
    } else if ('?' == *pattern) {
      if ('\0' == *text) {
        return false;
      }
      ++pattern;
      ++text;
    } else if ('[' == *pattern) {
      const char *scan = pattern + 1;
      bool negate = false;
      if ('!' == *scan || '^' == *scan) {
        negate = true;
        ++scan;
      }
      bool matched = false;
      bool first = true;
      while ('\0' != *scan && (']' != *scan || first)) {
        first = false;
        char low = *scan++;
        char high = low;
        if ('-' == *scan && '\0' != scan[1] && ']' != scan[1]) {
          high = scan[1];
          scan += 2;
        }
        if ('\0' != *text && *text >= low && *text <= high) {
          matched = true;
        }
      }
      if (']' != *scan) {
        return false;
      }
      if ('\0' == *text) {
        return false;
      }
      if (matched == negate) {
        return false;
      }
      ++text;
      pattern = scan + 1;
    } else {
      if (*pattern != *text) {
        return false;
      }
      ++pattern;
      ++text;
    }
  }
  return '\0' == *text;
}

class GlobReader : public IRecordReader
{
public:
  explicit GlobReader(const std::vector<std::string> &files) : files_(files), position_(0) {}

  Status open(std::string &) override
  {
    position_ = 0;
    return Status::OK;
  }

  Status next(Row &row, bool &eof, std::string &error) override
  {
    Status status = Status::OK;
    eof = false;
    if (position_ >= static_cast<int64_t>(files_.size())) {
      eof = true;
    } else {
      const std::string &file = files_[static_cast<size_t>(position_)];
      ++position_;
      if (row.cells_.size() != 1) {
        row.cells_.resize(1);
      }
      row.row_number_ = position_;
      status = convert_text(file.data(), static_cast<int64_t>(file.size()), false,
                            ColumnType::VARCHAR, row.cells_[0], error);
    }
    return status;
  }

  void close() override { position_ = 0; }

private:
  const std::vector<std::string> &files_;
  int64_t position_;
};

// ---------------------------------------------------------------------------
// Schema inference
// ---------------------------------------------------------------------------

Status infer_csv(const std::string &path, const Options &options, std::vector<Column> &columns,
                 std::string &error)
{
  Status status = Status::OK;
  FileStream stream;
  status = stream.open(path, error);
  std::vector<std::string> fields;
  bool eof = false;
  for (int64_t i = 0; Status::OK == status && i < options.skip_rows_; ++i) {
    status = read_csv_record(stream, options, fields, eof, error);
    if (Status::OK == status && eof) {
      error = "file ended while skipping rows";
      status = Status::PARSE_ERROR;
    }
  }
  if (Status::OK == status) {
    status = read_csv_record(stream, options, fields, eof, error);
    if (Status::OK == status && eof) {
      error = "file is empty";
      status = Status::PARSE_ERROR;
    }
  }
  if (Status::OK == status && !fields.empty()) {
    // A UTF-8 BOM belongs to the first header name, not to the data.
    if (fields[0].size() >= 3 && static_cast<unsigned char>(fields[0][0]) == 0xEF &&
        static_cast<unsigned char>(fields[0][1]) == 0xBB &&
        static_cast<unsigned char>(fields[0][2]) == 0xBF) {
      fields[0].erase(0, 3);
    }
  }
  if (Status::OK == status) {
    const int64_t width = static_cast<int64_t>(fields.size());
    if (width <= 0) {
      error = "file has no columns";
      status = Status::PARSE_ERROR;
    } else if (width > MAX_FILE_COLUMNS) {
      error = "file has more columns than supported";
      status = Status::TOO_LARGE;
    } else {
      columns.clear();
      columns.reserve(static_cast<size_t>(width));
      for (int64_t i = 0; i < width; ++i) {
        Column column;
        const std::string &name = options.header_ ? fields[static_cast<size_t>(i)]
                                                  : std::string();
        column.name_ = unique_column_name(columns, name, i);
        column.type_ = ColumnType::SQL_NULL;
        columns.push_back(column);
      }
    }
  }
  if (Status::OK == status && !options.header_) {
    for (int64_t i = 0; Status::OK == status && i < static_cast<int64_t>(columns.size()); ++i) {
      const std::string &field = fields[static_cast<size_t>(i)];
      const bool is_null = !options.null_string_.empty() && field == options.null_string_;
      if (!is_null) {
        columns[static_cast<size_t>(i)].type_ =
            merge_column_type(columns[static_cast<size_t>(i)].type_,
                              detect_text_type(field.data(), static_cast<int64_t>(field.size())));
      }
    }
  }
  while (Status::OK == status) {
    status = read_csv_record(stream, options, fields, eof, error);
    if (Status::OK != status || eof) {
      break;
    }
    if (static_cast<int64_t>(fields.size()) != static_cast<int64_t>(columns.size())) {
      error = "a data row has " + std::to_string(fields.size()) + " fields but " +
              std::to_string(columns.size()) + " columns were inferred";
      status = Status::PARSE_ERROR;
      break;
    }
    for (int64_t i = 0; Status::OK == status && i < static_cast<int64_t>(columns.size()); ++i) {
      const std::string &field = fields[static_cast<size_t>(i)];
      const bool is_null = !options.null_string_.empty() && field == options.null_string_;
      if (!is_null) {
        columns[static_cast<size_t>(i)].type_ =
            merge_column_type(columns[static_cast<size_t>(i)].type_,
                              detect_text_type(field.data(), static_cast<int64_t>(field.size())));
      }
    }
  }
  stream.close();
  return status;
}

Status infer_json(const std::string &path, std::vector<Column> &columns, std::string &error)
{
  Status status = Status::OK;
  FileStream stream;
  status = stream.open(path, error);
  columns.clear();
  std::string line;
  int64_t line_number = 0;
  std::vector<JsonMember> members;
  while (Status::OK == status) {
    bool eof = false;
    status = next_text_line(stream, line, eof, error);
    if (Status::OK != status || eof) {
      break;
    }
    if (line.empty()) {
      continue;
    }
    ++line_number;
    JsonCursor cursor(line.data(), static_cast<int64_t>(line.size()));
    members.clear();
    status = cursor.parse_object(members, error);
    if (Status::PARSE_ERROR == status) {
      error = "line " + std::to_string(line_number) + ": " + error;
      break;
    }
    for (size_t i = 0; Status::OK == status && i < members.size(); ++i) {
      const JsonMember &member = members[i];
      int64_t found = -1;
      for (size_t c = 0; c < columns.size() && found < 0; ++c) {
        if (columns[c].name_ == member.name_) {
          found = static_cast<int64_t>(c);
        }
      }
      if (found < 0) {
        if (static_cast<int64_t>(columns.size()) >= MAX_FILE_COLUMNS) {
          error = "document has more columns than supported";
          status = Status::TOO_LARGE;
          break;
        }
        Column column;
        column.name_ = member.name_;
        column.type_ = ColumnType::SQL_NULL;
        columns.push_back(column);
        found = static_cast<int64_t>(columns.size()) - 1;
      }
      if (!member.is_null_) {
        columns[static_cast<size_t>(found)].type_ =
            merge_column_type(columns[static_cast<size_t>(found)].type_,
                              detect_text_type(member.text_.data(),
                                               static_cast<int64_t>(member.text_.size())));
      }
    }
  }
  if (Status::OK == status && columns.empty()) {
    error = "file has no columns";
    status = Status::PARSE_ERROR;
  }
  stream.close();
  return status;
}

Status infer_parquet(const std::string &path, std::vector<Column> &columns, std::string &error)
{
  Status status = Status::OK;
  FILE *fp = fopen(path.c_str(), "rb");
  columns.clear();
  if (nullptr == fp) {
    const int saved = errno;
    error = std::string("cannot open file '") + path + "': " + strerror(saved);
    status = (ENOENT == saved) ? Status::FILE_NOT_FOUND : Status::OPEN_FAILED;
  } else {
    ParquetMeta meta;
    status = load_parquet_meta(fp, meta, &columns, error);
    fclose(fp);
  }
  return status;
}

} // namespace

bool is_glob_pattern(const std::string &path)
{
  return path.find_first_of("*?[") != std::string::npos;
}

Status expand_glob(const std::string &pattern, std::vector<std::string> &files, std::string &error)
{
  Status status = Status::OK;
  files.clear();
  if (!is_glob_pattern(pattern)) {
    struct stat file_stat;
    if (0 != stat(pattern.c_str(), &file_stat)) {
      error = std::string("file not found: ") + pattern;
      status = Status::FILE_NOT_FOUND;
    } else if (!S_ISREG(file_stat.st_mode)) {
      error = std::string("not a regular file: ") + pattern;
      status = Status::INVALID_OPTION;
    } else {
      files.push_back(pattern);
    }
  } else {
    const size_t slash = pattern.rfind('/');
    const std::string directory = std::string::npos == slash ? std::string(".")
                                                             : pattern.substr(0, slash);
    const std::string name_pattern = std::string::npos == slash ? pattern
                                                                : pattern.substr(slash + 1);
    DIR *dir = opendir(directory.c_str());
    if (nullptr == dir) {
      error = std::string("cannot open directory: ") + directory;
      status = Status::FILE_NOT_FOUND;
    } else {
      for (;;) {
        struct dirent *entry = readdir(dir);
        if (nullptr == entry) {
          break;
        }
        const std::string name(entry->d_name);
        if ("." == name || ".." == name) {
          continue;
        }
        if (!wildcard_match(name_pattern.c_str(), name.c_str())) {
          continue;
        }
        const std::string full = ("." == directory) ? name : (directory + "/" + name);
        struct stat file_stat;
        if (0 == stat(full.c_str(), &file_stat) && S_ISREG(file_stat.st_mode)) {
          if (static_cast<int64_t>(files.size()) >= MAX_GLOB_FILES) {
            error = "glob matches too many files";
            status = Status::TOO_LARGE;
            break;
          }
          files.push_back(full);
        }
      }
      closedir(dir);
      if (Status::OK == status) {
        std::sort(files.begin(), files.end());
        if (files.empty()) {
          error = std::string("glob pattern matched no file: ") + pattern;
          status = Status::FILE_NOT_FOUND;
        }
      }
    }
  }
  return status;
}

Status canonicalize_path(const std::string &path, std::string &canonical, std::string &error)
{
  Status status = Status::OK;
  char buffer[4096];
  canonical.clear();
#ifdef _WIN32
  if (nullptr == _fullpath(buffer, path.c_str(), sizeof(buffer))) {
    error = std::string("cannot resolve path: ") + path;
    status = Status::FILE_NOT_FOUND;
  }
#else
  if (nullptr == realpath(path.c_str(), buffer)) {
    error = std::string("cannot resolve path: ") + path + ": " + strerror(errno);
    status = Status::FILE_NOT_FOUND;
  }
#endif
  if (Status::OK == status) {
    canonical.assign(buffer);
  }
  return status;
}

Status stat_file(const std::string &path, int64_t &size, int64_t &modify_time, std::string &error)
{
  Status status = Status::OK;
  size = 0;
  modify_time = 0;
#ifdef _WIN32
  struct _stat64 file_stat;
  if (0 != _stat64(path.c_str(), &file_stat)) {
    error = std::string("cannot stat file: ") + path;
    status = Status::FILE_NOT_FOUND;
  } else {
    size = static_cast<int64_t>(file_stat.st_size);
    modify_time = static_cast<int64_t>(file_stat.st_mtime);
  }
#else
  struct stat file_stat;
  if (0 != stat(path.c_str(), &file_stat)) {
    error = std::string("cannot stat file: ") + path;
    status = Status::FILE_NOT_FOUND;
  } else {
    size = static_cast<int64_t>(file_stat.st_size);
    modify_time = static_cast<int64_t>(file_stat.st_mtime);
  }
#endif
  return status;
}

Status infer_schema(const std::string &path, const Options &options, std::vector<Column> &columns,
                    std::string &error)
{
  Status status = Status::OK;
  columns.clear();
  if (FileFormat::CSV == options.format_) {
    status = infer_csv(path, options, columns, error);
  } else if (FileFormat::JSON == options.format_ || FileFormat::NDJSON == options.format_) {
    status = infer_json(path, columns, error);
  } else if (FileFormat::PARQUET == options.format_) {
    status = infer_parquet(path, columns, error);
  } else {
    error = "unsupported file format";
    status = Status::INVALID_OPTION;
  }
  if (Status::OK == status && columns.empty()) {
    error = "no columns were inferred";
    status = Status::PARSE_ERROR;
  }
  return status;
}

Status create_reader(const std::string &path, const Options &options,
                     const std::vector<Column> &columns, const std::vector<int64_t> &needed_columns,
                     IRecordReader *&reader, std::string &error)
{
  Status status = Status::OK;
  reader = nullptr;
  for (size_t i = 0; i < needed_columns.size(); ++i) {
    if (needed_columns[i] < 0 || needed_columns[i] >= static_cast<int64_t>(columns.size())) {
      error = "invalid column projection";
      status = Status::INVALID_OPTION;
    }
  }
  if (Status::OK == status) {
    if (FileFormat::CSV == options.format_) {
      reader = new (std::nothrow) CsvReader(path, options, columns, needed_columns);
    } else if (FileFormat::JSON == options.format_ || FileFormat::NDJSON == options.format_) {
      reader = new (std::nothrow) JsonLineReader(path, columns, needed_columns);
    } else if (FileFormat::PARQUET == options.format_) {
      reader = new (std::nothrow) ParquetReader(path, columns, needed_columns);
    } else {
      error = "unsupported file format";
      status = Status::INVALID_OPTION;
    }
    if (Status::OK == status && nullptr == reader) {
      error = "cannot allocate the file reader";
      status = Status::OPEN_FAILED;
    }
  }
  return status;
}

void destroy_reader(IRecordReader *reader)
{
  delete reader;
}

Status create_glob_reader(const std::vector<std::string> &files, IRecordReader *&reader,
                          std::string &error)
{
  Status status = Status::OK;
  reader = new (std::nothrow) GlobReader(files);
  if (nullptr == reader) {
    error = "cannot allocate the glob reader";
    status = Status::OPEN_FAILED;
  }
  return status;
}

// ---------------------------------------------------------------------------
// Bind time snapshot encoding
// ---------------------------------------------------------------------------

namespace
{

const char OPTIONS_MAGIC[4] = {'F', 'S', 'Q', 'L'};
constexpr int64_t OPTIONS_VERSION = 1;

void append_u8(std::string &buffer, const uint8_t value)
{
  buffer.push_back(static_cast<char>(value));
}

void append_u32(std::string &buffer, const uint32_t value)
{
  for (int i = 0; i < 4; ++i) {
    buffer.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
  }
}

void append_i64(std::string &buffer, const int64_t value)
{
  const uint64_t raw = static_cast<uint64_t>(value);
  for (int i = 0; i < 8; ++i) {
    buffer.push_back(static_cast<char>((raw >> (8 * i)) & 0xFF));
  }
}

void append_string(std::string &buffer, const std::string &value)
{
  append_u32(buffer, static_cast<uint32_t>(value.size()));
  buffer.append(value);
}

class BlobReader
{
public:
  BlobReader(const char *data, const int64_t size) : data_(data), size_(size), pos_(0) {}

  bool read_u8(uint8_t &value)
  {
    bool ok = pos_ < size_;
    if (ok) {
      value = static_cast<uint8_t>(data_[pos_++]);
    }
    return ok;
  }

  bool read_u32(uint32_t &value)
  {
    bool ok = pos_ + 4 <= size_;
    if (ok) {
      value = 0;
      for (int i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(static_cast<uint8_t>(data_[pos_ + i])) << (8 * i);
      }
      pos_ += 4;
    }
    return ok;
  }

  bool read_i64(int64_t &value)
  {
    bool ok = pos_ + 8 <= size_;
    if (ok) {
      uint64_t raw = 0;
      for (int i = 0; i < 8; ++i) {
        raw |= static_cast<uint64_t>(static_cast<uint8_t>(data_[pos_ + i])) << (8 * i);
      }
      pos_ += 8;
      value = static_cast<int64_t>(raw);
    }
    return ok;
  }

  bool read_string(std::string &value)
  {
    uint32_t length = 0;
    bool ok = read_u32(length) && pos_ + static_cast<int64_t>(length) <= size_;
    if (ok) {
      value.assign(data_ + pos_, length);
      pos_ += static_cast<int64_t>(length);
    }
    return ok;
  }

  bool at_end() const { return pos_ == size_; }

private:
  const char *data_;
  int64_t size_;
  int64_t pos_;
};

} // namespace

std::string encode_options(const Options &options, const std::string &path,
                           const std::vector<Column> &columns, const int64_t file_size,
                           const int64_t modify_time, const std::vector<std::string> &files)
{
  std::string buffer;
  buffer.append(OPTIONS_MAGIC, 4);
  append_u8(buffer, static_cast<uint8_t>(OPTIONS_VERSION));
  append_u8(buffer, static_cast<uint8_t>(options.format_));
  append_u8(buffer, static_cast<uint8_t>(options.delimiter_));
  append_u8(buffer, static_cast<uint8_t>(options.quote_));
  append_u8(buffer, static_cast<uint8_t>(options.escape_));
  append_u8(buffer, options.header_ ? 1 : 0);
  append_i64(buffer, options.skip_rows_);
  append_i64(buffer, options.sample_size_);
  append_i64(buffer, file_size);
  append_i64(buffer, modify_time);
  append_string(buffer, path);
  append_string(buffer, options.null_string_);
  append_u32(buffer, static_cast<uint32_t>(columns.size()));
  for (size_t i = 0; i < columns.size(); ++i) {
    append_u8(buffer, static_cast<uint8_t>(columns[i].type_));
    append_string(buffer, columns[i].name_);
  }
  append_u32(buffer, static_cast<uint32_t>(files.size()));
  for (size_t i = 0; i < files.size(); ++i) {
    append_string(buffer, files[i]);
  }
  return buffer;
}

Status decode_options(const char *data, const int64_t length, Options &options, std::string &path,
                      std::vector<Column> &columns, int64_t &file_size, int64_t &modify_time,
                      std::vector<std::string> &files)
{
  Status status = Status::OK;
  columns.clear();
  files.clear();
  if (nullptr == data || length < 5 || 0 != memcmp(data, OPTIONS_MAGIC, 4) ||
      static_cast<uint8_t>(OPTIONS_VERSION) != static_cast<uint8_t>(data[4])) {
    status = Status::INVALID_OPTION;
  } else {
    BlobReader reader(data + 5, length - 5);
    uint8_t format = 0;
    uint8_t delimiter = 0;
    uint8_t quote = 0;
    uint8_t escape = 0;
    uint8_t header = 0;
    uint32_t column_count = 0;
    uint32_t file_count = 0;
    if (!reader.read_u8(format) || !reader.read_u8(delimiter) || !reader.read_u8(quote) ||
        !reader.read_u8(escape) || !reader.read_u8(header) || !reader.read_i64(options.skip_rows_) ||
        !reader.read_i64(options.sample_size_) || !reader.read_i64(file_size) ||
        !reader.read_i64(modify_time) || !reader.read_string(path) ||
        !reader.read_string(options.null_string_) || !reader.read_u32(column_count)) {
      status = Status::INVALID_OPTION;
    } else {
      options.format_ = static_cast<FileFormat>(format);
      options.delimiter_ = static_cast<char>(delimiter);
      options.quote_ = static_cast<char>(quote);
      options.escape_ = static_cast<char>(escape);
      options.header_ = 0 != header;
      for (uint32_t i = 0; Status::OK == status && i < column_count; ++i) {
        uint8_t type = 0;
        Column column;
        if (!reader.read_u8(type) || !reader.read_string(column.name_)) {
          status = Status::INVALID_OPTION;
        } else {
          column.type_ = static_cast<ColumnType>(type);
          columns.push_back(column);
        }
      }
      if (Status::OK == status && !reader.read_u32(file_count)) {
        status = Status::INVALID_OPTION;
      }
      for (uint32_t i = 0; Status::OK == status && i < file_count; ++i) {
        std::string file;
        if (!reader.read_string(file)) {
          status = Status::INVALID_OPTION;
        } else {
          files.push_back(file);
        }
      }
    }
  }
  return status;
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
