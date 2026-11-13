// crc equivalence test: slicing-by-8 vs byte-wise reference, single-shot and
// chained (the SerializeTo buffer splits crc across arbitrary chunk sizes).
#include "mvdb.h"

#include <cstdio>
#include <cstdint>
#include <random>

using mvdb::crc32_soft;

static uint32_t ref(const uint8_t* p, size_t n, uint32_t crc = 0u) {
  static uint32_t T[256];
  static bool init = false;
  if (!init) {
    for (uint32_t k = 0; k < 256; k++) {
      uint32_t c = k;
      for (int b = 0; b < 8; b++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      T[k] = c;
    }
    init = true;
  }
  crc ^= 0xFFFFFFFFu;
  for (size_t k = 0; k < n; k++) crc = T[(crc ^ p[k]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

int main() {
  // standard CRC-32 check value: crc32("123456789") must be 0xCBF43926
  {
    const char* probe = "123456789";
    uint32_t ck = crc32_soft((const uint8_t*)probe, 9);
    if (ck != 0xCBF43926u) {
      printf("CHECK VALUE WRONG: %08x (want CBF43926)\n", ck);
      return 1;
    }
    printf("check value OK\n");
  }
  std::mt19937 rng(1);
  int fails = 0;
  for (int t = 0; t < 2000; t++) {
    size_t n = rng() % 400;
    uint8_t buf[400];
    for (size_t i = 0; i < n; i++) buf[i] = uint8_t(rng());
    uint32_t b = ref(buf, n);
    uint32_t a = crc32_soft(buf, n);
    if (a != b) {
      printf("MISMATCH single n=%zu got=%08x want=%08x\n", n, a, b);
      if (++fails > 3) return 1;
    }
    if (n > 1) {
      for (int s = 0; s < 6; s++) {
        size_t split = 1 + (rng() % (n - 1));
        uint32_t c1 = crc32_soft(buf, split);
        uint32_t c2 = crc32_soft(buf + split, n - split, c1);
        if (c2 != b) {
          printf("MISMATCH chained n=%zu split=%zu got=%08x want=%08x\n", n, split, c2, b);
          if (++fails > 3) return 1;
        }
      }
    }
  }
  if (fails == 0) printf("CRC EQUIVALENCE OK\n");
  return fails ? 1 : 0;
}
