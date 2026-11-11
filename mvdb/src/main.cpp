// MiniVectorDB - entry point: official stdin/stdout JSONL protocol loop.
//
// Wire protocol (STDIO_PROTOCOL.md, 2026-competition-round2):
//   * startup: after recovery, output exactly {"type":"ready"} + newline,
//     flushed, BEFORE reading any request
//   * requests:  one JSON object per line
//       {"id":N,"op":"execute","sql":"...","search_mode":"exact|ann"}
//       {"id":N,"op":"shutdown"}
//   * responses: one JSON line per request, echoing `id` (same JSON type)
//       query : {"id":N,"ok":true,"columns":[...],"rows":[...]}
//       dml   : {"id":N,"ok":true,"affected_rows":N}
//       other : {"id":N,"ok":true}
//       error : {"id":N,"ok":false,"error":{"code":"<semantic>","message":"..."}}
//   * shutdown: respond ok, then finish persistence and exit 0; no further
//     stdout output of any kind
//   * SIGTERM/SIGINT/stdin-EOF trigger the same graceful shutdown path
// All logs go to stderr.
#include "engine.h"
#include "parser.h"

#include <csignal>
#include <cstring>
#include <iostream>
#include <unistd.h>

using namespace mvdb;

static volatile sig_atomic_t gStop = 0;
static void onSignal(int) {
  gStop = 1;
  // force any blocked read on stdin to return EOF so the shutdown path runs
  // even if the driver keeps stdin open while waiting for us to exit
  ::close(STDIN_FILENO);
}

// ---------------------------------------------------------------------------
// minimal JSON parser (request side). Self-implemented, no third-party deps.
// ---------------------------------------------------------------------------
namespace {

static void skipWs(const char*& p, const char* e) {
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

// JSON string literal -> UTF-8 value (handles \uXXXX incl. surrogate pairs)
static bool parseString(const char*& p, const char* e, std::string& out) {
  if (p >= e || *p != '"') return false;
  p++;
  out.clear();
  while (true) {
    if (p >= e) return false;
    unsigned char c = (unsigned char)*p;
    if (c == '"') { p++; return true; }
    if (c == '\\') {
      p++;
      if (p >= e) return false;
      char esc = *p++;
      switch (esc) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          if (p + 4 > e) return false;
          uint32_t cp = 0;
          for (int i = 0; i < 4; i++) {
            int h = hexVal(p[i]);
            if (h < 0) return false;
            cp = (cp << 4) | uint32_t(h);
          }
          p += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // high surrogate: expect a following \uDC00-\uDFFF
            if (p + 6 > e || p[0] != '\\' || p[1] != 'u') return false;
            uint32_t lo = 0;
            for (int i = 2; i < 6; i++) {
              int h = hexVal(p[i]);
              if (h < 0) return false;
              lo = (lo << 4) | uint32_t(h);
            }
            if (lo < 0xDC00 || lo > 0xDFFF) return false;
            p += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          appendUtf8(out, cp);
          break;
        }
        default: return false;
      }
    } else {
      out += char(c);
      p++;
    }
  }
}

// JSON number token (kept verbatim so the response can echo it exactly)
static bool parseNumberToken(const char*& p, const char* e, std::string& tok) {
  const char* start = p;
  if (p < e && *p == '-') p++;
  if (p >= e) return false;
  if (*p == '0') {
    p++;
  } else if (*p >= '1' && *p <= '9') {
    while (p < e && *p >= '0' && *p <= '9') p++;
  } else {
    return false;
  }
  if (p < e && *p == '.') {
    p++;
    if (p >= e || *p < '0' || *p > '9') return false;
    while (p < e && *p >= '0' && *p <= '9') p++;
  }
  if (p < e && (*p == 'e' || *p == 'E')) {
    p++;
    if (p < e && (*p == '+' || *p == '-')) p++;
    if (p >= e || *p < '0' || *p > '9') return false;
    while (p < e && *p >= '0' && *p <= '9') p++;
  }
  tok.assign(start, size_t(p - start));
  return true;
}

static bool skipValue(const char*& p, const char* e);

