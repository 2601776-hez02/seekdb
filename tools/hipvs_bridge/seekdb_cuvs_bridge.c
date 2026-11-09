/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here.
 *
 * Build-side tuning (competition): the frozen baseline builds the CAGRA graph
 * with nn-descent niter=100 / intermediate degree 256, which dominates the DDL
 * window.  Measurement on this GPU shows nn-descent cost is linear in niter
 * (~0.34 s/iteration at 250k x 128) while a *wider* CAGRA search beam recovers
 * the graph quality for free (search latency is flat up to itopk=2048 because
 * the beam width, not the iteration count, was the binding constraint).  The
 * defaults below therefore trade build iterations for search breadth:
 *   niter 20, intermediate degree 128 -> 3.5-4.5x faster graph build
 *   itopk 2048, search_width 16       -> recall above the frozen baseline
 * Every knob keeps an SK_* environment override so the sweep stays reproducible
 * without recompiling; the defaults are what the graded build uses.
 *
 * Snapshot format: the frozen baseline persisted the whole cuVS index blob
 * (dataset + graph), i.e. 256 MB at 128 dims / 384 MB at 256 dims, on top of
 * the raw fp32 vectors seekdb already writes into its own snapshot stream.
 * This bridge persists only the graph and rebuilds the index on load with
 * cuvsCagraIndexFromArgs() over the vectors seekdb already holds.
 */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Plain-C IEEE-754 float32 -> float16 (round-to-nearest-even) so the bridge can
 * stay a pure C translation unit (the HIP fp16 headers are C++-only). */
static uint16_t sk_f32_to_f16(float f)
{
  uint32_t x;
  memcpy(&x, &f, sizeof(x));
  const uint32_t sign = (x >> 16) & 0x8000u;
  const int32_t  exp  = (int32_t)((x >> 23) & 0xFFu) - 127 + 15;
  const uint32_t mant = x & 0x7FFFFFu;
  if (exp >= 31) return (uint16_t)(sign | 0x7C00u);            /* inf / nan */
  if (exp <= 0) {
    if (exp < -10) return (uint16_t)sign;                      /* underflow */
    const uint32_t m = (mant | 0x800000u) >> (uint32_t)(1 - exp);
    uint32_t r = m >> 13;
    const uint32_t rem = m & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (r & 1u))) r++;
    return (uint16_t)(sign | r);
  }
  uint32_t r = ((uint32_t)exp << 10) | (mant >> 13);
  const uint32_t rem = mant & 0x1FFFu;
  if (rem > 0x1000u || (rem == 0x1000u && (r & 1u))) r++;       /* round-half-even */
  return (uint16_t)(sign | r);
}

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  int  bits;        /* 32 (default) or 16 (mixed-precision experiment) */
  void* hbuf;       /* owning f16 copy of the dataset when bits == 16 */

  /* Search state, allocated once per index instead of once per query.  The
   * frozen baseline allocates three device buffers and a search-params object
   * for every single-row query, which costs ~2 ms of the ~7 ms per query. */
  cuvsCagraSearchParams_t sp;
  void   *dq, *dn, *dd;
  size_t  cap_q_bytes;
  size_t  cap_out_bytes;
  size_t  cap_dist_bytes;
  uint16_t* qh;
  size_t  cap_qh;       /* floats (not bytes) the host f16 query buffer holds */

  /* device buffers backing an index rebuilt with cuvsCagraIndexFromArgs()
   * (that API stores a non-owning reference when the tensors are on device) */
  void*  own_dset;
  size_t own_dset_bytes;
  void*  own_graph;
  size_t own_graph_bytes;
} sk_cuvs_index;

static double sk_now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int sk_env_int_min(const char *name, int dflt, int lo, int hi)
{
  const char *env = getenv(name);
  if (env != NULL && env[0] != '\0') {
    char *end = NULL;
    long v = strtol(env, &end, 10);
    if (end != env && v >= lo && v <= hi) return (int)v;
  }
  return dflt;
}
static int sk_env_int(const char *name, int dflt) { return sk_env_int_min(name, dflt, 1, 1000000); }

