#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct RxOptions {
  bool fastMode = true;            // FULL_MEM dataset (~2.1 GB) instead of light mode
  bool prebuildNextSeed = true;    // pre-build the dataset for next_seed_hash
  bool hugePages = false;          // needs SeLockMemoryPrivilege (admin); auto-falls back
  int datasetInitThreads = 8;
  int vmPool = 4;                  // concurrent verification VMs per seed
  int maxCachedSeeds = 3;
};

struct RxSnapshot {
  std::string currentSeed;
  std::string nextSeed;
  bool currentCacheReady = false;
  bool currentDatasetReady = false;
  double currentDatasetInitSec = 0;
  double lastVerifyMs = 0;
  uint64_t datasetMemBytes = 0;
  uint64_t cacheMemBytes = 0;
  bool hugePagesActive = false;
  int contexts = 0;
};

struct BenchResult {
  bool ok = false;
  std::string error;
  bool fullMem = false;
  int threads = 0;
  double datasetInitSec = 0;
  int initThreads = 0;
  uint64_t hashes = 0;
  double seconds = 0;
  double hashesPerSec = 0;
  double msPerHashMean = 0;
  double p50Ms = 0;
  double p95Ms = 0;
  double minMs = 0;
  double maxMs = 0;
};

class RandomxEngine {
 public:
  RandomxEngine() = default;
  ~RandomxEngine();

  bool start(const RxOptions& opt, std::string* err);
  void stop();

  // Called whenever a job shows up. Registers seed_hash / next_seed_hash and
  // schedules cache+dataset construction in the background.
  void noteJobSeeds(const std::string& seedHex, const std::string& nextSeedHex);

  // RandomX-1 (rx/0) hash of `input` with `seedHex`. Returns false when the seed
  // is unknown or its cache is not ready within timeoutMs. Hash goes to out[32].
  bool verify(const uint8_t* input, size_t len, const std::string& seedHex, uint32_t timeoutMs,
              uint8_t out[32], std::string* err);

  RxSnapshot snapshot();

  // Standalone measurement: builds its own cache (and dataset when fullMem) and
  // reports single-hash latency plus aggregate throughput.
  static BenchResult bench(bool fullMem, int threads, double seconds, const uint8_t seed[32],
                           bool hugePages, int initThreads);

 private:
  struct SeedCtx;

  SeedCtx* findCtxLocked(const std::string& hex);
  SeedCtx* ensureCtxLocked(const std::string& hex);
  void builderLoop();
  void buildCache(SeedCtx* ctx);
  void buildDataset(SeedCtx* ctx);
  void destroyVms(SeedCtx* ctx);
  void pruneLocked();

  RxOptions opt_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<SeedCtx*> ctxs_;
  std::string currentHex_;
  std::string nextHex_;
  bool stopping_ = false;
  bool started_ = false;
  std::thread builder_;
  double lastVerifyMs_ = 0;
  bool hugePagesActive_ = false;
};
