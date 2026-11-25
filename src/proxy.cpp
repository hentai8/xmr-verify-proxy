#include "proxy.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <memory>
#include <set>
#include <thread>

#include "json.hpp"
#include "log.h"
#include "util.h"

using nlohmann::json;

namespace {

constexpr size_t kNonceOffset = 39;
constexpr size_t kBlobBytes = 76;
constexpr size_t kMaxNoncesPerJob = 8192;
constexpr int64_t kKeepaliveIdBase = 1000000000;

struct Job {
  std::string jobId;
  std::string blobHex;
  std::string seedHash;
  std::string nextSeedHash;
  std::string algo;
  std::vector<uint8_t> blob;
  uint32_t compactTarget = 0;
  uint64_t target64 = 0;
  int64_t height = 0;
  int64_t receivedMs = 0;

  std::set<std::string> seenNonces;
  std::deque<std::string> nonceOrder;
};

// Structural validation of a pool job. Anything that would make the miner hash
// the wrong thing is rejected here, before the job ever reaches the miner.
bool parseJobJson(const json& jp, const Config& cfg, Job* job, std::string* reason) {
  if (!jp.is_object()) {
    *reason = "job is not an object";
    return false;
  }

  std::string algo = "rx/0";
  auto itAlgo = jp.find("algo");
  if (itAlgo != jp.end() && itAlgo->is_string()) algo = itAlgo->get<std::string>();
  if (!cfg.allowedAlgos.empty()) {
    bool allowed = false;
    for (const auto& a : cfg.allowedAlgos) {
      if (a == algo) allowed = true;
    }
    if (!allowed) {
      *reason = "unsupported algo '" + algo + "'";
      return false;
    }
  }

  std::string jobId;
  auto itId = jp.find("job_id");
  if (itId == jp.end() || !itId->is_string() || itId->get<std::string>().empty()) {
    *reason = "missing job_id";
    return false;
  }
  jobId = itId->get<std::string>();
  if (jobId.size() > 64) {
    *reason = "job_id too long";
    return false;
  }

  std::string blobHex;
  auto itBlob = jp.find("blob");
  if (itBlob == jp.end() || !itBlob->is_string()) {
    *reason = "missing blob";
    return false;
  }
  blobHex = util::toLower(itBlob->get<std::string>());
  if (!util::isHex(blobHex, kBlobBytes * 2)) {
    *reason = "blob is not " + std::to_string(kBlobBytes) + " bytes of hex";
    return false;
  }

  std::string targetHex;
  auto itTarget = jp.find("target");
  if (itTarget == jp.end() || !itTarget->is_string()) {
    *reason = "missing target";
    return false;
  }
  targetHex = util::toLower(itTarget->get<std::string>());
  if (!util::isHex(targetHex, 8)) {
    *reason = "target is not 4 bytes of hex";
    return false;
  }
  const uint32_t compact = util::compactFromHex(targetHex);
  if (compact == 0) {
    *reason = "target is zero";
    return false;
  }

  std::string seedHash;
  auto itSeed = jp.find("seed_hash");
  if (itSeed == jp.end() || !itSeed->is_string()) {
    *reason = "missing seed_hash (required for rx/0)";
    return false;
  }
  seedHash = util::toLower(itSeed->get<std::string>());
  if (!util::isHex(seedHash, 64)) {
    *reason = "seed_hash is not 32 bytes of hex";
    return false;
  }

  std::string nextSeed;
  auto itNext = jp.find("next_seed_hash");
  if (itNext != jp.end() && itNext->is_string()) {
    nextSeed = util::toLower(itNext->get<std::string>());
    if (!nextSeed.empty() && !util::isHex(nextSeed, 64)) {
      *reason = "next_seed_hash is not 32 bytes of hex";
      return false;
    }
  }

  int64_t height = 0;
  auto itHeight = jp.find("height");
  if (itHeight != jp.end() && itHeight->is_number()) {
    height = itHeight->get<int64_t>();
  } else {
    *reason = "missing height";
    return false;
  }

  job->jobId = jobId;
  job->blobHex = blobHex;
  job->blob.resize(kBlobBytes);
  if (!util::hexDecodeInto(blobHex, job->blob.data(), kBlobBytes)) {
    *reason = "blob hex decode failed";
    return false;
  }
  job->seedHash = seedHash;
  job->nextSeedHash = nextSeed;
  job->algo = algo;
  job->compactTarget = compact;
  job->target64 = util::target64FromCompact(compact);
  job->height = height;
  job->receivedMs = util::nowMs();
  return true;
}

std::string idKey(const json& msg) {
  auto it = msg.find("id");
  if (it == msg.end()) return std::string();
  return it->dump();
}

}  // namespace

