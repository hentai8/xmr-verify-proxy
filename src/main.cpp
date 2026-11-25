#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "config.h"
#include "json.hpp"
#include "log.h"
#include "net.h"
#include "proxy.h"
#include "randomx_engine.h"
#include "stats.h"
#include "mockpool.h"
#include "testminer.h"
#include "util.h"

using nlohmann::json;

namespace {

constexpr size_t kNonceOffset = 39;

LogLevel parseLevel(const std::string& s) {
  const std::string v = util::toLower(s);
  if (v == "debug") return LogLevel::Debug;
  if (v == "warn" || v == "warning") return LogLevel::Warn;
  if (v == "error") return LogLevel::Error;
  return LogLevel::Info;
}

// ---------------------------------------------------------------------------
// selftest: prove the RandomX implementation against real accepted shares.
// ---------------------------------------------------------------------------
int runSelftest(const std::string& vectorsPath, bool fastMode, int initThreads) {
  std::ifstream in(vectorsPath, std::ios::binary);
  if (!in) {
    std::printf("cannot open vectors file: %s\n", vectorsPath.c_str());
    return 2;
  }
  json vectors;
  try {
    in >> vectors;
  } catch (const std::exception& e) {
    std::printf("bad vectors json: %s\n", e.what());
    return 2;
  }
  if (!vectors.is_array()) {
    std::printf("vectors file must be a JSON array\n");
    return 2;
  }

  LOG().setLevel(LogLevel::Info);
  std::printf("== RandomX selftest (%s mode) ==\n", fastMode ? "fast" : "light");
  std::printf("== %zu vectors from %s ==\n", vectors.size(), vectorsPath.c_str());

  RandomxEngine engine;
  RxOptions opt;
  opt.fastMode = fastMode;
  opt.datasetInitThreads = initThreads;
  opt.vmPool = 2;
  std::string err;
  if (!engine.start(opt, &err)) {
    std::printf("engine start failed: %s\n", err.c_str());
    return 2;
  }

  int pass = 0, fail = 0;

  if (fastMode && !vectors.empty()) {
    engine.noteJobSeeds(vectors[0]["job"]["seed_hash"].get<std::string>(), "");
    std::printf("waiting for dataset (fast mode) ...\n");
    const int64_t deadline = util::nowMs() + 120000;
    while (!engine.snapshot().currentDatasetReady && util::nowMs() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::printf("dataset ready after %.2fs\n", engine.snapshot().currentDatasetInitSec);
  }

  for (const auto& v : vectors) {
    const std::string blobHex = v["job"]["blob"].get<std::string>();
    const std::string nonce = v["nonce"].get<std::string>();
    const std::string seed = v["job"]["seed_hash"].get<std::string>();
    const std::string expect = v["result"].get<std::string>();
    const std::string hashInputHex = v["hash_input_hex"].get<std::string>();
    const int index = v.value("index", 0);

    uint8_t input[76];
    uint8_t nonceBytes[4];
    if (blobHex.size() != 152 || !util::hexDecodeInto(blobHex, input, 76) ||
        !util::hexDecodeInto(nonce, nonceBytes, 4)) {
      std::printf("vector #%d: malformed blob/nonce\n", index);
      ++fail;
      continue;
    }
    std::memcpy(input + kNonceOffset, nonceBytes, 4);

    if (util::hexEncode(input, 76) != hashInputHex) {
      std::printf("vector #%d: assembled hash_input differs from recorded hash_input_hex\n", index);
      ++fail;
      continue;
    }

    engine.noteJobSeeds(seed, "");

    uint8_t out[32];
    const double t0 = util::nowSteady();
    if (!engine.verify(input, sizeof(input), seed, 120000, out, &err)) {
      std::printf("vector #%d: verify error: %s\n", index, err.c_str());
      ++fail;
      continue;
    }
    const double ms = (util::nowSteady() - t0) * 1000.0;
    const std::string got = util::hexEncode(out, 32);
    const bool ok = (got == expect);

    const uint64_t shareTarget = util::target64FromHash(out);
    const uint64_t shareDiff = util::difficultyFromTarget64(shareTarget);
    const uint32_t compact = util::compactFromHex(v["job"]["target"].get<std::string>());
    const uint64_t jobTarget64 = util::target64FromCompact(compact);

    std::printf("vector #%d: %s  %6.2f ms  seed=%s..  shareDiff=%llu %s job target\n", index,
                ok ? "PASS" : "FAIL", ms, seed.substr(0, 8).c_str(),
                static_cast<unsigned long long>(shareDiff),
                shareTarget <= jobTarget64 ? "meets" : "BELOW");
    if (!ok) {
      std::printf("   expected %s\n   got      %s\n", expect.c_str(), got.c_str());
      ++fail;
    } else {
      ++pass;
    }
  }

  engine.stop();
  std::printf("== result: %d passed, %d failed ==\n", pass, fail);
  return fail == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// bench
// ---------------------------------------------------------------------------
void printBench(const char* label, const BenchResult& r) {
  if (!r.ok) {
    std::printf("%-16s FAILED: %s\n", label, r.error.c_str());
    return;
  }
  std::printf("%-16s %8.1f H/s   single hash: mean %.2f ms, p50 %.2f ms, p95 %.2f ms\n", label,
              r.hashesPerSec, r.msPerHashMean, r.p50Ms, r.p95Ms);
  if (r.fullMem) {
    std::printf("%-16s dataset init %.2fs (2080 MB, %d init threads)\n", "", r.datasetInitSec,
                r.initThreads);
  }
}

int runBench(const std::string& seedHex, bool fastOnly, bool lightOnly, int threads, double seconds,
             bool hugePages, int initThreads) {
  uint8_t seed[32];
  if (seedHex.size() != 64 || !util::hexDecodeInto(seedHex, seed, 32)) {
    std::printf("bench needs a 64-char hex seed (use the seed_hash from your pool's job)\n");
    return 2;
  }
  std::printf("== RandomX benchmark ==\n");
  std::printf("seed=%s threads=%d duration=%.0fs largepages=%s\n", seedHex.c_str(), threads,
              seconds, hugePages ? "on" : "off");
  if (!lightOnly) {
    BenchResult r = RandomxEngine::bench(true, threads, seconds, seed, hugePages, initThreads);
    printBench("fast (FULL_MEM)", r);
  }
  if (!fastOnly) {
    BenchResult r = RandomxEngine::bench(false, threads, seconds, seed, hugePages, initThreads);
    printBench("light", r);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------
Proxy* g_proxy = nullptr;
StatsServer* g_stats = nullptr;

BOOL WINAPI ctrlHandler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
    std::printf("\nshutting down...\n");
    if (g_proxy) g_proxy->stop();
    if (g_stats) g_stats->stop();
    std::fflush(stdout);
    std::_Exit(0);
  }
  return FALSE;
}

int runProxy(const Config& cfg) {
  LOG().setLevel(parseLevel(cfg.logLevel));
  if (!cfg.logFile.empty()) {
    std::error_code ec;
    std::filesystem::path p(cfg.logFile);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    if (!LOG().openFile(cfg.logFile)) {
      LOG().warn("could not open log file %s (continuing with console only)", cfg.logFile.c_str());
    }
  }

  std::printf("\n");
  std::printf("  xmr-verify-proxy  -  RandomX share verification proxy\n");
  std::printf("  ----------------------------------------------------\n");
  std::printf("  listen    : %s\n", cfg.listen.c_str());
  std::printf("  upstream  : %s\n", cfg.upstream.c_str());
  std::printf("  randomx   : %s mode, %s\n", cfg.fastMode ? "fast" : "light",
              cfg.fastMode ? "dataset ~2080 MB" : "cache only ~256 MB");
  std::printf("  rules     : hash=%d target=%d duplicate=%d jobid=%d dropBadJobs=%d\n",
              cfg.checkHash, cfg.checkTarget, cfg.checkDuplicate, cfg.checkJobId,
              cfg.blockInvalidJobs);
  std::printf("  stats     : %s\n", cfg.statsEnabled ? cfg.statsListen.c_str() : "off");
  std::printf("\n");

  RandomxEngine engine;
  RxOptions opt;
  opt.fastMode = cfg.fastMode;
  opt.prebuildNextSeed = cfg.prebuildNextSeed;
  opt.hugePages = cfg.hugePages;
  opt.datasetInitThreads = cfg.datasetInitThreads;
  opt.vmPool = cfg.vmPool;
  opt.maxCachedSeeds = cfg.maxCachedSeeds;

  std::string err;
  if (!engine.start(opt, &err)) {
    LOG().error("RandomX engine failed to start: %s", err.c_str());
    return 1;
  }

  StatsRegistry registry;
  registry.startedMs = util::nowMs();

  Proxy proxy(cfg, &engine, &registry);
  if (!proxy.start(&err)) {
    LOG().error("proxy failed to start: %s", err.c_str());
    return 1;
  }

  StatsServer stats(&registry, &engine, &cfg);
  if (!stats.start(&err)) {
    LOG().error("stats server failed to start: %s", err.c_str());
    return 1;
  }

  g_proxy = &proxy;
  g_stats = &stats;
  SetConsoleCtrlHandler(ctrlHandler, TRUE);

  LOG().info("ready - point your miner at %s", cfg.listen.c_str());
  proxy.run();

  stats.stop();
  proxy.stop();
  engine.stop();
  return 0;
}

void usage() {
  std::printf(
      "xmr-verify-proxy - RandomX share verification proxy for Monero pools\n"
      "\n"
      "usage:\n"
      "  xmr-verify-proxy run [--config <path>] [--upstream host:port] [--listen host:port]\n"
      "  xmr-verify-proxy selftest [--vectors <path>] [--light] [--init-threads N]\n"
      "  xmr-verify-proxy bench --seed <64 hex> [--threads N] [--seconds S]\n"
      "                         [--light|--fast] [--hugepages] [--init-threads N]\n"
      "  xmr-verify-proxy mine --target host:port --login <addr>/<worker>\n"
      "                        [--fault none|bad-hash|low-diff|unknown-job|dup|all]\n"
      "                        [--threads N] [--timeout S] [--light]\n"
      "  xmr-verify-proxy mockpool [--mock-listen host:port] [--easy]\n"
      "                            [--mode valid|bad-blob|bad-seed|bad-algo|bad-target|all]\n"
      "\n"
      "selftest checks the RandomX implementation against tests/data/submit-vectors.json\n"
      "(9 shares that a real pool accepted). mine and mockpool are for testing this proxy;\n"
      "they are not needed in normal operation.\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string err;
  if (!netInit(&err)) {
    std::printf("winsock init failed: %s\n", err.c_str());
    return 1;
  }

  std::string cmd = argc > 1 ? argv[1] : "run";
  std::string configPath = "config.json";
  std::string vectorsPath = "tests/data/submit-vectors.json";
  std::string seedHex;
  std::string upstreamOverride, listenOverride;
  MinerOptions minerOpt;
  MockPoolOptions mockOpt;
  std::string worker = "kcap";
  bool light = false, fastOnly = false, lightOnly = false, hugePages = false;
  int threads = 1, initThreads = 0;
  double seconds = 10.0;

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::printf("%s needs a value\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--config") configPath = next("--config");
    else if (a == "--vectors") vectorsPath = next("--vectors");
    else if (a == "--seed") seedHex = next("--seed");
    else if (a == "--threads") threads = static_cast<int>(util::parseI64(next("--threads"), 1));
    else if (a == "--init-threads") initThreads = static_cast<int>(util::parseI64(next("--init-threads"), 0));
    else if (a == "--seconds") seconds = std::atof(next("--seconds").c_str());
    else if (a == "--light") { light = true; lightOnly = true; }
    else if (a == "--fast") fastOnly = true;
    else if (a == "--hugepages") hugePages = true;
    else if (a == "--mode") mockOpt.mode = next("--mode");
    else if (a == "--easy") mockOpt.easyTarget = true;
    else if (a == "--mock-listen") mockOpt.listen = next("--mock-listen");
    else if (a == "--target") minerOpt.target = next("--target");
    else if (a == "--login") minerOpt.login = next("--login");
    else if (a == "--worker") worker = next("--worker");
    else if (a == "--fault") minerOpt.fault = next("--fault");
    else if (a == "--timeout") minerOpt.mineTimeoutSec = static_cast<int>(util::parseI64(next("--timeout"), 900));
    else if (a == "--upstream") upstreamOverride = next("--upstream");
    else if (a == "--listen") listenOverride = next("--listen");
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else {
      std::printf("unknown option: %s\n", a.c_str());
      usage();
      return 2;
    }
  }

  int rc = 0;
  if (cmd == "selftest") {
    if (initThreads <= 0) initThreads = 8;
    rc = runSelftest(vectorsPath, !light, initThreads);
  } else if (cmd == "bench") {
    if (initThreads <= 0) initThreads = 8;
    rc = runBench(seedHex, fastOnly, lightOnly, threads, seconds, hugePages, initThreads);
  } else if (cmd == "mockpool") {
    rc = runMockPool(mockOpt);
  } else if (cmd == "mine") {
    minerOpt.threads = std::max(1, threads);
    minerOpt.lightMode = light;
    if (minerOpt.login.find('/') == std::string::npos && !minerOpt.login.empty()) {
      minerOpt.login += "/" + worker;
    }
    rc = runTestMiner(minerOpt);
  } else if (cmd == "run") {
    Config cfg;
    bool loaded = false;
    if (!loadConfig(configPath, &cfg, &err, &loaded)) {
      std::printf("config error: %s\n", err.c_str());
      netCleanup();
      return 2;
    }
    if (!loaded) {
      std::printf("note: %s not found, using built-in defaults\n", configPath.c_str());
    }
    if (!upstreamOverride.empty()) cfg.upstream = upstreamOverride;
    if (!listenOverride.empty()) cfg.listen = listenOverride;
    rc = runProxy(cfg);
  } else {
    usage();
    rc = 2;
  }

  netCleanup();
  return rc;
}
