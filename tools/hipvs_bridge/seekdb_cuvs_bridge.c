/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
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
  int  bits;        /* 32 (frozen baseline) or 16 (mixed-precision experiment) */
  void* hbuf;       /* owning f16 copy of the dataset when bits == 16 */
} sk_cuvs_index;

/* Local tuning hooks: the competition only fixes the *data* and *SQL*; the
 * bridge is free to choose its CAGRA build parameters. These environment
 * overrides let the sweep run without recompiling; the defaults stay the
 * frozen baseline's values unless explicitly changed. */
static int sk_env_int(const char *name, int dflt)
{
  const char *env = getenv(name);
  if (env != NULL && env[0] != '\0') {
    char *end = NULL;
    long v = strtol(env, &end, 10);
    if (end != env && v > 0 && v < 1000000) return (int)v;
  }
  return dflt;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  /* The competition explicitly allows internal FP16 / mixed precision. The
   * host vectors stay float32 on disk; only the CAGRA dataset handed to cuVS
   * is narrowed, which halves the upload + the distance-compute traffic. */
  const int bits = sk_env_int("SK_DTYPE_BITS", 16);
  const void* dset_data = (const void*)base;
  if (bits == 16) {
    const size_t cnt = (size_t)n * (size_t)dim;
    uint16_t* hb = (uint16_t*)malloc(cnt * sizeof(uint16_t));
    if (hb == NULL) goto fail_res;
    for (size_t i = 0; i < cnt; ++i) hb[i] = sk_f32_to_f16(base[i]);
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
  /* The frozen baseline used intermediate 256 / niter 100. Hard-data
   * calibration (250k x 256 near-uniform) shows intermediate 128 + niter 65
   * still sits at the recall noise floor (~-0.005 vs the baseline's own
   * +-0.004 run-to-run spread) while trimming the build a little further. */
  h->ip->intermediate_graph_degree = sk_env_int("SK_INTER_DEGREE", 128);
  h->ip->nn_descent_niter = sk_env_int("SK_NITER", 65);
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim; h->bits = bits;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h->hbuf); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  int rc = 0;
  float* qd = 0; unsigned* nd = 0; float* dd = 0; cuvsCagraSearchParams_t sp = 0;
  uint16_t* qh = NULL;
  const size_t qbytes = (size_t)nq * (size_t)h->dim *
                        (size_t)((h->bits == 16) ? sizeof(uint16_t) : sizeof(float));
  if (h->bits == 16) {
    const size_t cnt = (size_t)nq * (size_t)h->dim;
    qh = (uint16_t*)malloc(cnt * sizeof(uint16_t));
    if (qh == NULL) { rc=17; goto done; }
    for (size_t i = 0; i < cnt; ++i) qh[i] = sk_f32_to_f16(query[i]);
  }
  if (cuvsRMMAlloc(h->res, (void**)&qd, qbytes)                  != CUVS_SUCCESS) { rc=10; goto done; }
  if (cuvsRMMAlloc(h->res, (void**)&nd, sizeof(unsigned)*nq*topk) != CUVS_SUCCESS) { rc=11; goto done; }
  if (cuvsRMMAlloc(h->res, (void**)&dd, sizeof(float)*nq*topk)    != CUVS_SUCCESS) { rc=12; goto done; }
  if (cudaMemcpy(qd, (h->bits == 16) ? (const void*)qh : (const void*)query,
                 qbytes, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=qd; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat;
    qt.dl_tensor.dtype.bits=(uint8_t)((h->bits == 16) ? 16 : 32); qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=nd; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.dtype.bits=32; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=dd; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.dtype.bits=32; dt.dl_tensor.shape=dsh;
    if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) { rc=14; goto done; }
    {
      size_t required_itopk = (size_t)topk * 4;
      if (required_itopk < 512) required_itopk = 512;
      if (sp->itopk_size < required_itopk) sp->itopk_size = required_itopk;
      sp->search_width = 4;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, nd, sizeof(unsigned)*nq*topk, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist) cudaMemcpy(out_dist, dd, sizeof(float)*nq*topk, cudaMemcpyDefault);
  }
done:
  if (sp) cuvsCagraSearchParamsDestroy(sp);
  if (dd) cuvsRMMFree(h->res, dd, sizeof(float)*nq*topk);
  if (nd) cuvsRMMFree(h->res, nd, sizeof(unsigned)*nq*topk);
  if (qd) cuvsRMMFree(h->res, qd, qbytes);
  free(qh);
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h->hbuf);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
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