// ---------------------------------------------------------------------------
// One miner connection == one upstream connection. Nothing is rewritten on the
// wire except that individual lines can be dropped (or answered locally).
// ---------------------------------------------------------------------------
class Session {
 public:
  Session(Proxy& px, Socket miner, std::string peer)
      : px_(px),
        cfg_(px.cfg()),
        miner_(std::move(miner)),
        peer_(std::move(peer)),
        readerMiner_(),
        readerUpstream_() {
    info_ = px_.stats()->addSession(peer_, cfg_.upstream);
    lastIoMs_ = util::nowMs();
  }

  ~Session() {
    px_.untrackSocket(miner_.raw());
    px_.untrackSocket(upstream_.raw());
    px_.stats()->removeSession(info_->id);
    px_.stats()->counters.connectionsActive--;
  }

  void run();

 private:
  bool connectUpstream();
  void minerLoop();
  void upstreamLoop();

  void onMinerLine(const std::string& raw);
  void onUpstreamLine(const std::string& raw);
  bool handleSubmit(const json& msg, const std::string& raw);
  bool registerJob(const json& jobParams, bool fromLogin);

  void sendToMiner(const std::string& payload);
  void sendToUpstream(const std::string& payload);
  void rejectShare(const json& msg, const char* reason);
  void noteBlock(const std::string& reason);

  Job* findJobLocked(const std::string& jobId);
  void pruneJobsLocked();
  void maybeKeepalive();

  Proxy& px_;
  const Config& cfg_;
  Socket miner_;
  Socket upstream_;
  std::string peer_;
  LineReader readerMiner_;
  LineReader readerUpstream_;
  std::shared_ptr<SessionInfo> info_;

  std::mutex minerWriteMu_;
  std::mutex upstreamWriteMu_;
  std::mutex jobMu_;
  std::mutex pendingMu_;

  std::map<std::string, Job> jobs_;
  std::map<std::string, std::string> pendingSubmits_;  // request id -> job_id
  std::set<std::string> internalIds_;                   // our own keepalives

  std::string loginRequestId_;
  std::string login_;
  std::string sessionId_;
  bool sessionEstablished_ = false;
  bool upstreamClosed_ = false;

  std::atomic<int64_t> lastIoMs_{0};
  std::atomic<bool> stopping_{false};
  int64_t kpNext_ = kKeepaliveIdBase;
};

bool Session::connectUpstream() {
  Endpoint ep;
  if (!parseEndpoint(cfg_.upstream, &ep)) {
    LOG().error("[%s] bad upstream address '%s'", peer_.c_str(), cfg_.upstream.c_str());
    return false;
  }
  std::string err;
  if (!upstream_.connectTo(ep, cfg_.connectTimeoutMs, &err)) {
    px_.stats()->counters.upstreamFailures++;
    LOG().error("[%s] upstream connect to %s failed: %s", peer_.c_str(), ep.str().c_str(),
                err.c_str());
    return false;
  }
  px_.stats()->counters.upstreamConnects++;
  px_.trackSocket(upstream_.raw());
  // Short recv timeout so the pump thread can drive keepalives on its own.
  upstream_.setRecvTimeout(1000);
  upstream_.setSendTimeout(15000);
  return true;
}

void Session::run() {
  px_.stats()->counters.connectionsTotal++;
  px_.stats()->counters.connectionsActive++;
  px_.trackSocket(miner_.raw());

  if (!connectUpstream()) {
    miner_.shutdownBoth();
    return;
  }
  LOG().info("[%s] connected, upstream %s ready", peer_.c_str(), cfg_.upstream.c_str());

  std::thread upstreamThread([this] { upstreamLoop(); });
  minerLoop();

  stopping_ = true;
  upstream_.shutdownBoth();
  miner_.shutdownBoth();
  if (upstreamThread.joinable()) upstreamThread.join();
  LOG().info("[%s] session closed", peer_.c_str());
}

