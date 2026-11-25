#include "randomx_engine.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>

#include "log.h"
#include "util.h"

extern "C" {
#include "randomx.h"
}

namespace {

randomx_flags baseFlags() {
  // JIT + hardware AES where the CPU supports it, exactly what a real miner uses.
  return randomx_get_flags();
}

randomx_flags withLargePages(randomx_flags f) {
  return static_cast<randomx_flags>(f | RANDOMX_FLAG_LARGE_PAGES);
}

constexpr uint32_t kVerifyWaitSliceMs = 50;

struct LatencyStats {
  double p50 = 0, p95 = 0, mn = 0, mx = 0, mean = 0;
};

LatencyStats computeLatency(std::vector<double>& v) {
  LatencyStats s;
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  auto at = [&](double q) {
    size_t idx = static_cast<size_t>(q * (v.size() - 1));
    return v[idx];
  };
  s.mn = v.front();
  s.mx = v.back();
  s.p50 = at(0.50);
  s.p95 = at(0.95);
  double sum = 0;
  for (double x : v) sum += x;
  s.mean = sum / static_cast<double>(v.size());
  return s;
}

}  // namespace

struct RandomxEngine::SeedCtx {
  std::string hex;
  uint8_t key[32] = {0};
  randomx_cache* cache = nullptr;
  randomx_dataset* dataset = nullptr;
  std::atomic<bool> cacheReady{false};
  std::atomic<bool> cacheFailed{false};
  std::atomic<bool> datasetReady{false};
  bool datasetBuilding = false;
  bool datasetFailed = false;
  double datasetInitSec = 0;
  int64_t lastUsedMs = 0;
  std::vector<randomx_vm*> idleFast;
  std::vector<randomx_vm*> idleLight;
  int vmsOut = 0;
  int totalVms = 0;
};

RandomxEngine::~RandomxEngine() { stop(); }

bool RandomxEngine::start(const RxOptions& opt, std::string* err) {
  std::lock_guard<std::mutex> lock(mu_);
  if (started_) return true;
  opt_ = opt;
  stopping_ = false;
  started_ = true;
  builder_ = std::thread([this] { builderLoop(); });
  (void)err;
  return true;
}

void RandomxEngine::stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_) return;
    stopping_ = true;
  }
  cv_.notify_all();
  if (builder_.joinable()) builder_.join();

  std::lock_guard<std::mutex> lock(mu_);
  for (SeedCtx* c : ctxs_) {
    destroyVms(c);
    if (c->dataset) {
      randomx_release_dataset(c->dataset);
      c->dataset = nullptr;
    }
    if (c->cache) {
      randomx_release_cache(c->cache);
      c->cache = nullptr;
    }
    delete c;
  }
  ctxs_.clear();
  currentHex_.clear();
  nextHex_.clear();
  started_ = false;
}

RandomxEngine::SeedCtx* RandomxEngine::findCtxLocked(const std::string& hex) {
  for (SeedCtx* c : ctxs_) {
    if (c->hex == hex) return c;
  }
  return nullptr;
}

RandomxEngine::SeedCtx* RandomxEngine::ensureCtxLocked(const std::string& hex) {
  SeedCtx* c = findCtxLocked(hex);
  if (c) return c;
  if (static_cast<int>(ctxs_.size()) >= std::max(1, opt_.maxCachedSeeds)) {
    // Drop the least recently used context that is not current/next.
    SeedCtx* victim = nullptr;
    for (SeedCtx* cand : ctxs_) {
      if (cand->hex == currentHex_ || cand->hex == nextHex_) continue;
      if (cand->vmsOut > 0) continue;
      if (!victim || cand->lastUsedMs < victim->lastUsedMs) victim = cand;
    }
    if (victim) {
      ctxs_.erase(std::remove(ctxs_.begin(), ctxs_.end(), victim), ctxs_.end());
      destroyVms(victim);
      if (victim->dataset) randomx_release_dataset(victim->dataset);
      if (victim->cache) randomx_release_cache(victim->cache);
      LOG().info("RandomX: dropped cached seed %s", victim->hex.c_str());
      delete victim;
    }
  }
  c = new SeedCtx();
  c->hex = hex;
  std::vector<uint8_t> key = util::hexDecode(hex);
  if (key.size() == 32) std::memcpy(c->key, key.data(), 32);
  c->lastUsedMs = util::nowMs();
  ctxs_.push_back(c);
  LOG().info("RandomX: new seed %s registered", hex.c_str());
  return c;
}

