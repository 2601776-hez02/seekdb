// MiniVectorDB - self-implemented HNSW index (L2 distance).
// The index owns its vector data: row-major float32 array indexed by node id.
// Build: batch-parallel — worker threads compute neighbor selections against a
// stable graph snapshot; the apply phase mutates the graph serially, so there
// are no concurrent reads/writes on the adjacency lists.
#pragma once

#include "mvdb.h"

#include <cerrno>
#include <cmath>
#include <random>
#include <thread>
#include <atomic>
#include <functional>
#include <unistd.h>

namespace mvdb {

#define MVDB_HNSW_V2 2

#if defined(__x86_64__) && (defined(__AVX2__) || defined(MVDB_WANT_AVX2))
#include <immintrin.h>
#endif

static inline bool writeAllFd(int fd, const void* p, size_t n) {
  while (n) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p = static_cast<const char*>(p) + w;
    n -= size_t(w);
  }
  return true;
}

static inline bool cpu_avx2() {
#if defined(__x86_64__)
  return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
  return false;
#endif
}

// Squared L2 over float32 vectors. AVX2+FMA path is runtime-dispatched; the
// scalar fallback covers CPUs without AVX2 (and non-x86 builds).
inline float l2sq_simd(const float* a, const float* b, size_t d) {
#if defined(__x86_64__) && (defined(__AVX2__) || defined(MVDB_WANT_AVX2))
  if (cpu_avx2()) {
    __m256 acc0 = _mm256_setzero_ps();
    __m256 acc1 = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 16 <= d; i += 16) {
      __m256 x0 = _mm256_loadu_ps(a + i);
      __m256 y0 = _mm256_loadu_ps(b + i);
      __m256 x1 = _mm256_loadu_ps(a + i + 8);
      __m256 y1 = _mm256_loadu_ps(b + i + 8);
      __m256 d0 = _mm256_sub_ps(x0, y0);
      __m256 d1 = _mm256_sub_ps(x1, y1);
      acc0 = _mm256_fmadd_ps(d0, d0, acc0);
      acc1 = _mm256_fmadd_ps(d1, d1, acc1);
    }
    for (; i + 8 <= d; i += 8) {
      __m256 x = _mm256_loadu_ps(a + i);
      __m256 y = _mm256_loadu_ps(b + i);
      __m256 dd = _mm256_sub_ps(x, y);
      acc0 = _mm256_fmadd_ps(dd, dd, acc0);
    }
    __m256 sum = _mm256_add_ps(acc0, acc1);
    float r[8];
    _mm256_storeu_ps(r, sum);
    float s = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    for (; i < d; i++) {
      float x = a[i] - b[i];
      s += x * x;
    }
    return s;
  }
#endif
  float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
  size_t i = 0;
  for (; i + 4 <= d; i += 4) {
    float d0 = a[i] - b[i], d1 = a[i+1] - b[i+1], d2 = a[i+2] - b[i+2], d3 = a[i+3] - b[i+3];
    s0 += d0 * d0; s1 += d1 * d1; s2 += d2 * d2; s3 += d3 * d3;
  }
  for (; i < d; i++) { float x = a[i] - b[i]; s0 += x * x; }
  return (s0 + s1) + (s2 + s3);
}

// ---------------------------------------------------------------------------
// HNSW over L2. vecs_ is d * N floats (row-major, node id = row). pks_[node]
// maps a node to its primary key. Node ids grow monotonically; updates mark
// the old node dead and append a new one.
// ---------------------------------------------------------------------------
class HNSWIndex {
 public:
  HNSWIndex(size_t dim, int M = 16, int efConstruction = 200, int efSearch = 64)
      : dim_(dim), M_(M), M0_(2 * M), efC_(efConstruction), efS_(efSearch),
        levelMult_(1.0 / std::log(double(M))) {}

  size_t dim() const { return dim_; }
  size_t size() const { return vecs_.size() / dim_; }

