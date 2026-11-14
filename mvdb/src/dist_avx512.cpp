// AVX-512 distance kernel, compiled as its own translation unit with the
// AVX-512 flags (the rest of the binary stays AVX2-compatible). Runtime
// dispatch via cpu_avx512() keeps non-AVX-512 machines working.
#include <cpuid.h>
#include <cstddef>
#include <cstdint>
#include <immintrin.h>

namespace mvdb {

bool cpu_avx512() {
  // cached: this is called once per distance computation (i.e. per row)
  static const bool ok = []() -> bool {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) return false;
    if (!(ebx & (1u << 16))) return false;   // AVX512F
    if (!(ebx & (1u << 17))) return false;   // AVX512DQ (reduce)
    if (!(ebx & (1u << 31))) return false;   // AVX512VL
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
    if (!(ecx & (1u << 27))) return false;   // OSXSAVE
    if ((_xgetbv(0) & 0xE6) != 0xE6) return false;  // XCR0: opmask + zmm state
    return true;
#else
    return false;
#endif
  }();
  return ok;
}

// Squared L2 over float32 vectors with 512-bit FMA; `d` may be arbitrary.
float l2sq_avx512(const float* a, const float* b, size_t d) {
  __m512 acc0 = _mm512_setzero_ps();
  __m512 acc1 = _mm512_setzero_ps();
  size_t i = 0;
  for (; i + 32 <= d; i += 32) {
    __m512 x0 = _mm512_loadu_ps(a + i);
    __m512 y0 = _mm512_loadu_ps(b + i);
    __m512 x1 = _mm512_loadu_ps(a + i + 16);
    __m512 y1 = _mm512_loadu_ps(b + i + 16);
    __m512 d0 = _mm512_sub_ps(x0, y0);
    __m512 d1 = _mm512_sub_ps(x1, y1);
    acc0 = _mm512_fmadd_ps(d0, d0, acc0);
    acc1 = _mm512_fmadd_ps(d1, d1, acc1);
  }
  for (; i + 16 <= d; i += 16) {
    __m512 x = _mm512_loadu_ps(a + i);
    __m512 y = _mm512_loadu_ps(b + i);
    __m512 dd = _mm512_sub_ps(x, y);
    acc0 = _mm512_fmadd_ps(dd, dd, acc0);
  }
  __m512 sum = _mm512_add_ps(acc0, acc1);
  float s = _mm512_reduce_add_ps(sum);
  for (; i < d; i++) {
    float x = a[i] - b[i];
    s += x * x;
  }
  return s;
}

}  // namespace mvdb