static bool parseRequest(const std::string& line, std::string& idText, bool& idIsNum,
                         std::string& op, std::string& sql, std::string& searchMode) {
  const char* p = line.data();
  const char* e = p + line.size();
  skipWs(p, e);
  if (p >= e || *p != '{') return false;
  p++;
  skipWs(p, e);
  if (p >= e) return false;
  if (*p == '}') { p++; skipWs(p, e); return p == e; }
  while (true) {
    skipWs(p, e);
    std::string key;
    if (!parseString(p, e, key)) return false;
    skipWs(p, e);
    if (p >= e || *p != ':') return false;
    p++;
    skipWs(p, e);
    if (key == "id") {
      if (p < e && *p == '"') {
        if (!parseString(p, e, idText)) return false;
        idIsNum = false;
      } else {
        if (!parseNumberToken(p, e, idText)) return false;
        idIsNum = true;
      }
    } else if (key == "op") {
      if (!parseString(p, e, op)) return false;
    } else if (key == "sql") {
      if (!parseString(p, e, sql)) return false;
    } else if (key == "search_mode") {
      if (!parseString(p, e, searchMode)) return false;
    } else {
      if (!skipValue(p, e)) return false;
    }
    skipWs(p, e);
    if (p >= e) return false;
    if (*p == ',') { p++; continue; }
    if (*p == '}') { p++; break; }
    return false;
  }
  skipWs(p, e);
  return p == e;
}

static bool skipValue(const char*& p, const char* e) {
  skipWs(p, e);
  if (p >= e) return false;
  char c = *p;
  if (c == '"') {
    std::string tmp;
    return parseString(p, e, tmp);
  }
  if (c == '{') {
    p++;
    skipWs(p, e);
    if (p < e && *p == '}') { p++; return true; }
    while (true) {
      skipWs(p, e);
      std::string key;
      if (!parseString(p, e, key)) return false;
      skipWs(p, e);
      if (p >= e || *p != ':') return false;
      p++;
      if (!skipValue(p, e)) return false;
      skipWs(p, e);
      if (p >= e) return false;
      if (*p == ',') { p++; continue; }
      if (*p == '}') { p++; return true; }
      return false;
    }
  }
  if (c == '[') {
    p++;
    skipWs(p, e);
    if (p < e && *p == ']') { p++; return true; }
    while (true) {
      if (!skipValue(p, e)) return false;
      skipWs(p, e);
      if (p >= e) return false;
      if (*p == ',') { p++; continue; }
      if (*p == ']') { p++; return true; }
      return false;
    }
  }
  if (c == 't') {
    if (size_t(e - p) >= 4 && std::memcmp(p, "true", 4) == 0) { p += 4; return true; }
    return false;
  }
  if (c == 'f') {
    if (size_t(e - p) >= 5 && std::memcmp(p, "false", 5) == 0) { p += 5; return true; }
    return false;
  }
  if (c == 'n') {
    if (size_t(e - p) >= 4 && std::memcmp(p, "null", 4) == 0) { p += 4; return true; }
    return false;
  }
  std::string tok;
  return parseNumberToken(p, e, tok);
}

}  // namespace

// ---------------------------------------------------------------------------
// response builders
// ---------------------------------------------------------------------------
static void jsonEscape(const std::string& s, std::string& out) {
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(char(c));
        }
    }
  }
}

static void valueToJson(const Value& v, std::string& out) {
  switch (v.t) {
    case Value::T::BigInt:
      out += std::to_string(v.i);
      break;
    case Value::T::F64: {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.9g", v.f);
      out += buf;
      break;
    }
    case Value::T::Str:
      out += '"';
      jsonEscape(v.s, out);
      out += '"';
      break;
    case Value::T::Vec: {
      out += '"';
      out += '[';
      for (size_t i = 0; i < v.v.size(); i++) {
        if (i) out += ',';
        char buf[64];
        // shortest float32-friendly form (7 significant digits round-trips f32)
        snprintf(buf, sizeof(buf), "%.7g", double(v.v[i]));
        out += buf;
      }
      out += ']';
      out += '"';
      break;
    }
  }
}

static void emit(const std::string& s) {
  fwrite(s.data(), 1, s.size(), stdout);
  fflush(stdout);
}

static void idField(std::string& out, const std::string& idText, bool idIsNum) {
  out += "\"id\":";
  if (idIsNum) {
    out += idText;
  } else {
    out += '"';
    jsonEscape(idText, out);
    out += '"';
  }
}