  // bulk build: feed all vectors (row-major, n*d floats) + pks
  void Build(const float* data, const int64_t* pks, size_t n, int threads = 0) {
    Clear();
    if (n == 0) return;
    vecs_.assign(data, data + n * dim_);
    pks_.assign(pks, pks + n);
    pkToNode_.reserve(n);
    for (size_t i = 0; i < n; i++) pkToNode_[pks_[i]] = uint32_t(i);
    GrowGraph(n);
    if (threads <= 0) {
      threads = int(std::thread::hardware_concurrency());
      if (threads < 1) threads = 1;
      if (threads > 8) threads = 8;
    }
    if (n < 4096 || threads <= 1) {
      HNSWBuildSeq();
    } else {
      HNSWBuildParallel(threads);
    }
  }

  // incremental: append one vector; returns the new node id
  uint32_t Append(const float* vec, int64_t pk) {
    size_t node = size();
    vecs_.insert(vecs_.end(), vec, vec + dim_);
    pks_.push_back(pk);
    pkToNode_[pk] = uint32_t(node);
    GrowGraph(node + 1);
    Scratch sc;
    PrepareScratch(sc);
    int lvl = SampleLevel(sc);
    levels_[size_t(node)] = lvl;
    alive_[size_t(node)] = 1;
    if (entry_ < 0) { entry_ = int(node); EnsureLevels(lvl); return uint32_t(node); }
    std::vector<std::vector<uint32_t>> selByLevel;
    ComputeLinks(uint32_t(node), lvl, sc, selByLevel);
    ApplyLinks(uint32_t(node), lvl, selByLevel);
    return uint32_t(node);
  }

  void MarkDead(uint32_t node) { if (node < levels_.size()) alive_[node] = 0; }
  bool Alive(uint32_t node) const { return node < levels_.size() && alive_[node] == 1; }
  int64_t Pk(uint32_t node) const { return pks_[node]; }
  uint32_t NodeOf(int64_t pk) const {
    auto it = pkToNode_.find(pk);
    return it == pkToNode_.end() ? UINT32_MAX : it->second;
  }

  void DebugStats(size_t& l0links, size_t& zeroDeg, int& maxLvl) const {
    l0links = 0;
    zeroDeg = 0;
    maxLvl = 0;
    if (lvlAdj_.empty()) return;
    for (auto& lst : lvlAdj_[0]) {
      l0links += lst.size();
      if (lst.empty()) zeroDeg++;
    }
    for (int l : levels_) maxLvl = std::max(maxLvl, l);
  }
  std::vector<uint32_t> NeighborsOf(uint32_t node) const {
    if (lvlAdj_.empty() || node >= lvlAdj_[0].size()) return {};
    return lvlAdj_[0][node];
  }
  int LevelOf(uint32_t node) const { return levels_[size_t(node)]; }
  int Entry() const { return entry_; }