void Session::minerLoop() {
  for (;;) {
    std::string line, err;
    const LineReader::Result r = readerMiner_.next(miner_, &line, &err);
    if (r == LineReader::Result::Timeout) continue;
    if (r == LineReader::Result::Closed) {
      LOG().info("[%s] miner closed connection", peer_.c_str());
      return;
    }
    if (r == LineReader::Result::Error) {
      // WSAECONNRESET etc: miners disconnect abruptly all the time, that is normal.
      LOG().info("[%s] miner disconnected (%s)", peer_.c_str(), err.c_str());
      return;
    }
    if (line.empty()) continue;
    lastIoMs_ = util::nowMs();
    onMinerLine(line);
    if (stopping_) return;
  }
}

void Session::upstreamLoop() {
  for (;;) {
    if (stopping_) return;
    std::string line, err;
    const LineReader::Result r = readerUpstream_.next(upstream_, &line, &err);
    if (r == LineReader::Result::Timeout) {
      maybeKeepalive();
      continue;
    }
    if (r == LineReader::Result::Closed) {
      upstreamClosed_ = true;
      if (!stopping_) LOG().warn("[%s] upstream closed the connection", peer_.c_str());
      miner_.shutdownBoth();
      return;
    }
    if (r == LineReader::Result::Error) {
      upstreamClosed_ = true;
      if (!stopping_) LOG().warn("[%s] upstream read error: %s", peer_.c_str(), err.c_str());
      miner_.shutdownBoth();
      return;
    }
    if (line.empty()) continue;
    lastIoMs_ = util::nowMs();
    onUpstreamLine(line);
  }
}

void Session::maybeKeepalive() {
  if (cfg_.keepaliveSec <= 0) return;
  if (!sessionEstablished_ || sessionId_.empty()) return;
  const int64_t now = util::nowMs();
  if (now - lastIoMs_.load() < static_cast<int64_t>(cfg_.keepaliveSec) * 1000) return;

  const int64_t id = kpNext_++;
  {
    std::lock_guard<std::mutex> lock(pendingMu_);
    internalIds_.insert(std::to_string(id));
  }
  char buf[160];
  std::snprintf(buf, sizeof(buf),
                "{\"id\":%lld,\"jsonrpc\":\"2.0\",\"method\":\"keepalived\",\"params\":{\"id\":\"%s\"}}\n",
                static_cast<long long>(id), sessionId_.c_str());
  sendToUpstream(buf);
  lastIoMs_ = now;
  LOG().debug("[%s] sent keepalived #%lld upstream", peer_.c_str(), static_cast<long long>(id));
}

void Session::sendToMiner(const std::string& payload) {
  std::lock_guard<std::mutex> lock(minerWriteMu_);
  if (miner_.sendAll(payload.data(), payload.size()) < 0) {
    miner_.shutdownBoth();
  }
}

void Session::sendToUpstream(const std::string& payload) {
  std::lock_guard<std::mutex> lock(upstreamWriteMu_);
  if (upstream_.sendAll(payload.data(), payload.size()) < 0) {
    LOG().warn("[%s] upstream write failed", peer_.c_str());
    upstream_.shutdownBoth();
  }
}

void Session::noteBlock(const std::string& reason) {
  std::lock_guard<std::mutex> lock(info_->noteMu);
  info_->lastBlockReason = reason;
}

void Session::rejectShare(const json& msg, const char* reason) {
  json resp;
  auto it = msg.find("id");
  resp["id"] = (it == msg.end()) ? json(nullptr) : *it;
  resp["jsonrpc"] = "2.0";
  resp["error"] = {{"code", -1}, {"message", reason}};
  resp["result"] = nullptr;
  sendToMiner(resp.dump() + "\n");
}

Job* Session::findJobLocked(const std::string& jobId) {
  auto it = jobs_.find(jobId);
  return (it == jobs_.end()) ? nullptr : &it->second;
}

void Session::pruneJobsLocked() {
  const int64_t now = util::nowMs();
  const int64_t maxAge = static_cast<int64_t>(cfg_.jobMaxAgeSec) * 1000;
  for (auto it = jobs_.begin(); it != jobs_.end();) {
    if (now - it->second.receivedMs > maxAge) {
      it = jobs_.erase(it);
    } else {
      ++it;
    }
  }
  while (static_cast<int>(jobs_.size()) > std::max(1, cfg_.maxJobsPerSession)) {
    auto oldest = jobs_.begin();
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
      if (it->second.receivedMs < oldest->second.receivedMs) oldest = it;
    }
    jobs_.erase(oldest);
  }
}

