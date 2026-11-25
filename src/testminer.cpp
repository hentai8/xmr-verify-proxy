#include "testminer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "json.hpp"
#include "log.h"
#include "net.h"
#include "randomx_engine.h"
#include "util.h"

using nlohmann::json;

namespace {

constexpr size_t kNonceOffset = 39;
constexpr size_t kBlobBytes = 76;

struct Job {
  std::string jobId;
  std::string blobHex;
  std::vector<uint8_t> blob;
  std::string seedHash;
  std::string nextSeedHash;
  uint32_t compact = 0;
  uint64_t target64 = 0;
  int64_t height = 0;

  uint64_t difficulty() const { return util::difficultyFromTarget64(target64); }
};

struct Conn {
  Socket sock;
  LineReader reader;
  int64_t nextId = 1;

  bool connect(const Endpoint& ep, int timeoutMs, std::string* err) {
    if (!sock.connectTo(ep, timeoutMs, err)) return false;
    sock.setRecvTimeout(200);
    return true;
  }

  bool sendLine(const std::string& line) {
    const std::string payload = line + "\n";
    return sock.sendAll(payload.data(), payload.size()) >= 0;
  }

  bool waitForId(int64_t id, int timeoutMs, json* out, std::string* err, bool verbose) {
    const int64_t deadline = util::nowMs() + timeoutMs;
    for (;;) {
      std::string line, rerr;
      const LineReader::Result r = reader.next(sock, &line, &rerr);
      if (r == LineReader::Result::Timeout) {
        if (util::nowMs() > deadline) {
          if (err) *err = "timed out waiting for a reply";
          return false;
        }
        continue;
      }
      if (r == LineReader::Result::Closed) {
        if (err) *err = "connection closed by peer";
        return false;
      }
      if (r == LineReader::Result::Error) {
        if (err) *err = rerr;
        return false;
      }
      if (line.empty()) continue;
      json msg;
      try {
        msg = json::parse(line);
      } catch (...) {
        continue;
      }
      if (msg.contains("id") && msg["id"].is_number() && msg["id"].get<int64_t>() == id) {
        *out = msg;
        return true;
      }
      if (msg.contains("method") && verbose) {
        std::printf("  <- (notification) %s\n", line.substr(0, 240).c_str());
      }
    }
  }
};

bool jobFromJson(const json& jp, Job* job, std::string* err) {
  if (!jp.is_object()) {
    *err = "job not an object";
    return false;
  }
  job->jobId = jp.value("job_id", std::string());
  job->blobHex = util::toLower(jp.value("blob", std::string()));
  job->seedHash = util::toLower(jp.value("seed_hash", std::string()));
  job->nextSeedHash = util::toLower(jp.value("next_seed_hash", std::string()));
  job->compact = util::compactFromHex(util::toLower(jp.value("target", std::string())));
  job->target64 = util::target64FromCompact(job->compact);
  job->height = jp.value("height", static_cast<int64_t>(0));
  job->blob.resize(kBlobBytes);
  if (job->jobId.empty() || !util::hexDecodeInto(job->blobHex, job->blob.data(), kBlobBytes) ||
      !util::isHex(job->seedHash, 64) || job->compact == 0) {
    *err = "job is missing job_id/blob/seed_hash/target";
    return false;
  }
  return true;
}

struct Submission {
  std::string nonceHex;
  std::string resultHex;
  uint64_t shareDiff = 0;
};

bool submitShare(Conn& conn, const std::string& sessionId, const std::string& jobId,
                 const Submission& sub, bool verbose, json* response, std::string* err) {
  const int64_t id = conn.nextId++;
  json req;
  req["id"] = id;
  req["jsonrpc"] = "2.0";
  req["method"] = "submit";
  req["params"] = {{"id", sessionId}, {"job_id", jobId}, {"nonce", sub.nonceHex},
                   {"result", sub.resultHex}};
  const std::string line = req.dump();
  if (verbose) std::printf("  -> %s\n", line.c_str());
  if (!conn.sendLine(line)) {
    if (err) *err = "send failed";
    return false;
  }
  if (!conn.waitForId(id, 30000, response, err, verbose)) return false;
  if (verbose) std::printf("  <- %s\n", response->dump().c_str());
  return true;
}

enum class Verdict { LocalBlocked, PoolAccepted, PoolRejected, Error };

Verdict classify(const json& resp, std::string* detail) {
  if (resp.contains("error") && !resp["error"].is_null()) {
    const json& e = resp["error"];
    if (detail) {
      *detail = e.is_object() ? e.value("message", std::string("error")) : e.dump();
    }
    return Verdict::LocalBlocked;
  }
  if (resp.contains("result") && resp["result"].is_object()) {
    const std::string status = resp["result"].value("status", std::string());
    if (detail) *detail = status;
    return status == "OK" ? Verdict::PoolAccepted : Verdict::PoolRejected;
  }
  if (detail) *detail = "unexpected reply shape";
  return Verdict::Error;
}

const char* verdictName(Verdict v) {
  switch (v) {
    case Verdict::LocalBlocked: return "BLOCKED LOCALLY (pool never saw it)";
    case Verdict::PoolAccepted: return "POOL ACCEPTED";
    case Verdict::PoolRejected: return "POOL REJECTED";
    case Verdict::Error:        return "UNEXPECTED REPLY";
  }
  return "?";
}

int compareAndReport(const char* label, Verdict want, const json& resp) {
  std::string detail;
  const Verdict got = classify(resp, &detail);
  const bool ok = (got == want);
  std::printf("  [%s] %s -> %s%s%s\n", ok ? "PASS" : "FAIL", label, verdictName(got),
              detail.empty() ? "" : "  (", detail.empty() ? "" : (detail + ")").c_str());
  return ok ? 0 : 1;
}

// Mines while following job updates, exactly like a real miner does.
struct FoundShare {
  std::string jobId;
  Submission sub;
};

bool mineShare(Conn& conn, RandomxEngine& engine, std::shared_ptr<Job> initialJob, int threads,
               int timeoutSec, int verboseNotifications, FoundShare* out) {
  auto state = std::make_shared<std::mutex>();
  std::shared_ptr<Job> current = initialJob;

  std::atomic<bool> found{false};
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> hashes{0};
  std::mutex foundMu;
  FoundShare result;

  std::printf("mining against %s (diff %llu, %d thread(s), timeout %ds)\n",
              initialJob->jobId.c_str(), static_cast<unsigned long long>(initialJob->difficulty()),
              std::max(1, threads), timeoutSec);

  const int nthreads = std::max(1, threads);
  std::vector<std::thread> workers;
  for (int t = 0; t < nthreads; ++t) {
    workers.emplace_back([&, t] {
      uint8_t input[kBlobBytes];
      std::shared_ptr<Job> job;
      uint32_t nonce = static_cast<uint32_t>(t);
      uint64_t local = 0;
      std::string jobId;

      while (!found.load(std::memory_order_relaxed) && !stop.load(std::memory_order_relaxed)) {
        {
          std::lock_guard<std::mutex> lock(*state);
          job = current;
        }
        if (!job) break;
        if (job->jobId != jobId) {
          jobId = job->jobId;
          std::memcpy(input, job->blob.data(), kBlobBytes);
          nonce = static_cast<uint32_t>(t);
        }

        const uint32_t n = nonce;
        input[kNonceOffset + 0] = static_cast<uint8_t>(n & 0xFF);
        input[kNonceOffset + 1] = static_cast<uint8_t>((n >> 8) & 0xFF);
        input[kNonceOffset + 2] = static_cast<uint8_t>((n >> 16) & 0xFF);
        input[kNonceOffset + 3] = static_cast<uint8_t>((n >> 24) & 0xFF);
        nonce += static_cast<uint32_t>(nthreads);

        uint8_t hash[32];
        std::string err;
        if (!engine.verify(input, sizeof(input), job->seedHash, 60000, hash, &err)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          continue;
        }
        ++local;
        if ((local & 0x3F) == 0) hashes += 64;
        if (util::target64FromHash(hash) <= job->target64) {
          std::lock_guard<std::mutex> lock(foundMu);
          if (!found.exchange(true)) {
            result.jobId = job->jobId;
            result.sub.nonceHex = util::hexEncode(input + kNonceOffset, 4);
            result.sub.resultHex = util::hexEncode(hash, 32);
            result.sub.shareDiff = util::difficultyFromTarget64(util::target64FromHash(hash));
          }
        }
      }
    });
  }

  const double t0 = util::nowSteady();
  const int64_t deadline = util::nowMs() + static_cast<int64_t>(timeoutSec) * 1000;
  int64_t nextProgress = util::nowMs() + 5000;

  while (!found.load() && util::nowMs() < deadline) {
    // Consume whatever the pool pushed meanwhile (vardiff / new block jobs).
    std::string line, rerr;
    const LineReader::Result r = conn.reader.next(conn.sock, &line, &rerr);
    if (r == LineReader::Result::Line && !line.empty()) {
      json msg;
      try {
        msg = json::parse(line);
      } catch (...) {
        msg = json();
      }
      if (msg.contains("method") && msg["method"] == "job") {
        Job next;
        std::string jerr;
        if (jobFromJson(msg["params"], &next, &jerr)) {
          const bool changed = next.jobId != initialJob->jobId;
          {
            std::lock_guard<std::mutex> lock(*state);
            current = std::make_shared<Job>(next);
          }
          engine.noteJobSeeds(next.seedHash, next.nextSeedHash);
          if (changed) {
            std::printf("  ... followup job %s height=%lld diff=%llu\n", next.jobId.c_str(),
                        static_cast<long long>(next.height),
                        static_cast<unsigned long long>(next.difficulty()));
          }
        }
      } else if (msg.contains("method") && verboseNotifications > 0) {
        std::printf("  ... notification: %s\n", line.substr(0, 160).c_str());
      }
    } else if (r == LineReader::Result::Closed || r == LineReader::Result::Error) {
      std::printf("  ... connection dropped while mining (%s)\n", rerr.c_str());
      break;
    }

    const int64_t now = util::nowMs();
    if (now >= nextProgress) {
      nextProgress = now + 5000;
      const double elapsed = util::nowSteady() - t0;
      if (elapsed > 0.5) {
        std::printf("  ... %.0fs  %llu hashes  %.0f H/s\n", elapsed,
                    static_cast<unsigned long long>(hashes.load()),
                    static_cast<double>(hashes.load()) / elapsed);
      }
    }
  }

  found = true;
  stop = true;
  for (auto& w : workers) w.join();

  const double elapsed = util::nowSteady() - t0;
  if (result.sub.resultHex.empty()) return false;
  std::printf("found share: job=%s nonce=%s diff=%llu after %.1fs (%.0f H/s)\n",
              result.jobId.c_str(), result.sub.nonceHex.c_str(),
              static_cast<unsigned long long>(result.sub.shareDiff), elapsed,
              static_cast<double>(hashes.load()) / std::max(0.001, elapsed));
  *out = result;
  return true;
}

}  // namespace