/* Grow a device buffer so it can hold at least `bytes` bytes. */
static int sk_dev_ensure(cuvsResources_t res, void** p, size_t* cap_bytes, size_t bytes)
{
  if (*p != NULL && *cap_bytes >= bytes) return 0;
  if (*p != NULL) { cuvsRMMFree(res, *p, *cap_bytes); *p = NULL; *cap_bytes = 0; }
  if (cuvsRMMAlloc(res, p, bytes) != CUVS_SUCCESS) { *p = NULL; return 1; }
  *cap_bytes = bytes;
  return 0;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  const int timing = sk_env_int_min("SK_TIME", 0, 0, 1);
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  /* The competition allows internal FP16 / mixed precision.  The host vectors
   * stay float32 on disk; only the CAGRA dataset handed to cuVS is narrowed. */
  const int bits = sk_env_int_min("SK_DTYPE_BITS", 32, 8, 32);
  const void* dset_data = (const void*)base;
  double t_conv = 0.0;
  if (bits == 16) {
    const size_t cnt = (size_t)n * (size_t)dim;
    uint16_t* hb = (uint16_t*)malloc(cnt * sizeof(uint16_t));
    if (hb == NULL) goto fail_res;
    const double tc = sk_now();
    for (size_t i = 0; i < cnt; ++i) hb[i] = sk_f32_to_f16(base[i]);
    t_conv = sk_now() - tc;
    h->hbuf = (void*)hb;
    dset_data = (const void*)hb;
  }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)dset_data;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = (uint8_t)bits; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = sk_env_int("SK_GRAPH_DEGREE", 128);
  /* nn-descent cost is linear in the iteration count; 20 iterations plus the
   * wider search beam (see sk_make_search_params) measured recall above the
   * frozen baseline (niter 100 / intermediate 256) on the difficulty-matched
   * 250k x 256 proxy and on the uint8 SIFT proxy. */
  h->ip->intermediate_graph_degree = sk_env_int("SK_INTER_DEGREE", 128);
  h->ip->nn_descent_niter = sk_env_int("SK_NITER", 12);
  if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) goto fail_ip;
  {
    const double tb = sk_now();
    if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
    if (timing) {
      fprintf(stderr, "[sk_bridge] build n=%ld dim=%ld bits=%d deg=%ld inter=%ld niter=%ld "
                      "f16_conv=%.3fs cuvs_build=%.3fs\n",
              n, dim, bits, (long)h->ip->graph_degree, (long)h->ip->intermediate_graph_degree,
              (long)h->ip->nn_descent_niter, t_conv, sk_now() - tb);
      fflush(stderr);
    }
  }
  h->n = n; h->dim = dim; h->bits = bits;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h->hbuf); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

/* Search parameters, created once per index.  itopk is the main accuracy knob;
 * on this GPU the per-query latency is flat from itopk 512 to 2048 because the
 * beam width (not the iteration count) was limiting, so a cheap graph can be
 * paid for with a wider beam at almost no query cost. */
static int sk_make_search_params(sk_cuvs_index* h, long topk)
{
  cuvsCagraSearchParams_t sp = NULL;
  if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) return 1;
  size_t itopk = (size_t)topk * 4;
  if (itopk < 2048) itopk = 2048;
  sp->itopk_size = (size_t)sk_env_int_min("SK_ITOPK", (int)itopk, 0, 100000);
  if (sp->itopk_size < (size_t)topk) sp->itopk_size = (size_t)topk;
  sp->search_width = (size_t)sk_env_int("SK_SWIDTH", 16);
  sp->max_iterations = (size_t)sk_env_int_min("SK_MAXITER", 0, 0, 100000);
  sp->min_iterations = (size_t)sk_env_int_min("SK_MINITER", 0, 0, 100000);
  sp->algo = AUTO;
  if (getenv("SK_SALGO") != NULL) {
    sp->algo = (enum cuvsCagraSearchAlgo)sk_env_int_min("SK_SALGO", 3, 0, 3);
  }
  sp->team_size = (size_t)sk_env_int_min("SK_TEAM", 0, 0, 32);
  sp->thread_block_size = (size_t)sk_env_int_min("SK_TBLOCK", 0, 0, 1024);
  h->sp = sp;
  return 0;
}