void Session::onMinerLine(const std::string& raw) {
  json msg;
  try {
    msg = json::parse(raw);
  } catch (const std::exception& e) {
    LOG().warn("[%s] miner sent unparsable JSON (%s), passing it through: %.180s", peer_.c_str(),
               e.what(), raw.c_str());
    sendToUpstream(raw + "\n");
    return;
  }
  if (!msg.is_object()) {
    sendToUpstream(raw + "\n");
    return;
  }

  const std::string method = msg.value("method", std::string());
  if (method == "submit") {
    if (handleSubmit(msg, raw)) return;  // forwarded
    return;                             // blocked locally
  }
  if (method == "login") {
    loginRequestId_ = idKey(msg);
    if (msg.contains("params") && msg["params"].is_object()) {
      login_ = msg["params"].value("login", std::string());
      info_->login = login_;
    }
    LOG().info("[%s] login as %s", peer_.c_str(), login_.c_str());
  }
  sendToUpstream(raw + "\n");
}

bool Session::handleSubmit(const json& msg, const std::string& raw) {
  auto& counters = px_.stats()->counters;
  counters.sharesSeen++;
  info_->sharesSeen++;
  info_->lastShareMs = util::nowMs();

  std::string sessionId, jobId, nonce, resultHex;
  if (msg.contains("params") && msg["params"].is_object()) {
    const json& p = msg["params"];
    auto itSid = p.find("id");
    if (itSid != p.end()) {
      if (itSid->is_string()) {
        sessionId = itSid->get<std::string>();
      } else if (itSid->is_number()) {
        sessionId = itSid->dump();
      }
    }
    if (p.contains("job_id") && p["job_id"].is_string()) jobId = p["job_id"].get<std::string>();
    if (p.contains("nonce") && p["nonce"].is_string()) nonce = util::toLower(p["nonce"].get<std::string>());
    if (p.contains("result") && p["result"].is_string())
      resultHex = util::toLower(p["result"].get<std::string>());
  }

  auto block = [&](std::atomic<uint64_t>& counter, const char* wireReason,
                   const std::string& note) {
    counter++;
    info_->sharesBlocked++;
    noteBlock(note);
    rejectShare(msg, wireReason);
    LOG().warn("[%s] BLOCKED share job=%s nonce=%s: %s", peer_.c_str(),
               jobId.empty() ? "?" : jobId.c_str(), nonce.empty() ? "?" : nonce.c_str(),
               note.c_str());
    return false;
  };

  if (!sessionEstablished_ || sessionId.empty() || sessionId != sessionId_) {
    return block(counters.blockNoSession, "Unauthenticated",
                 "share arrived with a session id that is not the one the pool assigned");
  }
  if (jobId.empty() || !util::isHex(nonce, 8) || !util::isHex(resultHex, 64)) {
    return block(counters.blockMalformed, "Invalid submit params",
                 "malformed submit (job_id/nonce/result)");
  }

  Job jobCopy;
  {
    std::lock_guard<std::mutex> lock(jobMu_);
    Job* job = findJobLocked(jobId);
    if (!job) {
      if (cfg_.checkJobId) {
        return block(counters.blockUnknownJob, "Invalid job id",
                     "job_id " + jobId + " is not a job we handed this miner (stale/unknown)");
      }
    } else {
      if (cfg_.checkDuplicate && job->seenNonces.count(nonce)) {
        return block(counters.blockDuplicate, "Duplicate share",
                     "duplicate nonce for job " + jobId);
      }
      jobCopy = *job;
    }
  }

  uint8_t resultBytes[32];
  if (!util::hexDecodeInto(resultHex, resultBytes, 32)) {
    return block(counters.blockMalformed, "Invalid submit params", "result is not hex");
  }

  const uint64_t hashTarget = util::target64FromHash(resultBytes);
  const uint64_t shareDiff = util::difficultyFromTarget64(hashTarget);

  // Cheap check first: a share that cannot meet the target is never forwarded.
  if (cfg_.checkTarget) {
    if (jobCopy.target64 == 0) {
      return block(counters.blockUnknownJob, "Invalid job id", "job has no usable target");
    }
    if (hashTarget > jobCopy.target64) {
      char note[192];
      std::snprintf(note, sizeof(note),
                    "low difficulty share (share diff %llu < job diff %llu)",
                    static_cast<unsigned long long>(shareDiff),
                    static_cast<unsigned long long>(util::difficultyFromTarget64(jobCopy.target64)));
      return block(counters.blockLowDiff, "Low difficulty share", note);
    }
  }

  double verifyMs = 0;
  if (cfg_.checkHash && jobCopy.blob.size() == kBlobBytes) {
    uint8_t input[kBlobBytes];
    std::memcpy(input, jobCopy.blob.data(), kBlobBytes);
    uint8_t nonceBytes[4];
    util::hexDecodeInto(nonce, nonceBytes, 4);
    std::memcpy(input + kNonceOffset, nonceBytes, 4);

    uint8_t computed[32];
    std::string err;
    const double t0 = util::nowSteady();
    const bool haveHash = px_.engine()->verify(input, sizeof(input), jobCopy.seedHash,
                                               static_cast<uint32_t>(cfg_.verifyTimeoutMs), computed,
                                               &err);
    verifyMs = (util::nowSteady() - t0) * 1000.0;
    info_->lastVerifyUs = static_cast<uint64_t>(verifyMs * 1000.0);

    if (!haveHash) {
      // Fail closed: we cannot prove this share is good, so the pool must not see it.
      return block(counters.blockVerifyUnavailable, "Verification unavailable",
                   "could not verify: " + err);
    }
    if (std::memcmp(computed, resultBytes, 32) != 0) {
      return block(counters.blockInvalidHash, "Invalid result",
                   "hash mismatch (miner hashed the wrong blob/nonce)");
    }
  }

  {
    std::lock_guard<std::mutex> lock(jobMu_);
    Job* job = findJobLocked(jobId);
    if (job) {
      job->seenNonces.insert(nonce);
      job->nonceOrder.push_back(nonce);
      while (job->nonceOrder.size() > kMaxNoncesPerJob) {
        job->seenNonces.erase(job->nonceOrder.front());
        job->nonceOrder.pop_front();
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(pendingMu_);
    pendingSubmits_[idKey(msg)] = jobId;
  }
  counters.sharesForwarded++;
  info_->sharesForwarded++;
  LOG().info("[%s] share OK job=%s nonce=%s diff=%llu verified in %.2f ms -> forwarding",
             peer_.c_str(), jobId.c_str(), nonce.c_str(),
             static_cast<unsigned long long>(shareDiff), verifyMs);
  sendToUpstream(raw + "\n");
  return true;
}

bool Session::registerJob(const json& jobParams, bool fromLogin) {
  Job job;
  std::string reason;
  if (!parseJobJson(jobParams, cfg_, &job, &reason)) {
    px_.stats()->counters.jobsBlocked++;
    info_->jobsBlocked++;
    noteBlock("job dropped: " + reason);
    if (cfg_.blockInvalidJobs) {
      LOG().error("[%s] POOL JOB REJECTED (%s) -- not forwarded to the miner", peer_.c_str(),
                  reason.c_str());
      if (fromLogin) {
        LOG().error("[%s] login response carried that job; withholding the whole login reply",
                    peer_.c_str());
      }
      return false;
    }
    LOG().warn("[%s] pool job looks wrong (%s) but blockInvalidJobs=false, forwarding anyway",
               peer_.c_str(), reason.c_str());
  }

  {
    std::lock_guard<std::mutex> lock(jobMu_);
    jobs_[job.jobId] = job;
    pruneJobsLocked();
  }
  info_->height = job.height;
  px_.stats()->counters.jobsRelayed++;
  info_->jobsRelayed++;

  if (px_.engine()) px_.engine()->noteJobSeeds(job.seedHash, job.nextSeedHash);

  LOG().info("[%s] job %s height=%lld diff=%llu%s%s", peer_.c_str(), job.jobId.c_str(),
             static_cast<long long>(job.height),
             static_cast<unsigned long long>(util::difficultyFromTarget64(job.target64)),
             job.nextSeedHash.empty() ? "" : " (next seed announced)",
             fromLogin ? " [from login]" : "");
  return true;
}

void Session::onUpstreamLine(const std::string& raw) {
  auto& counters = px_.stats()->counters;
  json msg;
  try {
    msg = json::parse(raw);
  } catch (const std::exception&) {
    sendToMiner(raw + "\n");
    return;
  }
  if (!msg.is_object()) {
    sendToMiner(raw + "\n");
    return;
  }

  // Notifications: job pushes are the interesting ones.
  if (msg.contains("method") && msg["method"].is_string()) {
    const std::string method = msg["method"].get<std::string>();
    if (method == "job" && msg.contains("params")) {
      if (registerJob(msg["params"], false)) sendToMiner(raw + "\n");
      return;
    }
    sendToMiner(raw + "\n");
    return;
  }

  const std::string key = idKey(msg);

  {
    std::lock_guard<std::mutex> lock(pendingMu_);
    if (!key.empty() && internalIds_.erase(key) > 0) {
      return;  // reply to one of our own keepalives
    }
  }

  if (!loginRequestId_.empty() && key == loginRequestId_) {
    bool ok = false;
    bool jobOk = true;
    if (msg.contains("result") && msg["result"].is_object()) {
      const json& result = msg["result"];
      auto itSid = result.find("id");
      if (itSid != result.end()) {
        if (itSid->is_string()) sessionId_ = itSid->get<std::string>();
        else if (itSid->is_number()) sessionId_ = itSid->dump();
      }
      if (!sessionId_.empty()) {
        sessionEstablished_ = true;
        ok = true;
      }
      if (result.contains("job")) {
        jobOk = registerJob(result["job"], true);
      }
    }
    if (ok) {
      counters.loginOk++;
      LOG().info("[%s] login accepted, session id %s", peer_.c_str(), sessionId_.c_str());
      if (jobOk) {
        sendToMiner(raw + "\n");
      } else {
        // Without a usable first job the miner would sit idle forever waiting for
        // credentials it never received. Dropping the connection makes it retry.
        LOG().error("[%s] closing miner connection: the pool's first job was unusable",
                    peer_.c_str());
        miner_.shutdownBoth();
      }
    } else {
      counters.loginError++;
      LOG().warn("[%s] login rejected by pool: %s", peer_.c_str(), raw.c_str());
      sendToMiner(raw + "\n");
    }
    return;
  }

  {
    bool known = false;
    std::string jobId;
    {
      std::lock_guard<std::mutex> lock(pendingMu_);
      auto it = pendingSubmits_.find(key);
      if (it != pendingSubmits_.end()) {
        known = true;
        jobId = it->second;
        pendingSubmits_.erase(it);
      }
    }
    if (known) {
      bool accepted = false;
      if (msg.contains("result") && msg["result"].is_object()) {
        accepted = msg["result"].value("status", std::string()) == "OK";
      }
      if (msg.contains("error") && !msg["error"].is_null()) accepted = false;
      if (accepted) {
        counters.poolAccepted++;
        info_->poolAccepted++;
        LOG().info("[%s] pool accepted share job=%s", peer_.c_str(), jobId.c_str());
      } else {
        counters.poolRejected++;
        info_->poolRejected++;
        LOG().warn("[%s] pool REJECTED share job=%s: %s", peer_.c_str(), jobId.c_str(), raw.c_str());
      }
      sendToMiner(raw + "\n");
      return;
    }
  }

  sendToMiner(raw + "\n");
}

// ---------------------------------------------------------------------------
// Proxy
// ---------------------------------------------------------------------------

Proxy::Proxy(const Config& cfg, RandomxEngine* rx, StatsRegistry* stats)
    : cfg_(cfg), rx_(rx), stats_(stats) {}

Proxy::~Proxy() { stop(); }

bool Proxy::start(std::string* err) {
  Endpoint ep;
  if (!parseEndpoint(cfg_.listen, &ep)) {
    if (err) *err = "bad listen address: " + cfg_.listen;
    return false;
  }
  if (!listener_.listenOn(ep.host, ep.port, err)) return false;
  LOG().info("proxy listening on %s", ep.str().c_str());
  return true;
}

void Proxy::run() {
  while (!stopping_) {
    std::string peer;
    Socket sock = listener_.acceptOne(&peer);
    if (!sock.valid()) {
      if (stopping_) break;
      continue;
    }
    LOG().info("miner connected from %s", peer.c_str());
    Socket miner(std::move(sock));
    // Session lifetime is owned by its thread; Proxy only tracks raw sockets.
    Session* raw = new Session(*this, std::move(miner), peer);
    std::thread([raw] {
      raw->run();
      delete raw;
    }).detach();
  }
}

void Proxy::stop() {
  if (stopping_.exchange(true)) return;
  listener_.closeSocket();
  std::lock_guard<std::mutex> lock(socksMu_);
  for (SOCKET s : liveSockets_) {
    if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
  }
}

void Proxy::trackSocket(SOCKET s) {
  if (s == INVALID_SOCKET) return;
  std::lock_guard<std::mutex> lock(socksMu_);
  liveSockets_.push_back(s);
}

void Proxy::untrackSocket(SOCKET s) {
  if (s == INVALID_SOCKET) return;
  std::lock_guard<std::mutex> lock(socksMu_);
  liveSockets_.erase(std::remove(liveSockets_.begin(), liveSockets_.end(), s), liveSockets_.end());
}