int runTestMiner(const MinerOptions& opt) {
  Endpoint ep;
  if (!parseEndpoint(opt.target, &ep)) {
    std::printf("bad target: %s\n", opt.target.c_str());
    return 2;
  }
  if (opt.login.empty()) {
    std::printf("--login <address>/<worker> is required\n");
    return 2;
  }

  LOG().setLevel(LogLevel::Info);
  std::printf("== test miner -> %s as %s (fault=%s) ==\n", ep.str().c_str(), opt.login.c_str(),
              opt.fault.c_str());

  Conn conn;
  std::string err;
  if (!conn.connect(ep, 15000, &err)) {
    std::printf("connect failed: %s\n", err.c_str());
    return 1;
  }

  const int64_t loginId = conn.nextId++;
  json loginReq;
  loginReq["id"] = loginId;
  loginReq["jsonrpc"] = "2.0";
  loginReq["method"] = "login";
  loginReq["params"] = {{"login", opt.login}, {"pass", opt.pass}, {"agent", opt.agent},
                        {"algo", json::array({"rx/0"})}};
  const std::string loginLine = loginReq.dump();
  std::printf("  -> %s\n", loginLine.c_str());
  if (!conn.sendLine(loginLine)) {
    std::printf("login send failed\n");
    return 1;
  }

  json loginResp;
  if (!conn.waitForId(loginId, 20000, &loginResp, &err, true)) {
    std::printf("no login reply: %s\n", err.c_str());
    return 1;
  }
  std::printf("  <- %s\n", loginResp.dump().c_str());
  if (loginResp.contains("error") && !loginResp["error"].is_null()) {
    std::printf("login rejected by pool\n");
    return 1;
  }

  std::string sessionId;
  Job job;
  const json& result = loginResp["result"];
  if (result.contains("id")) {
    sessionId = result["id"].is_string() ? result["id"].get<std::string>() : result["id"].dump();
  }
  if (!result.contains("job") || !jobFromJson(result["job"], &job, &err)) {
    std::printf("login reply has no usable job: %s\n", err.c_str());
    return 1;
  }
  std::printf("session=%s job=%s height=%lld diff=%llu\n", sessionId.c_str(), job.jobId.c_str(),
              static_cast<long long>(job.height),
              static_cast<unsigned long long>(job.difficulty()));

  RandomxEngine engine;
  RxOptions rxOpt;
  rxOpt.fastMode = !opt.lightMode;
  rxOpt.datasetInitThreads = std::max(2, opt.threads);
  rxOpt.vmPool = std::max(2, opt.threads);
  if (!engine.start(rxOpt, &err)) {
    std::printf("engine start failed: %s\n", err.c_str());
    return 1;
  }
  engine.noteJobSeeds(job.seedHash, job.nextSeedHash);
  std::printf("waiting for RandomX (%s mode) ...\n", opt.lightMode ? "light" : "fast");
  {
    const int64_t dl = util::nowMs() + 180000;
    while (util::nowMs() < dl) {
      RxSnapshot s = engine.snapshot();
      if (opt.lightMode ? s.currentCacheReady : s.currentDatasetReady) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  int failures = 0;
  const std::string fault = opt.fault;

  if (fault == "bad-hash" || fault == "all") {
    Submission sub;
    sub.nonceHex = "01020304";
    // Claims an enormous difficulty (tail u64 == 0) while the real RandomX hash
    // of that blob+nonce is something else entirely.
    sub.resultHex = std::string(64, '0');
    json resp;
    if (submitShare(conn, sessionId, job.jobId, sub, true, &resp, &err)) {
      failures += compareAndReport("bad hash (result != RandomX(blob+nonce))",
                                   Verdict::LocalBlocked, resp);
    } else {
      std::printf("  [FAIL] bad hash: %s\n", err.c_str());
      ++failures;
    }
  }

  if (fault == "low-diff" || fault == "all") {
    uint8_t input[kBlobBytes];
    std::memcpy(input, job.blob.data(), kBlobBytes);
    Submission sub;
    bool have = false;
    for (uint32_t n = 1; n < 5000 && !have; ++n) {
      input[kNonceOffset + 0] = static_cast<uint8_t>(n & 0xFF);
      input[kNonceOffset + 1] = static_cast<uint8_t>((n >> 8) & 0xFF);
      input[kNonceOffset + 2] = static_cast<uint8_t>((n >> 16) & 0xFF);
      input[kNonceOffset + 3] = static_cast<uint8_t>((n >> 24) & 0xFF);
      uint8_t hash[32];
      std::string herr;
      if (!engine.verify(input, sizeof(input), job.seedHash, 60000, hash, &herr)) continue;
      if (util::target64FromHash(hash) > job.target64) {
        sub.nonceHex = util::hexEncode(input + kNonceOffset, 4);
        sub.resultHex = util::hexEncode(hash, 32);
        sub.shareDiff = util::difficultyFromTarget64(util::target64FromHash(hash));
        have = true;
      }
    }
    if (have) {
      std::printf("  (genuine hash, share diff %llu < job diff %llu)\n",
                  static_cast<unsigned long long>(sub.shareDiff),
                  static_cast<unsigned long long>(job.difficulty()));
      json resp;
      if (submitShare(conn, sessionId, job.jobId, sub, true, &resp, &err)) {
        failures += compareAndReport("low difficulty share", Verdict::LocalBlocked, resp);
      } else {
        std::printf("  [FAIL] low-diff: %s\n", err.c_str());
        ++failures;
      }
    }
  }

  if (fault == "unknown-job" || fault == "all") {
    Submission sub;
    sub.nonceHex = "0a0b0c0d";
    sub.resultHex = std::string(64, '0');
    json resp;
    if (submitShare(conn, sessionId, "deadbeefcafe", sub, true, &resp, &err)) {
      failures += compareAndReport("unknown/stale job_id", Verdict::LocalBlocked, resp);
    } else {
      std::printf("  [FAIL] unknown-job: %s\n", err.c_str());
      ++failures;
    }
  }

  FoundShare real;
  bool haveReal = false;
  if (fault == "none" || fault == "dup" || fault == "all") {
    auto initial = std::make_shared<Job>(job);
    if (!mineShare(conn, engine, initial, opt.threads, opt.mineTimeoutSec, 1, &real)) {
      std::printf("  [FAIL] did not find a share within %ds\n", opt.mineTimeoutSec);
      ++failures;
    } else {
      haveReal = true;
      json resp;
      if (submitShare(conn, sessionId, real.jobId, real.sub, true, &resp, &err)) {
        failures += compareAndReport("valid share (mined here, verified by proxy)",
                                     Verdict::PoolAccepted, resp);
      } else {
        std::printf("  [FAIL] valid share: %s\n", err.c_str());
        ++failures;
      }

      if (fault == "dup" || fault == "all") {
        json resp2;
        if (submitShare(conn, sessionId, real.jobId, real.sub, true, &resp2, &err)) {
          failures += compareAndReport("duplicate of an accepted share", Verdict::LocalBlocked,
                                       resp2);
        } else {
          std::printf("  [FAIL] duplicate: %s\n", err.c_str());
          ++failures;
        }
      }
    }
  }

  (void)haveReal;
  engine.stop();
  conn.sock.shutdownBoth();
  std::printf("== test miner done: %s ==\n", failures == 0 ? "ALL EXPECTATIONS MET" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
