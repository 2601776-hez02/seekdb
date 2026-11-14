// Equivalence fuzz for parse_f64_fast vs parse_f64: whenever the fast path
// claims success, its double must be bit-identical to the general parser's.
#include <cstring>
#include <cstdio>
#include <random>
#include <string>
#include "mvdb.h"

static int failures = 0;
static int fastHits = 0;

static void check(const std::string& tok) {
  double fd = 0, gd = 0;
  bool f = mvdb::parse_f64_fast(tok.data(), tok.size(), fd);
  bool g = mvdb::parse_f64(tok.data(), tok.size(), gd);
  if (f) {
    fastHits++;
    if (!g || std::memcmp(&fd, &gd, sizeof(double)) != 0) {
      failures++;
      if (failures <= 20) {
        std::printf("MISMATCH tok='%s' fast=%d(%.17g) general=%d(%.17g)\n",
                    tok.c_str(), f ? 1 : 0, fd, g ? 1 : 0, gd);
      }
    }
  }
}

int main() {
  // fixed edge set
  const char* edges[] = {
      "0", "1", "-1", "+1", "5", "0.0", "-0.0", "+0.0", ".5", "-.5", "+.5",
      "5.", "-5.", "0.00000000000000000001", "1.00000000000000000001",
      "99999999999999999999.99999999999999999999", "123456789.123456789",
      "0.1", "-0.1", "10.5", "007.500", "1.500000", "-1.500000",
      "3.4028234663852886e+38", "3.4028235e38", "-3.4028235e38",
      "1e-5", "1E-5", "1e+5", ".5e2", "1e5", "5e0", "0e0", "-0e0", "1e",
      "e5", "1ee5", "1..5", "1.2.3", "nan", "NaN", "inf", "-inf", "infinity",
      "1_0.5", "0x1p2", "1f", "1d", "1.5f", "1.5d", "1,5", "", ".", "-", "+",
      "1.5e", "1.5e+", "1.5e-", "1.5e+3", "1.5e-3", " 1.5", "1.5 ", "1 .5",
      "1. 5", "+1.5", "-.5e+2", "12345678901234567890.5",
      "1234567890123456789012345678.9", "0.12345678901234567890123456789",
      "9007199254740993.0", "1.7976931348623157e308", "1.7976931348623159e308",
      "2.2250738585072014e-308", "5e-324", "1e-400", "1e400", "1e309",
      "-1e309", "4.9e-324", "123456.654321", "0.000001", "1.000000",
      "18446744073709551616.0", "1.0000000000000002", "0.9999999999999999"};
  for (const char* e : edges) check(e);

  // random decimal shapes in the fast-path grammar
  std::mt19937_64 rng(12345);
  for (int i = 0; i < 200000; i++) {
    std::string tok;
    if (rng() % 2) tok += (rng() % 2 ? '-' : '+');
    int idigits = 1 + int(rng() % 22);
    int fdigits = 0 + int(rng() % 22);
    for (int j = 0; j < idigits; j++) tok += char('0' + rng() % 10);
    tok += '.';
    for (int j = 0; j < fdigits; j++) tok += char('0' + rng() % 10);
    check(tok);
  }
  // random garbage that must fall back or reject identically on fast=true
  const char* garbage = "0123456789.eE+-xX_,";
  for (int i = 0; i < 100000; i++) {
    std::string tok;
    int len = 1 + int(rng() % 20);
    for (int j = 0; j < len; j++) tok += garbage[rng() % std::strlen(garbage)];
    check(tok);
  }
  std::printf("fastHits=%d failures=%d\n", fastHits, failures);
  return failures == 0 ? 0 : 1;
}