  // Query search: returns ascending (distSq, node) candidates, up to ef.
  // `filter(nodeId)` is pre-filter style: filtered nodes are traversable but
  // excluded from results. If the returned count < k the caller must fall
  // back to an exact scan.
  void Search(const float* qvec, int ef, int k,
              std::vector<std::pair<float, uint32_t>>& out,
              const std::function<bool(uint32_t)>& filter = nullptr,
              size_t* visitedOut = nullptr,
              uint32_t entryOverride = UINT32_MAX) const {
    out.clear();
    if (entry_ < 0) return;
    ef = std::max(ef, k);
    auto cmpMax = [](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& b) {
      return a.first < b.first;   // heap top = largest
    };
    auto cmpMin = [](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& b) {
      return a.first > b.first;   // heap top = smallest
    };
    Scratch& sc = qScratch_;
    sc.cand.clear();
    sc.res.clear();
    sc.gen++;
    if (sc.mark.size() < levels_.size()) sc.mark.resize(levels_.size(), 0);
    uint32_t gen = sc.gen;

    int ep = (entryOverride != UINT32_MAX) ? int(entryOverride) : entry_;
    float dep = DistQ(qvec, uint32_t(ep));
    for (int lc = levels_[size_t(ep)]; lc > 0; lc--) {
      bool moved = true;
      while (moved) {
        moved = false;
        for (uint32_t nb : lvlAdj_[size_t(lc)][size_t(ep)]) {
          if (!Alive(nb)) continue;
          float d = DistQ(qvec, nb);
          if (d < dep) { dep = d; ep = int(nb); moved = true; }
        }
      }
    }
    sc.cand.emplace_back(dep, uint32_t(ep));
    sc.res.emplace_back(dep, uint32_t(ep));
    sc.mark[size_t(ep)] = gen;
    size_t visited = 1;
    while (!sc.cand.empty()) {
      std::pop_heap(sc.cand.begin(), sc.cand.end(), cmpMin);
      auto cur = sc.cand.back();
      sc.cand.pop_back();
      if (cur.first > sc.res.front().first && int(sc.res.size()) >= ef) break;
      for (uint32_t nb : lvlAdj_[0][size_t(cur.second)]) {
        if (!Alive(nb) || sc.mark[size_t(nb)] == gen) continue;
        sc.mark[size_t(nb)] = gen;
        visited++;
        float d = DistQ(qvec, nb);
        bool pass = !filter || filter(nb);
        if (pass) {
          sc.cand.emplace_back(d, nb);
          std::push_heap(sc.cand.begin(), sc.cand.end(), cmpMin);
          if (d < sc.res.front().first || int(sc.res.size()) < ef) {
            sc.res.emplace_back(d, nb);
            std::push_heap(sc.res.begin(), sc.res.end(), cmpMax);
            while (int(sc.res.size()) > ef) {
              std::pop_heap(sc.res.begin(), sc.res.end(), cmpMax);
              sc.res.pop_back();
            }
          }
        }
      }
    }
    if (visitedOut) *visitedOut = visited;
    out.reserve(sc.res.size());
    for (size_t i = sc.res.size(); i-- > 0;) {
      std::pop_heap(sc.res.begin(), sc.res.begin() + i + 1, cmpMax);
      out.push_back(sc.res[i]);
    }
    std::reverse(out.begin(), out.end());
  }

  const float* VecOf(uint32_t node) const { return vecs_.data() + size_t(node) * dim_; }

  // ---- persistence (used at clean checkpoints; rebuild is the fallback) ----
  // Streamed serialization to a file descriptor; identical layout to
  // Serialize(), but memory-bounded (chunked writes + running CRC) so a
  // checkpoint can run when the graph already fills most of the container RAM.
  Err SerializeTo(int fd) const {
    Err err = Err::OK;
    char buf[1 << 16];
    size_t n = 0;
    uint32_t crc = 0xFFFFFFFFu;
    auto flush = [&]() -> bool {
      if (n == 0) return true;
      if (!writeAllFd(fd, buf, n)) { err = Err::IOError; return false; }
      n = 0;
      return true;
    };
    auto put = [&](const void* p, size_t len) {
      if (err != Err::OK) return;
      crc = crc32_soft(static_cast<const uint8_t*>(p), len, crc);
      while (len) {
        size_t chunk = std::min(len, sizeof(buf) - n);
        std::memcpy(buf + n, p, chunk);
        n += chunk;
        len -= chunk;
        p = static_cast<const char*>(p) + chunk;
        if (n == sizeof(buf) && !flush()) return;
      }
    };
    // magic (excluded from the crc, matching Serialize())
    const char magic[8] = {'H', 'N', 'S', 'W', 'I', 'D', 'X', '1'};
    if (!writeAllFd(fd, magic, 8)) return Err::IOError;
    uint64_t u64;
    uint32_t u32;
    uint8_t u8;
    u64 = uint64_t(dim_);
    put(&u64, 8);
    u64 = uint64_t(size());
    put(&u64, 8);
    u32 = uint32_t(entry_ >= 0 ? entry_ : 0xFFFFFFFFu);
    put(&u32, 4);
    for (size_t i = 0; i < size() && err == Err::OK; i++) {
      u32 = uint32_t(levels_[i] < 0 ? 0xFFFFFFFFu : levels_[i]);
      put(&u32, 4);
      u8 = alive_[i] ? 1 : 0;
      put(&u8, 1);
    }
    u64 = uint64_t(lvlAdj_.size());
    put(&u64, 8);
    for (size_t l = 0; l < lvlAdj_.size() && err == Err::OK; l++) {
      u64 = uint64_t(lvlAdj_[l].size());
      put(&u64, 8);
      for (auto& lst : lvlAdj_[l]) {
        u32 = uint32_t(lst.size());
        put(&u32, 4);
        // chunk the neighbour ids through the buffer
        size_t off = 0;
        while (off < lst.size() && err == Err::OK) {
          size_t cnt = std::min(size_t(1024), lst.size() - off);
          put(lst.data() + off, cnt * sizeof(uint32_t));
          off += cnt;
        }
      }
    }
    // vectors in 1M-float chunks
    const size_t FCHUNK = size_t(1) << 20;
    for (size_t off = 0; off < vecs_.size() && err == Err::OK; off += FCHUNK) {
      size_t cnt = std::min(FCHUNK, vecs_.size() - off);
      put(vecs_.data() + off, cnt * sizeof(float));
    }
    for (int64_t p : pks_) {
      if (err != Err::OK) break;
      put(&p, 8);
    }
    if (!flush()) return Err::IOError;
    uint32_t final = crc;
    if (!writeAllFd(fd, &final, 4)) return Err::IOError;
    return err;
  }

