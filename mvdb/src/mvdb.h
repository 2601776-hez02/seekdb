// MiniVectorDB - common definitions and small utilities.
// C++17, no external dependencies beyond the standard library.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <queue>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>

namespace mvdb {

// ---------------------------------------------------------------------------
// Error semantics (snake_case names used by the test assertions)
// ---------------------------------------------------------------------------
enum class Err : uint32_t {
  OK = 0,
  SyntaxError,          // syntax_error
  UnsupportedSQL,       // unsupported_sql
  TableNotFound,        // table_not_found
  ColumnNotFound,       // column_not_found
  DuplicateTable,       // duplicate_table
  DuplicateKey,         // duplicate_key
  DuplicateIndex,       // duplicate_index
  IndexNotFound,        // index_not_found
  NotVectorColumn,      // not_vector_column
  DimensionMismatch,    // dimension_mismatch
  InvalidVector,        // invalid_vector
  ValueOverflow,        // value_overflow
  StringTooLong,        // string_too_long
  TxnState,             // txn_state
  IOError,              // io_error
  InternalError,        // internal_error
};

inline const char* err_name(Err e) {
  switch (e) {
    case Err::OK: return "ok";
    case Err::SyntaxError: return "syntax_error";
    case Err::UnsupportedSQL: return "unsupported_sql";
    case Err::TableNotFound: return "table_not_found";
    case Err::ColumnNotFound: return "column_not_found";
    case Err::DuplicateTable: return "duplicate_table";
    case Err::DuplicateKey: return "duplicate_key";
    case Err::DuplicateIndex: return "duplicate_index";
    case Err::IndexNotFound: return "index_not_found";
    case Err::NotVectorColumn: return "not_vector_column";
    case Err::DimensionMismatch: return "dimension_mismatch";
    case Err::InvalidVector: return "invalid_vector";
    case Err::ValueOverflow: return "value_overflow";
    case Err::StringTooLong: return "string_too_long";
    case Err::TxnState: return "txn_state";
    case Err::IOError: return "io_error";
    default: return "internal_error";
  }
}

// ---------------------------------------------------------------------------
// Column types
// ---------------------------------------------------------------------------
enum class ColType : uint8_t { BigInt = 1, VarChar = 2, Vector = 3 };

struct Column {
  std::string name;
  ColType type = ColType::BigInt;
  int64_t n = 0;      // VARCHAR: max chars (unicode codepoints); VECTOR: dims
  bool is_pk = false;
};

struct TableSchema {
  std::string name;
  int64_t id = -1;
  std::vector<Column> cols;
  int pk_col = -1;        // index of the primary key column
  int vec_col = -1;       // index of the vector column, -1 if none
  // vector index (persisted): name + build params
  std::string vec_index_name;
  int vec_index_M = 16;
  int vec_index_efC = 200;
  int vec_index_efS = 1024;
  bool has_index() const { return !vec_index_name.empty(); }
};

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------
inline uint32_t crc32c_hw(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu);
inline uint32_t crc32_soft(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu) {
  // CRC-32 (IEEE) software implementation, init 0xFFFFFFFF.
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    init = true;
  }
  crc ^= 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

inline uint32_t crc32(const void* data, size_t n) {
  return crc32_soft(static_cast<const uint8_t*>(data), n);
}

// Parse a signed 64-bit integer; returns false on overflow / bad syntax.
inline bool parse_i64(const std::string& s, int64_t& out) {
  if (s.empty()) return false;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '+' || s[0] == '-') { neg = (s[0] == '-'); i = 1; }
  if (i >= s.size()) return false;
  uint64_t v = 0;
  for (; i < s.size(); i++) {
    char c = s[i];
    if (c < '0' || c > '9') return false;
    uint64_t d = uint64_t(c - '0');
    if (v > (UINT64_MAX - d) / 10) return false;  // overflow
    v = v * 10 + d;
  }
  if (neg) {
    if (v > (uint64_t(INT64_MAX) + 1)) return false;
    out = (v == uint64_t(INT64_MAX) + 1) ? INT64_MIN : -int64_t(v);
  } else {
    if (v > uint64_t(INT64_MAX)) return false;
    out = int64_t(v);
  }
  return true;
}