void RandomxEngine::noteJobSeeds(const std::string& seedHex, const std::string& nextSeedHex) {
  std::string seed = util::toLower(seedHex);
  std::string next = util::toLower(nextSeedHex);

  std::lock_guard<std::mutex> lock(mu_);
  if (!started_ || stopping_) return;
  if (!util::isHex(seed, 64)) return;

  if (seed != currentHex_) {
    const bool hadPrevious = !currentHex_.empty();
    currentHex_ = seed;
    ensureCtxLocked(seed);
    if (hadPrevious) {
      LOG().info("RandomX: active seed switched to %s", seed.c_str());
    }
  }

  if (!next.empty() && util::isHex(next, 64)) {
    if (next != nextHex_) {
      nextHex_ = next;
      ensureCtxLocked(next);
      if (opt_.prebuildNextSeed && opt_.fastMode) {
        LOG().info("RandomX: pre-building dataset for next seed %s", next.c_str());
      }
    }
  } else if (!nextHex_.empty() && seed == nextHex_) {
    nextHex_.clear();
  }

  cv_.notify_all();
}

void RandomxEngine::builderLoop() {
  for (;;) {
    SeedCtx* work = nullptr;
    int stage = 0;
    {
      std::unique_lock<std::mutex> lock(mu_);
      if (stopping_) return;
      for (SeedCtx* c : ctxs_) {
        if (!c->cacheReady && !c->cacheFailed) {
          work = c;
          stage = 1;
          break;
        }
      }
      if (!work) {
        for (SeedCtx* c : ctxs_) {
          const bool wanted = (c->hex == currentHex_) ||
                              (!nextHex_.empty() && c->hex == nextHex_);
          if (wanted && opt_.fastMode && c->cacheReady && !c->datasetReady &&
              c->dataset == nullptr && !c->datasetBuilding && !c->datasetFailed) {
            c->datasetBuilding = true;
            work = c;
            stage = 2;
            break;
          }
        }
      }
      if (!work) {
        cv_.wait_for(lock, std::chrono::milliseconds(500));
        continue;
      }
    }

    if (stage == 1) {
      buildCache(work);
    } else {
      buildDataset(work);
    }
  }
}

void RandomxEngine::buildCache(SeedCtx* ctx) {
  const double t0 = util::nowSteady();
  randomx_flags cf = static_cast<randomx_flags>(baseFlags() & RANDOMX_FLAG_JIT);
  randomx_cache* cache = nullptr;

  if (opt_.hugePages) {
    cache = randomx_alloc_cache(withLargePages(cf));
    if (cache) {
      std::lock_guard<std::mutex> lock(mu_);
      hugePagesActive_ = true;
    }
  }
  if (!cache) cache = randomx_alloc_cache(cf);
  if (!cache) cache = randomx_alloc_cache(RANDOMX_FLAG_DEFAULT);

  if (!cache) {
    std::lock_guard<std::mutex> lock(mu_);
    ctx->cacheFailed = true;
    cv_.notify_all();
    LOG().error("RandomX: cache allocation failed (need ~256 MB free)");
    return;
  }

  randomx_init_cache(cache, ctx->key, sizeof(ctx->key));
  const double secs = util::nowSteady() - t0;

  {
    std::lock_guard<std::mutex> lock(mu_);
    ctx->cache = cache;
    ctx->cacheReady = true;
    cv_.notify_all();
  }
  LOG().info("RandomX: cache ready for seed %s in %.2fs", ctx->hex.c_str(), secs);
  cv_.notify_all();
}