  void Serialize(std::string& out) const {
    out.clear();
    out.append("HNSWIDX1", 8);
    put_u64(out, uint64_t(dim_));
    put_u64(out, uint64_t(size()));
    put_u32(out, uint32_t(entry_ >= 0 ? entry_ : 0xFFFFFFFFu));
    // levels + alive
    for (size_t i = 0; i < size(); i++) {
      put_u32(out, uint32_t(levels_[i] < 0 ? 0xFFFFFFFFu : levels_[i]));
      put_u8(out, alive_[i] ? 1 : 0);
    }
    // adjacency per level
    size_t maxLvl = lvlAdj_.size();
    put_u64(out, uint64_t(maxLvl));
    for (size_t l = 0; l < maxLvl; l++) {
      put_u64(out, uint64_t(lvlAdj_[l].size()));
      for (auto& lst : lvlAdj_[l]) {
        put_u32(out, uint32_t(lst.size()));
        for (uint32_t id : lst) put_u32(out, id);
      }
    }
    // vectors (raw float32)
    out.append((const char*)vecs_.data(), vecs_.size() * sizeof(float));
    // pks
    for (int64_t p : pks_) put_i64(out, p);
    // trailing crc over everything after the magic
    uint32_t crc = crc32(out.data() + 8, out.size() - 8);
    put_u32(out, crc);
  }

  bool Deserialize(const char* data, size_t len) {
    if (len < 8 + 8 + 8 + 4 + 4) return false;
    if (std::memcmp(data, "HNSWIDX1", 8) != 0) return false;
    uint32_t crc;
    std::memcpy(&crc, data + len - 4, 4);
    if (crc32(data + 8, len - 12) != crc) return false;
    ByteReader r(data + 8, len - 12);
    uint64_t dim = r.u64();
    uint64_t n = r.u64();
    uint32_t entry = r.u32();
    if (dim != dim_ || !r.ok()) return false;
    if (n > (1ull << 26)) return false;
    vecs_.clear();
    pks_.clear();
    levels_.assign(size_t(n), -1);
    alive_.assign(size_t(n), 1);
    for (size_t i = 0; i < size_t(n); i++) {
      uint32_t lv = r.u32();
      levels_[i] = lv == 0xFFFFFFFFu ? -1 : int(lv);
      alive_[i] = r.u8();
    }
    uint64_t maxLvl = r.u64();
    if (maxLvl > 256) return false;
    lvlAdj_.clear();
    lvlAdj_.resize(size_t(maxLvl));
    for (size_t l = 0; l < size_t(maxLvl); l++) {
      uint64_t cnt = r.u64();
      if (cnt != n) return false;
      lvlAdj_[l].resize(size_t(n));
      for (size_t i = 0; i < size_t(n); i++) {
        uint32_t deg = r.u32();
        auto& lst = lvlAdj_[l][i];
        lst.resize(size_t(deg));
        for (size_t k = 0; k < size_t(deg); k++) {
          lst[k] = r.u32();
          if (lst[k] >= n) return false;
        }
      }
    }
    if (!r.ok()) return false;
    size_t vecBytes = size_t(n) * size_t(dim_) * sizeof(float);
    if (r.pos + vecBytes > r.n) return false;
    vecs_.assign((const float*)(r.p + r.pos), (const float*)(r.p + r.pos + vecBytes));
    r.pos += vecBytes;
    pks_.resize(size_t(n));
    for (size_t i = 0; i < size_t(n); i++) pks_[i] = r.i64();
    if (!r.ok()) return false;
    entry_ = entry == 0xFFFFFFFFu ? -1 : int(entry);
    if (entry_ >= int(n)) entry_ = -1;
    pkToNode_.clear();
    pkToNode_.reserve(size_t(n));
    for (size_t i = 0; i < size_t(n); i++) pkToNode_[pks_[i]] = uint32_t(i);
    return true;
  }