// Parse a double; strict-ish: rejects empty, NaN, Inf; returns false on failure.
inline bool parse_f64(const std::string& s, double& out) {
  if (s.empty()) return false;
  for (char c : s) {
    if (c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E') continue;
    if (c < '0' || c > '9') return false;
  }
  // strtod handles overflow to inf; we reject inf/nan afterwards.
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0') return false;
  if (!std::isfinite(v)) return false;
  out = v;
  return true;
}

// Count unicode codepoints in a UTF-8 string (tolerant: invalid bytes count as 1).
inline int64_t utf8_chars(const std::string& s) {
  int64_t n = 0;
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { i += 1; }
    else if ((c & 0xE0) == 0xC0) { i += std::min<size_t>(2, s.size() - i); }
    else if ((c & 0xF0) == 0xE0) { i += std::min<size_t>(3, s.size() - i); }
    else if ((c & 0xF8) == 0xF0) { i += std::min<size_t>(4, s.size() - i); }
    else { i += 1; }
    n++;
  }
  return n;
}

inline std::string to_upper(std::string s) {
  for (char& c : s) c = char(toupper((unsigned char)c));
  return s;
}
inline std::string to_lower(std::string s) {
  for (char& c : s) c = char(tolower((unsigned char)c));
  return s;
}

inline bool ieq(const std::string& a, const std::string& b) {
  return to_upper(a) == to_upper(b);
}

// ---------------------------------------------------------------------------
// Row serialization
//   payload = concatenation of every column EXCEPT the primary key column,
//   in schema order: BIGINT 8B LE | VARCHAR: u32 len + utf8 bytes | VECTOR d*4B
//   A full row on disk / WAL = [pk 8B LE][payload]
// ---------------------------------------------------------------------------
inline void put_u8(std::string& s, uint8_t v) { s.push_back(char(v)); }
inline void put_u32(std::string& s, uint32_t v) {
  s.push_back(char(v & 0xFF)); s.push_back(char((v >> 8) & 0xFF));
  s.push_back(char((v >> 16) & 0xFF)); s.push_back(char((v >> 24) & 0xFF));
}
inline void put_u64(std::string& s, uint64_t v) {
  for (int i = 0; i < 8; i++) s.push_back(char((v >> (8 * i)) & 0xFF));
}
inline void put_i64(std::string& s, int64_t v) { put_u64(s, uint64_t(v)); }
inline void put_f32(std::string& s, float v) {
  uint32_t u; std::memcpy(&u, &v, 4);
  put_u32(s, u);
}
inline void put_str(std::string& s, const std::string& v) {
  put_u32(s, uint32_t(v.size()));
  s.append(v);
}

struct ByteReader {
  const uint8_t* p;
  size_t n;
  size_t pos = 0;
  ByteReader(const char* d, size_t len) : p((const uint8_t*)d), n(len) {}
  bool ok() const { return pos <= n; }
  uint8_t u8() { if (pos + 1 > n) { pos = n + 1; return 0; } return p[pos++]; }
  uint32_t u32() {
    if (pos + 4 > n) { pos = n + 1; return 0; }
    uint32_t v = uint32_t(p[pos]) | (uint32_t(p[pos+1]) << 8) |
                 (uint32_t(p[pos+2]) << 16) | (uint32_t(p[pos+3]) << 24);
    pos += 4; return v;
  }
  uint64_t u64() {
    if (pos + 8 > n) { pos = n + 1; return 0; }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= uint64_t(p[pos + i]) << (8 * i);
    pos += 8; return v;
  }
  int64_t i64() { return int64_t(u64()); }
  float f32() { uint32_t v = u32(); float f; std::memcpy(&f, &v, 4); return f; }
  std::string str() {
    uint32_t len = u32();
    if (pos + len > n) { pos = n + 1; return ""; }
    std::string s((const char*)(p + pos), len);
    pos += len;
    return s;
  }
  bool Skip(size_t k) {
    if (pos + k > n) { pos = n + 1; return false; }
    pos += k;
    return true;
  }
  // read a u32 length prefix and skip that many bytes (varchar w/o copy)
  bool SkipStr() {
    uint32_t len = u32();
    return Skip(len);
  }
  bool eof() const { return pos >= n; }
};