void RandomxEngine::buildDataset(SeedCtx* ctx) {
  randomx_cache* cache = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    cache = ctx->cache;
  }
  if (!cache) {
    std::lock_guard<std::mutex> lock(mu_);
    ctx->datasetBuilding = false;
    return;
  }

  const double t0 = util::nowSteady();
  randomx_dataset* ds = nullptr;
  if (opt_.hugePages) ds = randomx_alloc_dataset(RANDOMX_FLAG_LARGE_PAGES);
  if (!ds) ds = randomx_alloc_dataset(RANDOMX_FLAG_DEFAULT);
  if (!ds) {
    std::lock_guard<std::mutex> lock(mu_);
    ctx->datasetBuilding = false;
    ctx->datasetFailed = true;
    cv_.notify_all();
    LOG().error("RandomX: dataset allocation failed (needs ~2080 MB free); staying in light mode");
    return;
  }

  const unsigned long items = randomx_dataset_item_count();
  int threads = std::max(1, std::min(opt_.datasetInitThreads, 64));
  if (static_cast<unsigned long>(threads) > items) threads = static_cast<int>(items);
  LOG().info("RandomX: initialising dataset (%.0f MB, %d threads) for seed %s",
             static_cast<double>(items) * 64.0 / (1024.0 * 1024.0), threads, ctx->hex.c_str());

  std::vector<std::thread> pool;
  for (int i = 0; i < threads; ++i) {
    const unsigned long begin = items * static_cast<unsigned long>(i) / static_cast<unsigned long>(threads);
    const unsigned long end = items * static_cast<unsigned long>(i + 1) / static_cast<unsigned long>(threads);
    const unsigned long count = end - begin;
    if (i == threads - 1) {
      randomx_init_dataset(ds, cache, begin, count);
    } else {
      pool.emplace_back([ds, cache, begin, count] { randomx_init_dataset(ds, cache, begin, count); });
    }
  }
  for (auto& t : pool) t.join();

  const double secs = util::nowSteady() - t0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    ctx->dataset = ds;
    ctx->datasetReady = true;
    ctx->datasetBuilding = false;
    ctx->datasetInitSec = secs;
    cv_.notify_all();
  }
  LOG().info("RandomX: dataset ready for seed %s in %.2fs (fast mode active)", ctx->hex.c_str(), secs);
  cv_.notify_all();
}

void RandomxEngine::destroyVms(SeedCtx* ctx) {
  for (randomx_vm* vm : ctx->idleFast) randomx_destroy_vm(vm);
  for (randomx_vm* vm : ctx->idleLight) randomx_destroy_vm(vm);
  ctx->idleFast.clear();
  ctx->idleLight.clear();
  ctx->totalVms = 0;
}