 private:
  size_t dim_;
  int M_, M0_, efC_, efS_;
  double levelMult_;
  std::vector<float> vecs_;
  std::vector<int64_t> pks_;
  std::unordered_map<int64_t, uint32_t> pkToNode_;
  std::vector<int> levels_;
  std::vector<uint8_t> alive_;
  std::vector<std::vector<std::vector<uint32_t>>> lvlAdj_;  // [level][node] -> nbrs
  int entry_ = -1;

  struct Scratch {
    std::vector<uint32_t> mark;
    uint32_t gen = 0;
    std::vector<std::pair<float, uint32_t>> cand, res, visited;
    std::mt19937_64 rng{0x9E3779B97F4A7C15ull};
  };
  mutable Scratch qScratch_;

  void Clear() {
    vecs_.clear();
    pks_.clear();
    pkToNode_.clear();
    levels_.clear();
    alive_.clear();
    lvlAdj_.clear();
    entry_ = -1;
  }

  void GrowGraph(size_t n) {
    levels_.resize(n, -1);
    alive_.resize(n, 1);
    for (auto& lv : lvlAdj_) lv.resize(n);
  }

  void PrepareScratch(Scratch& sc) {
    if (sc.mark.size() < levels_.size()) sc.mark.resize(levels_.size(), 0);
  }

  void EnsureLevels(int lvl) {
    while (int(lvlAdj_.size()) <= lvl) {
      lvlAdj_.emplace_back();
      lvlAdj_.back().resize(levels_.size());
    }
  }

  void HNSWBuildSeq() {
    entry_ = -1;
    for (auto& lv : lvlAdj_) lv.assign(lv.size(), std::vector<uint32_t>());
    Scratch sc;
    for (size_t i = 0; i < size(); i++) {
      PrepareScratch(sc);
      int lvl = SampleLevel(sc);
      levels_[i] = lvl;
      alive_[i] = 1;
      if (entry_ < 0) { entry_ = int(i); EnsureLevels(lvl); continue; }
      std::vector<std::vector<uint32_t>> selByLevel;
      ComputeLinks(uint32_t(i), lvl, sc, selByLevel);
      ApplyLinks(uint32_t(i), lvl, selByLevel);
    }
  }

