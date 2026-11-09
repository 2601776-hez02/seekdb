/* bench_build.c — quantify hipVS/cuVS CAGRA build (GPU) vs VSAG HNSW build (CPU)
 * for the seekdb vector-index construction path.
 *
 * This is a standalone validation tool (NOT part of the seekdb build). It links
 * the plain-C bridge `libseekdb_cuvs_bridge.so` via dlopen, so no cuVS/ROCm
 * headers or link deps are needed to compile it.
 *
 * Build (on the AMD GPU host, after tools/hipvs_bridge/build.sh produced the .so):
 *   gcc -O2 -o bench_build bench_build.c -ldl
 *
 * Run:
 *   LD_LIBRARY_PATH=/path/to/bridge:/opt/hipvs/lib:/opt/rocm/lib \
 *     ./bench_build 100000 128 libseekdb_cuvs_bridge.so
 *
 * Output: GPU CAGRA build wall-time and throughput for (N x dim) float32 row-major.
 * Pair it with the CPU number below for the speedup = cpu_seconds / gpu_seconds.
 *
 * CPU (VSAG HNSW) reference number is produced separately; easiest is to build the
 * same data through seekdb's existing VSAG path (CREATE VECTOR INDEX ... lib=vsag)
 * or a small program linking devdeps-vsag, and report its wall-time here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <time.h>

typedef void *(*build_fn)(const float *, long, long);
typedef int (*search_fn)(void *, const float *, long, long, unsigned int *, float *);
typedef void (*free_fn)(void *);

static double now_sec(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
  if (argc < 3) {
    fprintf(stderr, "usage: %s N dim [bridge.so] [nq topk]\n", argv[0]);
    return 2;
  }
  long n = atol(argv[1]);
  long dim = atol(argv[2]);
  const char *so = (argc > 3) ? argv[3] : "libseekdb_cuvs_bridge.so";
  long nq = (argc > 4) ? atol(argv[4]) : 0;
  long topk = (argc > 5) ? atol(argv[5]) : 10;

  void *h = dlopen(so, RTLD_NOW);
  if (h == NULL) {
    fprintf(stderr, "dlopen(%s): %s\n", so, dlerror());
    return 1;
  }
  build_fn build = (build_fn)dlsym(h, "seekdb_cuvs_build");
  search_fn search = (search_fn)dlsym(h, "seekdb_cuvs_search");
  free_fn free_idx = (free_fn)dlsym(h, "seekdb_cuvs_free");
  if (build == NULL || search == NULL || free_idx == NULL) {
    fprintf(stderr, "dlsym failed: %s\n", dlerror());
    return 1;
  }

  float *base = (float *)malloc((size_t)n * dim * sizeof(float));
  if (base == NULL) { fprintf(stderr, "oom base\n"); return 1; }
  srand(42);
  for (size_t i = 0; i < (size_t)n * dim; ++i) {
    base[i] = (float)rand() / (float)RAND_MAX;
  }

  double t0 = now_sec();
  void *idx = build(base, n, dim);
  double t1 = now_sec();
  if (idx == NULL) {
    fprintf(stderr, "seekdb_cuvs_build failed (no GPU / bridge error)\n");
    return 1;
  }
  double build_sec = t1 - t0;
  printf("N=%ld dim=%ld GPU CAGRA build: %.3f s (%.0f vec/s)\n",
         n, dim, build_sec, (double)n / build_sec);

  if (nq > 0 && topk > 0) {
    float *q = (float *)malloc((size_t)nq * dim * sizeof(float));
    unsigned *ids = (unsigned *)malloc((size_t)nq * topk * sizeof(unsigned));
    float *dist = (float *)malloc((size_t)nq * topk * sizeof(float));
    if (q == NULL || ids == NULL || dist == NULL) { fprintf(stderr, "oom q\n"); return 1; }
    for (size_t i = 0; i < (size_t)nq * dim; ++i) q[i] = (float)rand() / (float)RAND_MAX;
    double s0 = now_sec();
    int rc = search(idx, q, nq, topk, ids, dist);
    double s1 = now_sec();
    if (rc == 0) {
      printf("N=%ld nq=%ld topk=%ld GPU CAGRA search: %.3f s (%.0f q/s)\n",
             n, nq, topk, s1 - s0, (double)nq / (s1 - s0));
    } else {
      fprintf(stderr, "seekdb_cuvs_search rc=%d\n", rc);
    }
    free(q); free(ids); free(dist);
  }

  free_idx(idx);
  free(base);
  dlclose(h);
  return 0;
}