bool RandomxEngine::verify(const uint8_t* input, size_t len, const std::string& seedHex,
                           uint32_t timeoutMs, uint8_t out[32], std::string* err) {
  const std::string hex = util::toLower(seedHex);
  if (!util::isHex(hex, 64)) {
    if (err) *err = "malformed seed";
    return false;
  }

  randomx_vm* vm = nullptr;
  SeedCtx* ctx = nullptr;
  bool usedFast = false;
  const int64_t deadline = util::nowMs() + static_cast<int64_t>(timeoutMs);
  const randomx_flags base = baseFlags();

  {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
      if (!started_ || stopping_) {
        if (err) *err = "engine stopped";
        return false;
      }
      ctx = findCtxLocked(hex);
      if (!ctx) {
        if (err) *err = "seed not registered";
        return false;
      }
      if (ctx->cacheFailed) {
        if (err) *err = "seed cache init failed";
        return false;
      }
      if (!ctx->cacheReady) {
        if (util::nowMs() >= deadline) {
          if (err) *err = "seed cache not ready";
          return false;
        }
        cv_.wait_for(lock, std::chrono::milliseconds(kVerifyWaitSliceMs));
        continue;
      }

      ctx->lastUsedMs = util::nowMs();
      const bool canFast = opt_.fastMode && ctx->datasetReady && ctx->dataset != nullptr;

      if (canFast) {
        if (!ctx->idleFast.empty()) {
          vm = ctx->idleFast.back();
          ctx->idleFast.pop_back();
          ctx->vmsOut++;
          usedFast = true;
        } else {
          if (ctx->totalVms >= opt_.vmPool && !ctx->idleLight.empty()) {
            randomx_destroy_vm(ctx->idleLight.back());
            ctx->idleLight.pop_back();
            ctx->totalVms--;
          }
          if (ctx->totalVms < opt_.vmPool) {
            randomx_flags f = static_cast<randomx_flags>(base | RANDOMX_FLAG_FULL_MEM);
            vm = randomx_create_vm(f, ctx->cache, ctx->dataset);
            if (!vm) vm = randomx_create_vm(static_cast<randomx_flags>(RANDOMX_FLAG_FULL_MEM | RANDOMX_FLAG_JIT),
                                            ctx->cache, ctx->dataset);
            if (vm) {
              ctx->totalVms++;
              ctx->vmsOut++;
              usedFast = true;
            }
          }
        }
      } else {
        if (!ctx->idleLight.empty()) {
          vm = ctx->idleLight.back();
          ctx->idleLight.pop_back();
          ctx->vmsOut++;
        } else if (ctx->totalVms < opt_.vmPool) {
          vm = randomx_create_vm(base, ctx->cache, nullptr);
          if (!vm) vm = randomx_create_vm(RANDOMX_FLAG_JIT, ctx->cache, nullptr);
          if (vm) {
            ctx->totalVms++;
            ctx->vmsOut++;
          }
        }
      }

      if (vm) break;
      if (util::nowMs() >= deadline) {
        if (err) *err = "timed out waiting for a verification VM";
        return false;
      }
      cv_.wait_for(lock, std::chrono::milliseconds(kVerifyWaitSliceMs));
    }
  }

  const double t0 = util::nowSteady();
  randomx_calculate_hash(vm, input, len, out);
  const double ms = (util::nowSteady() - t0) * 1000.0;

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (usedFast) {
      ctx->idleFast.push_back(vm);
    } else {
      ctx->idleLight.push_back(vm);
    }
    ctx->vmsOut--;
    lastVerifyMs_ = ms;
    cv_.notify_all();
  }
  return true;
}

RxSnapshot RandomxEngine::snapshot() {
  RxSnapshot s;
  std::lock_guard<std::mutex> lock(mu_);
  s.currentSeed = currentHex_;
  s.nextSeed = nextHex_;
  s.lastVerifyMs = lastVerifyMs_;
  s.hugePagesActive = hugePagesActive_;
  s.contexts = static_cast<int>(ctxs_.size());
  SeedCtx* c = findCtxLocked(currentHex_);
  if (c) {
    s.currentCacheReady = c->cacheReady.load();
    s.currentDatasetReady = c->datasetReady.load();
    s.currentDatasetInitSec = c->datasetInitSec;
  }
  uint64_t dsBytes = 0, cBytes = 0;
  for (SeedCtx* ctx : ctxs_) {
    if (ctx->dataset) dsBytes += randomx_dataset_item_count() * 64ULL;
    if (ctx->cache) cBytes += 256ULL * 1024 * 1024;
  }
  s.datasetMemBytes = dsBytes;
  s.cacheMemBytes = cBytes;
  return s;
}