  void HNSWBuildParallel(int threads) {
    entry_ = -1;
    for (auto& lv : lvlAdj_) lv.assign(lv.size(), std::vector<uint32_t>());
    size_t n = size();
    if (n == 0) return;
    // node 0 becomes the entry point first (sequential, empty graph)
    {
      std::mt19937_64 rng(0x9E3779B97F4A7C15ull);
      double r = double(rng() + 1) / (double(std::mt19937_64::max()) + 2.0);
      levels_[0] = int(-std::log(r) * levelMult_);
      alive_[0] = 1;
      entry_ = 0;
      EnsureLevels(levels_[0]);
    }
    size_t batch = std::max<size_t>(512, size_t(threads) * 256);
    size_t done = 1;
    while (done < n) {
      size_t from = done;
      size_t to = std::min(n, done + batch);
      size_t count = to - from;
      struct Job {
        int lvl = 0;
        std::vector<std::vector<uint32_t>> sel;  // index 0 = level lvl ... index lvl = level 0
      };
      std::vector<Job> jobs(count);
      std::atomic<size_t> next{0};
      std::vector<std::thread> pool;
      for (int t = 0; t < threads; t++) {
        pool.emplace_back([&, t]() {
          Scratch sc;
          sc.rng.seed(0x9E3779B97F4A7C15ull ^ uint64_t(t + 1) * 0x9E3779B97F4A7C15ull);
          while (true) {
            size_t i = next.fetch_add(1);
            if (i >= count) break;
            uint32_t node = uint32_t(from + i);
            PrepareScratch(sc);
            int lvl = SampleLevel(sc);
            jobs[i].lvl = lvl;
            ComputeLinks(node, lvl, sc, jobs[i].sel);
          }
        });
      }
      for (auto& th : pool) th.join();
      // serialized apply phase
      for (size_t i = 0; i < count; i++) {
        uint32_t node = uint32_t(from + i);
        levels_[size_t(node)] = jobs[i].lvl;
        alive_[size_t(node)] = 1;
        ApplyLinks(node, jobs[i].lvl, jobs[i].sel);
      }
      done = to;
    }
  }

  int SampleLevel(Scratch& sc) {
    if (getenv("MVDB_FLAT_LEVEL")) return 0;
    double r = double(sc.rng() + 1) / (double(std::mt19937_64::max()) + 2.0);
    return int(-std::log(r) * levelMult_);
  }

  float Dist(uint32_t a, uint32_t b) const {
    return l2sq_simd(vecs_.data() + size_t(a) * dim_, vecs_.data() + size_t(b) * dim_, dim_);
  }
  float DistQ(const float* q, uint32_t b) const {
    return l2sq_simd(q, vecs_.data() + size_t(b) * dim_, dim_);
  }

  // Search layer lc for the vector of node q (build-time); fills `visited`
  // with the CLOSEST visited (dist, node) pairs (capped at 3*ef).
  void SearchLayerFor(uint32_t q, uint32_t ep, int ef, int lc,
                      Scratch& sc, std::vector<std::pair<float, uint32_t>>& visited) const {
    visited.clear();
    auto cmpMax = [](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& b) {
      return a.first < b.first;   // heap top = largest
    };
    auto cmpMin = [](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& b) {
      return a.first > b.first;   // heap top = smallest
    };
    sc.cand.clear();
    sc.res.clear();
    sc.gen++;
    uint32_t gen = sc.gen;
    size_t cap = size_t(ef) * 3;
    float dep = Dist(ep, q);
    sc.cand.emplace_back(dep, ep);
    sc.res.emplace_back(dep, ep);
    sc.mark[size_t(ep)] = gen;
    visited.emplace_back(dep, ep);
    while (!sc.cand.empty()) {
      std::pop_heap(sc.cand.begin(), sc.cand.end(), cmpMin);
      auto cur = sc.cand.back();
      sc.cand.pop_back();
      if (cur.first > sc.res.front().first && int(sc.res.size()) >= ef) break;
      for (uint32_t nb : lvlAdj_[size_t(lc)][size_t(cur.second)]) {
        if (!Alive(nb) || sc.mark[size_t(nb)] == gen) continue;
        sc.mark[size_t(nb)] = gen;
        float d = Dist(nb, q);
        // keep the CLOSEST `cap` visited nodes (farthest-first heap)
        if (d < visited.front().first || visited.size() < cap) {
          visited.emplace_back(d, nb);
          std::push_heap(visited.begin(), visited.end(), cmpMax);
          if (visited.size() > cap) {
            std::pop_heap(visited.begin(), visited.end(), cmpMax);
            visited.pop_back();
          }
        }
        sc.cand.emplace_back(d, nb);
        std::push_heap(sc.cand.begin(), sc.cand.end(), cmpMin);
        if (d < sc.res.front().first || int(sc.res.size()) < ef) {
          sc.res.emplace_back(d, nb);
          std::push_heap(sc.res.begin(), sc.res.end(), cmpMax);
          while (int(sc.res.size()) > ef) {
            std::pop_heap(sc.res.begin(), sc.res.end(), cmpMax);
            sc.res.pop_back();
          }
        }
      }
    }
  }