// Encode a row payload (all columns except pk) from parsed values.
struct Value {
  enum class T : uint8_t { BigInt, F64, Str, Vec } t = T::BigInt;
  int64_t i = 0;
  double f = 0;
  std::string s;
  std::vector<float> v;
  bool isNull() const { return t == T::BigInt && i == 0 && s.empty() && v.empty(); }
};

struct Row {
  int64_t pk = 0;
  std::vector<Value> vals;  // aligned with schema columns (vals[pk_col] unused placeholder)
};

// serialize values (schema order, skipping pk col) into payload
inline Err encode_payload(const TableSchema& sch, const std::vector<Value>& vals,
                          std::string& out) {
  out.clear();
  for (size_t ci = 0; ci < sch.cols.size(); ci++) {
    if (int64_t(ci) == sch.pk_col) continue;
    const Column& c = sch.cols[ci];
    const Value& v = vals[ci];
    switch (c.type) {
      case ColType::BigInt: {
        if (v.t == Value::T::BigInt) {
          put_i64(out, v.i);
        } else if (v.t == Value::T::F64) {
          // MySQL-style cast: round half away from zero; reject overflow.
          if (v.f < -9.2233720368547758e18 || v.f > 9.2233720368547758e18)
            return Err::ValueOverflow;
          long double r = std::round(v.f);
          if (r < -9223372036854775808.0L || r > 9223372036854775807.0L)
            return Err::ValueOverflow;
          put_i64(out, int64_t(r));
        } else {
          return Err::ValueOverflow;
        }
        break;
      }
      case ColType::VarChar: {
        if (v.t != Value::T::Str) return Err::InternalError;
        if (utf8_chars(v.s) > c.n) return Err::StringTooLong;
        put_str(out, v.s);
        break;
      }
      case ColType::Vector: {
        if (v.t != Value::T::Vec) return Err::InternalError;
        if (int64_t(v.v.size()) != c.n) return Err::DimensionMismatch;
        for (float f : v.v) put_f32(out, f);
        break;
      }
      default: return Err::InternalError;
    }
  }
  return Err::OK;
}

inline Err decode_payload(const TableSchema& sch, const char* data, size_t len,
                          std::vector<Value>& vals, int64_t pk) {
  ByteReader r(data, len);
  vals.assign(sch.cols.size(), Value());
  if (sch.pk_col >= 0 && size_t(sch.pk_col) < vals.size()) {
    vals[size_t(sch.pk_col)].t = Value::T::BigInt;
    vals[size_t(sch.pk_col)].i = pk;
  }
  for (size_t ci = 0; ci < sch.cols.size(); ci++) {
    if (int64_t(ci) == sch.pk_col) continue;
    const Column& c = sch.cols[ci];
    switch (c.type) {
      case ColType::BigInt: vals[ci].t = Value::T::BigInt; vals[ci].i = r.i64(); break;
      case ColType::VarChar: vals[ci].t = Value::T::Str; vals[ci].s = r.str(); break;
      case ColType::Vector: {
        size_t needBytes = size_t(c.n) * 4;
        if (r.pos + needBytes > r.n) return Err::InternalError;
        vals[ci].t = Value::T::Vec;
        vals[ci].v.resize(size_t(c.n));
        std::memcpy(vals[ci].v.data(), r.p + r.pos, needBytes);
        r.pos += needBytes;
        break;
      }
      default: return Err::InternalError;
    }
    if (!r.ok()) return Err::InternalError;
  }
  return Err::OK;
}