static void respondQuery(const std::string& idText, bool idIsNum, const ExecResult& r) {
  std::string out = "{";
  idField(out, idText, idIsNum);
  out += ",\"ok\":true,\"columns\":[";
  for (size_t i = 0; i < r.columns.size(); i++) {
    if (i) out += ',';
    out += '"';
    jsonEscape(r.columns[i], out);
    out += '"';
  }
  out += "],\"rows\":[";
  for (size_t i = 0; i < r.rows.size(); i++) {
    if (i) out += ',';
    out += '[';
    for (size_t j = 0; j < r.rows[i].size(); j++) {
      if (j) out += ',';
      valueToJson(r.rows[i][j], out);
    }
    out += ']';
  }
  out += "]}\n";
  emit(out);
}

static void respondAffected(const std::string& idText, bool idIsNum, int64_t n) {
  std::string out = "{";
  idField(out, idText, idIsNum);
  out += ",\"ok\":true,\"affected_rows\":";
  out += std::to_string(n);
  out += "}\n";
  emit(out);
}

static void respondOk(const std::string& idText, bool idIsNum) {
  std::string out = "{";
  idField(out, idText, idIsNum);
  out += ",\"ok\":true}\n";
  emit(out);
}

static void respondError(const std::string& idText, bool idIsNum, Err e) {
  std::string out = "{";
  idField(out, idText, idIsNum);
  out += ",\"ok\":false,\"error\":{\"code\":\"";
  out += err_name(e);
  out += "\",\"message\":\"";
  out += err_name(e);
  out += "\"}}\n";
  emit(out);
}

int main(int argc, char** argv) {
  std::string dataDir = "/data";
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if ((a == "--data-dir" || a == "-d") && i + 1 < argc) {
      dataDir = argv[++i];
    } else if (a.rfind("--data-dir=", 0) == 0) {
      dataDir = a.substr(11);
    }
  }
  // block SIGPIPE: driver closing stdin/stdout must not kill us
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, onSignal);
  signal(SIGINT, onSignal);

  Engine engine(dataDir);
  Err e = engine.Init();
  if (e != Err::OK) {
    fprintf(stderr, "minivectordb: init failed (%s) in %s\n", err_name(e), dataDir.c_str());
    return 1;
  }
  // handshake: exactly this object, one line, flushed, before reading input
  emit("{\"type\":\"ready\"}\n");

  std::string line;
  while (!gStop && std::getline(std::cin, line)) {
    if (getenv("MVDB_TRACE")) {
      fprintf(stderr, "TRACE got request: %.200s%s\n", line.c_str(),
              line.size() > 200 ? "..." : "");
      fflush(stderr);
    }
    std::string idText, op, sql, searchMode = "exact";
    bool idIsNum = false;
    if (!parseRequest(line, idText, idIsNum, op, sql, searchMode) || op.empty()) {
      // malformed request: no reliable id to echo; log and keep going
      fprintf(stderr, "minivectordb: ignoring malformed request line\n");
      continue;
    }
    if (op == "shutdown") {
      respondOk(idText, idIsNum);
      break;
    }
    if (op != "execute") {
      respondError(idText, idIsNum, Err::UnsupportedSQL);
      continue;
    }
    bool annMode = (to_upper(searchMode) == "ANN");
    Parser parser(sql);
    ParseResult pr = parser.Parse();
    if (pr.err != Err::OK) {
      respondError(idText, idIsNum, pr.err);
      continue;
    }
    ExecResult res;
    Err ex = engine.Execute(*pr.stmt, annMode, res);
    if (ex != Err::OK) {
      respondError(idText, idIsNum, ex);
      continue;
    }
    if (res.isQuery) {
      respondQuery(idText, idIsNum, res);
    } else if (pr.stmt->kind == Stmt::Kind::Insert || pr.stmt->kind == Stmt::Kind::Update ||
               pr.stmt->kind == Stmt::Kind::Delete) {
      respondAffected(idText, idIsNum, res.affected);
    } else {
      respondOk(idText, idIsNum);
    }
  }
  // graceful shutdown: rollback open txn, flush, checkpoint.
  // The protocol requires exit code 0 after a shutdown response; failures are
  // logged to stderr only (nothing more may be written to stdout).
  Err sh = engine.Shutdown();
  if (sh != Err::OK) {
    fprintf(stderr, "minivectordb: shutdown failed (%s)\n", err_name(sh));
  }
  return 0;
}