  // worker phase: compute neighbor selections for `node` at all levels.
  // selByLevel[i] = neighbors at level (lvl - i) ... stored descending:
  // index 0 = level lvl, index lvl = level 0.
  void ComputeLinks(uint32_t node, int lvl, Scratch& sc,
                    std::vector<std::vector<uint32_t>>& selByLevel) {
    selByLevel.assign(size_t(lvl) + 1, {});
    int ep = entry_;
    // greedy descent from the entry point's top level down to lvl+1
    for (int lc = levels_[size_t(ep)]; lc > lvl; lc--) {
      bool moved = true;
      while (moved) {
        moved = false;
        float best = Dist(uint32_t(ep), node);
        for (uint32_t nb : lvlAdj_[size_t(lc)][size_t(ep)]) {
          float d = Dist(nb, node);
          if (d < best) { best = d; ep = int(nb); moved = true; }
        }
      }
    }
    for (int lc = std::min(lvl, levels_[size_t(entry_)]); lc >= 0; lc--) {
      SearchLayerFor(node, uint32_t(ep), efC_, lc, sc, sc.visited);
      int maxDeg = (lc == 0) ? M0_ : M_;
      std::sort(sc.visited.begin(), sc.visited.end());
      auto sel = SelectNeighbors(sc.visited, node, maxDeg);
      selByLevel[size_t(lvl - lc)] = std::move(sel);
      ep = selByLevel[size_t(lvl - lc)].empty()
               ? ep
               : int(selByLevel[size_t(lvl - lc)].front());  // closest selected
    }
  }

  // serialized apply phase: mutate the graph
  void ApplyLinks(uint32_t node, int lvl, const std::vector<std::vector<uint32_t>>& selByLevel) {
    EnsureLevels(lvl);
    for (int lc = lvl; lc >= 0; lc--) {
      const auto& sel = selByLevel[size_t(lvl - lc)];
      int maxDeg = (lc == 0) ? M0_ : M_;
      for (uint32_t nb : sel) {
        lvlAdj_[size_t(lc)][size_t(node)].push_back(nb);
        lvlAdj_[size_t(lc)][nb].push_back(node);
      }
      for (uint32_t nb : sel) Trim(nb, lc, maxDeg);
      Trim(node, lc, maxDeg);
    }
    if (lvl > levels_[size_t(entry_)]) entry_ = int(node);
  }

  void Trim(uint32_t node, int level, int maxDeg) {
    auto& lst = lvlAdj_[size_t(level)][size_t(node)];
    if (int(lst.size()) <= maxDeg) return;
    std::vector<std::pair<float, uint32_t>> nb;
    nb.reserve(lst.size());
    for (uint32_t id : lst) nb.emplace_back(Dist(node, id), id);
    std::sort(nb.begin(), nb.end());
    lst.resize(size_t(maxDeg));
    for (int i = 0; i < maxDeg; i++) lst[size_t(i)] = nb[size_t(i)].second;
  }

  // hnswlib-style diversity heuristic over closest-first candidates
  std::vector<uint32_t> SelectNeighbors(
      const std::vector<std::pair<float, uint32_t>>& cand, uint32_t node, int maxDeg) const {
    std::vector<uint32_t> sel;
    for (const auto& e : cand) {
      if (int(sel.size()) >= maxDeg) break;
      uint32_t c = e.second;
      if (c == node) continue;
      bool good = true;
      for (uint32_t s : sel) {
        if (Dist(c, s) < Dist(c, node)) { good = false; break; }
      }
      if (good) sel.push_back(c);
    }
    return sel;
  }
};

}  // namespace mvdb