BenchResult RandomxEngine::bench(bool fullMem, int threads, double seconds, const uint8_t seed[32],
                                 bool hugePages, int initThreads) {
  BenchResult r;
  r.fullMem = fullMem;
  r.threads = std::max(1, threads);
  r.initThreads = std::max(1, initThreads);

  randomx_flags cacheFlags = static_cast<randomx_flags>(baseFlags() & RANDOMX_FLAG_JIT);
  randomx_cache* cache = nullptr;
  if (hugePages) cache = randomx_alloc_cache(withLargePages(cacheFlags));
  if (!cache) cache = randomx_alloc_cache(cacheFlags);
  if (!cache) cache = randomx_alloc_cache(RANDOMX_FLAG_DEFAULT);
  if (!cache) {
    r.error = "cache allocation failed";
    return r;
  }

  double t0 = util::nowSteady();
  randomx_init_cache(cache, seed, 32);
  const double cacheInitSec = util::nowSteady() - t0;

  randomx_dataset* ds = nullptr;
  double dsInitSec = 0;
  if (fullMem) {
    if (hugePages) ds = randomx_alloc_dataset(RANDOMX_FLAG_LARGE_PAGES);
    if (!ds) ds = randomx_alloc_dataset(RANDOMX_FLAG_DEFAULT);
    if (!ds) {
      randomx_release_cache(cache);
      r.error = "dataset allocation failed";
      return r;
    }
    const unsigned long items = randomx_dataset_item_count();
    int it = std::max(1, std::min(initThreads, 64));
    t0 = util::nowSteady();
    std::vector<std::thread> pool;
    for (int i = 0; i < it; ++i) {
      const unsigned long begin = items * static_cast<unsigned long>(i) / static_cast<unsigned long>(it);
      const unsigned long end = items * static_cast<unsigned long>(i + 1) / static_cast<unsigned long>(it);
      const unsigned long count = end - begin;
      if (i == it - 1) {
        randomx_init_dataset(ds, cache, begin, count);
      } else {
        pool.emplace_back([ds, cache, begin, count] { randomx_init_dataset(ds, cache, begin, count); });
      }
    }
    for (auto& t : pool) t.join();
    dsInitSec = util::nowSteady() - t0;
  }
  r.datasetInitSec = dsInitSec;
  (void)cacheInitSec;

  const randomx_flags base = baseFlags();
  const randomx_flags vmFlags =
      fullMem ? static_cast<randomx_flags>(base | RANDOMX_FLAG_FULL_MEM) : base;

  std::vector<randomx_vm*> vms;
  for (int i = 0; i < r.threads; ++i) {
    randomx_vm* vm = fullMem ? randomx_create_vm(vmFlags, cache, ds)
                             : randomx_create_vm(vmFlags, cache, nullptr);
    if (!vm) {
      for (randomx_vm* v : vms) randomx_destroy_vm(v);
      if (ds) randomx_release_dataset(ds);
      randomx_release_cache(cache);
      r.error = "vm creation failed";
      return r;
    }
    vms.push_back(vm);
  }

  std::vector<std::vector<double>> lat(static_cast<size_t>(r.threads));
  std::atomic<uint64_t> total{0};
  const double runStart = util::nowSteady();
  const double runEnd = runStart + seconds;

  std::vector<std::thread> workers;
  for (int i = 0; i < r.threads; ++i) {
    workers.emplace_back([&, i] {
      uint8_t input[76];
      std::memset(input, 0, sizeof(input));
      uint32_t counter = static_cast<uint32_t>(i) * 7919u;
      uint64_t n = 0;
      auto& mine = lat[static_cast<size_t>(i)];
      mine.reserve(static_cast<size_t>(seconds * 2000));
      while (util::nowSteady() < runEnd) {
        input[39] = static_cast<uint8_t>(counter & 0xFF);
        input[40] = static_cast<uint8_t>((counter >> 8) & 0xFF);
        input[41] = static_cast<uint8_t>((counter >> 16) & 0xFF);
        input[42] = static_cast<uint8_t>((counter >> 24) & 0xFF);
        ++counter;
        uint8_t hash[32];
        const double s = util::nowSteady();
        randomx_calculate_hash(vms[static_cast<size_t>(i)], input, sizeof(input), hash);
        mine.push_back((util::nowSteady() - s) * 1000.0);
        ++n;
      }
      total += n;
    });
  }
  for (auto& t : workers) t.join();
  const double elapsed = util::nowSteady() - runStart;

  std::vector<double> all;
  for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
  LatencyStats ls = computeLatency(all);

  r.ok = true;
  r.hashes = total.load();
  r.seconds = elapsed;
  r.hashesPerSec = elapsed > 0 ? static_cast<double>(r.hashes) / elapsed : 0;
  r.msPerHashMean = ls.mean;
  r.p50Ms = ls.p50;
  r.p95Ms = ls.p95;
  r.minMs = ls.mn;
  r.maxMs = ls.mx;

  for (randomx_vm* v : vms) randomx_destroy_vm(v);
  if (ds) randomx_release_dataset(ds);
  randomx_release_cache(cache);
  return r;
}