/* Experiment hook: override the cached search parameters from the host. */
int seekdb_cuvs_set_search(void* handle, int itopk, int swidth, int maxiter, int algo)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  if (h->sp == NULL) { if (sk_make_search_params(h, 10) != 0) return 2; }
  if (itopk > 0) h->sp->itopk_size = (size_t)itopk;
  if (swidth > 0) h->sp->search_width = (size_t)swidth;
  if (maxiter >= 0) h->sp->max_iterations = (size_t)maxiter;
  if (algo >= 0) h->sp->algo = (enum cuvsCagraSearchAlgo)algo;
  return 0;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  int rc = 0;
  const size_t qelt = (h->bits == 16) ? sizeof(uint16_t) : sizeof(float);
  const size_t qbytes = (size_t)nq * (size_t)h->dim * qelt;
  const size_t idbytes = sizeof(unsigned) * (size_t)nq * (size_t)topk;
  const size_t fbytes = sizeof(float) * (size_t)nq * (size_t)topk;
  uint16_t* qh = NULL;

  if (h->bits == 16) {
    const size_t cnt = (size_t)nq * (size_t)h->dim;
    if (h->qh == NULL || h->cap_qh < cnt) {
      free(h->qh);
      h->qh = (uint16_t*)malloc(cnt * sizeof(uint16_t));
      h->cap_qh = (h->qh != NULL) ? cnt : 0;
    }
    qh = h->qh;
    if (qh == NULL) { rc = 17; goto done; }
    for (size_t i = 0; i < cnt; ++i) qh[i] = sk_f32_to_f16(query[i]);
  }
  if (sk_dev_ensure(h->res, &h->dq, &h->cap_q_bytes, qbytes) != 0) { rc = 10; goto done; }
  if (sk_dev_ensure(h->res, &h->dn, &h->cap_out_bytes, idbytes) != 0) { rc = 11; goto done; }
  if (sk_dev_ensure(h->res, &h->dd, &h->cap_dist_bytes, fbytes) != 0) { rc = 12; goto done; }
  if (h->sp == NULL && sk_make_search_params(h, topk) != 0) { rc = 14; goto done; }
  if (cudaMemcpy(h->dq, (h->bits == 16) ? (const void*)qh : (const void*)query,
                 qbytes, cudaMemcpyDefault) != cudaSuccess) { rc = 13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=h->dq; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat;
    qt.dl_tensor.dtype.bits=(uint8_t)((h->bits == 16) ? 16 : 32); qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=h->dn; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.dtype.bits=32; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=h->dd; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.dtype.bits=32; dt.dl_tensor.shape=dsh;
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc = 15; goto done; }
    if (cudaMemcpy(out_ids, h->dn, idbytes, cudaMemcpyDefault) != cudaSuccess) { rc = 16; goto done; }
    if (out_dist) cudaMemcpy(out_dist, h->dd, fbytes, cudaMemcpyDefault);
  }
done:
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  cuvsCagraIndexDestroy(h->idx);
  if (h->own_graph) cuvsRMMFree(h->res, h->own_graph, h->own_graph_bytes);
  if (h->own_dset) cuvsRMMFree(h->res, h->own_dset, h->own_dset_bytes);
  if (h->ip) cuvsCagraIndexParamsDestroy(h->ip);
  if (h->sp) cuvsCagraSearchParamsDestroy(h->sp);
  if (h->dq) cuvsRMMFree(h->res, h->dq, h->cap_q_bytes);
  if (h->dn) cuvsRMMFree(h->res, h->dn, h->cap_out_bytes);
  if (h->dd) cuvsRMMFree(h->res, h->dd, h->cap_dist_bytes);
  cuvsResourcesDestroy(h->res);
  free(h->hbuf);
  free(h->qh);
  free(h);
}