// Decode only the columns selected by `mask` (bit per schema column index,
// empty mask = all); other columns are skipped without copying strings.
// The primary key column is always filled.
inline Err decode_payload_masked(const TableSchema& sch, const char* data, size_t len,
                                 std::vector<Value>& vals, int64_t pk,
                                 const std::vector<uint8_t>& mask) {
  ByteReader r(data, len);
  vals.assign(sch.cols.size(), Value());
  if (sch.pk_col >= 0 && size_t(sch.pk_col) < vals.size()) {
    vals[size_t(sch.pk_col)].t = Value::T::BigInt;
    vals[size_t(sch.pk_col)].i = pk;
  }
  for (size_t ci = 0; ci < sch.cols.size(); ci++) {
    if (int64_t(ci) == sch.pk_col) continue;
    const Column& c = sch.cols[ci];
    bool need = mask.empty() || (ci < mask.size() && mask[ci]);
    switch (c.type) {
      case ColType::BigInt: {
        int64_t v = r.i64();
        if (need) { vals[ci].t = Value::T::BigInt; vals[ci].i = v; }
        break;
      }
      case ColType::VarChar: {
        if (need) {
          vals[ci].t = Value::T::Str;
          vals[ci].s = r.str();
        } else if (!r.SkipStr()) {
          return Err::InternalError;
        }
        break;
      }
      case ColType::Vector: {
        size_t needBytes = size_t(c.n) * 4;
        if (need) {
          if (r.pos + needBytes > r.n) return Err::InternalError;
          vals[ci].t = Value::T::Vec;
          vals[ci].v.resize(size_t(c.n));
          std::memcpy(vals[ci].v.data(), r.p + r.pos, needBytes);
          r.pos += needBytes;
        } else if (!r.Skip(needBytes)) {
          return Err::InternalError;
        }
        break;
      }
      default: return Err::InternalError;
    }
    if (!r.ok()) return Err::InternalError;
  }
  return Err::OK;
}

// Vector literal: '[1, 2, -3.5]' -> floats; NaN/Inf rejected; bad syntax rejected.
inline Err parse_vec_literal(const std::string& text, std::vector<float>& out) {
  out.clear();
  std::string s = text;
  // trim
  size_t b = s.find_first_not_of(" \t\r\n");
  size_t e = s.find_last_not_of(" \t\r\n");
  if (b == std::string::npos) return Err::InvalidVector;
  s = s.substr(b, e - b + 1);
  if (s.size() < 2 || s.front() != '[' || s.back() != ']') return Err::InvalidVector;
  std::string inner = s.substr(1, s.size() - 2);
  size_t i = 0;
  while (true) {
    while (i < inner.size() && (inner[i] == ' ' || inner[i] == '\t' || inner[i] == ',')) i++;
    if (i >= inner.size()) break;
    size_t j = i;
    while (j < inner.size() && inner[j] != ',') j++;
    std::string tok = inner.substr(i, j - i);
    // trim token
    size_t tb = tok.find_first_not_of(" \t");
    size_t te = tok.find_last_not_of(" \t");
    if (tb == std::string::npos) return Err::InvalidVector;
    tok = tok.substr(tb, te - tb + 1);
    double d = 0;
    if (!parse_f64(tok, d)) return Err::InvalidVector;
    if (d > double(std::numeric_limits<float>::max()) ||
        d < -double(std::numeric_limits<float>::max())) return Err::InvalidVector;
    out.push_back(float(d));
    i = j;
    if (i < inner.size() && inner[i] == ',') i++;
    else break;
  }
  // tolerate trailing comma
  std::string rest = inner.substr(i);
  for (char c : rest) if (c != ' ' && c != '\t' && c != ',') return Err::InvalidVector;
  return Err::OK;
}

}  // namespace mvdb