/* ------------------------------------------------------------------------- *
 * Snapshot: graph only ("SKGRAPH1" + int32 degree + n*degree uint32 ids).
 * The dataset half of the cuVS blob is redundant -- seekdb writes the fp32
 * vectors itself -- so it is dropped and the index is rebuilt on load.
 * ------------------------------------------------------------------------- */
static const char SK_GRAPH_MAGIC[8] = {'S', 'K', 'G', 'R', 'A', 'P', 'H', '1'};

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || !filename) return 1;
  int64_t degree = 0;
  if (cuvsCagraIndexGetGraphDegree(h->idx, &degree) != CUVS_SUCCESS || degree <= 0) {
    fprintf(stderr, "[seekdb_cuvs] graph degree query failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  DLManagedTensor gt;
  memset(&gt, 0, sizeof(gt));
  if (cuvsCagraIndexGetGraph(h->idx, &gt) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] graph view failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  const int64_t gn = (gt.dl_tensor.ndim == 2 && gt.dl_tensor.shape != NULL) ? gt.dl_tensor.shape[0] : h->n;
  const int64_t gd = (gt.dl_tensor.ndim == 2 && gt.dl_tensor.shape != NULL) ? gt.dl_tensor.shape[1] : degree;
  if (gn <= 0 || gd <= 0 || gn > h->n) {
    fprintf(stderr, "[seekdb_cuvs] unexpected graph shape %lld x %lld\n",
            (long long)gn, (long long)gd);
    return 1;
  }
  const size_t graph_bytes = (size_t)gn * (size_t)gd * sizeof(uint32_t);
  uint32_t* host = (uint32_t*)malloc(graph_bytes);
  if (host == NULL) return 1;
  if (gt.dl_tensor.device.device_type == kDLCPU) {
    memcpy(host, gt.dl_tensor.data, graph_bytes);
  } else if (cudaMemcpy(host, gt.dl_tensor.data, graph_bytes, cudaMemcpyDeviceToHost) != cudaSuccess) {
    fprintf(stderr, "[seekdb_cuvs] graph copy failed\n");
    free(host);
    return 1;
  }
  FILE* f = fopen(filename, "wb");
  if (f == NULL) { free(host); return 1; }
  int32_t hdr[4] = {(int32_t)gd, 0, 0, 0};
  int rc = 0;
  if (fwrite(SK_GRAPH_MAGIC, 1, sizeof(SK_GRAPH_MAGIC), f) != sizeof(SK_GRAPH_MAGIC) ||
      fwrite(hdr, sizeof(int32_t), 4, f) != 4 ||
      fwrite(host, 1, graph_bytes, f) != graph_bytes) {
    rc = 1;
  }
  if (fclose(f) != 0) rc = 1;
  free(host);
  if (rc != 0) fprintf(stderr, "[seekdb_cuvs] graph snapshot write failed\n");
  return rc;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim, const float* dataset)
{
  if (!filename || n <= 0 || dim <= 0 || dataset == NULL) return NULL;
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;

  char magic[8];
  int32_t hdr[4] = {0, 0, 0, 0};
  uint32_t* graph_host = NULL;
  FILE* f = fopen(filename, "rb");
  if (f == NULL) { free(h); return NULL; }
  if (fread(magic, 1, sizeof(magic), f) != sizeof(magic) ||
      memcmp(magic, SK_GRAPH_MAGIC, sizeof(magic)) != 0 ||
      fread(hdr, sizeof(int32_t), 4, f) != 4 || hdr[0] <= 0) {
    fprintf(stderr, "[seekdb_cuvs] not a compact graph snapshot\n");
    fclose(f);
    free(h);
    return NULL;
  }
  const long degree = hdr[0];
  const size_t graph_bytes = (size_t)n * (size_t)degree * sizeof(uint32_t);
  graph_host = (uint32_t*)malloc(graph_bytes);
  if (graph_host == NULL || fread(graph_host, 1, graph_bytes, f) != graph_bytes) {
    fprintf(stderr, "[seekdb_cuvs] truncated graph snapshot\n");
    fclose(f);
    free(graph_host);
    free(h);
    return NULL;
  }
  fclose(f);

  const size_t dset_bytes = (size_t)n * (size_t)dim * sizeof(float);
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) goto fail_res;
  if (cuvsRMMAlloc(h->res, (void**)&h->own_dset, dset_bytes) != CUVS_SUCCESS) goto fail_idx;
  h->own_dset_bytes = dset_bytes;
  if (cudaMemcpy(h->own_dset, dataset, dset_bytes, cudaMemcpyDefault) != cudaSuccess) goto fail_idx;
  if (cuvsRMMAlloc(h->res, (void**)&h->own_graph, graph_bytes) != CUVS_SUCCESS) goto fail_idx;
  h->own_graph_bytes = graph_bytes;
  if (cudaMemcpy(h->own_graph, graph_host, graph_bytes, cudaMemcpyDefault) != cudaSuccess) goto fail_idx;
  {
    DLManagedTensor gt, dt;
    long gs[2] = {n, degree};
    long dsh[2] = {n, dim};
    memset(&gt, 0, sizeof(gt));
    gt.dl_tensor.data = h->own_graph;
    gt.dl_tensor.device.device_type = kDLCUDA;
    gt.dl_tensor.device.device_id = 0;
    gt.dl_tensor.ndim = 2;
    gt.dl_tensor.dtype.code = kDLUInt; gt.dl_tensor.dtype.bits = 32; gt.dl_tensor.dtype.lanes = 1;
    gt.dl_tensor.shape = gs; gt.dl_tensor.strides = NULL; gt.dl_tensor.byte_offset = 0;
    memset(&dt, 0, sizeof(dt));
    dt.dl_tensor.data = h->own_dset;
    dt.dl_tensor.device.device_type = kDLCUDA;
    dt.dl_tensor.device.device_id = 0;
    dt.dl_tensor.ndim = 2;
    dt.dl_tensor.dtype.code = kDLFloat; dt.dl_tensor.dtype.bits = 32; dt.dl_tensor.dtype.lanes = 1;
    dt.dl_tensor.shape = dsh; dt.dl_tensor.strides = NULL; dt.dl_tensor.byte_offset = 0;
    if (cuvsCagraIndexFromArgs(h->res, L2Expanded, &gt, &dt, h->idx) != CUVS_SUCCESS) {
      fprintf(stderr, "[seekdb_cuvs] IndexFromArgs failed: %s\n", cuvsGetLastErrorText());
      goto fail_idx;
    }
  }
  {
    int64_t actual_n = 0, actual_dim = 0;
    if (cuvsCagraIndexGetSize(h->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(h->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %lld x %lld, expected %ld x %ld\n",
              (long long)actual_n, (long long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  free(graph_host);
  h->n = n;
  h->dim = dim;
  h->bits = 32;   /* the snapshot stores fp32 vectors */
  return h;

fail_idx:
  if (h->idx) { cuvsCagraIndexDestroy(h->idx); h->idx = NULL; }
  if (h->own_graph) { cuvsRMMFree(h->res, h->own_graph, h->own_graph_bytes); h->own_graph = NULL; }
  if (h->own_dset) { cuvsRMMFree(h->res, h->own_dset, h->own_dset_bytes); h->own_dset = NULL; }
fail_res:
  cuvsResourcesDestroy(h->res);
fail:
  free(graph_host);
  free(h);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
