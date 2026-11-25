#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "net.h"
#include "randomx_engine.h"

struct SessionInfo {
  int id = 0;
  std::string peer;
  std::string upstream;
  std::string login;
  int64_t startedMs = 0;

  std::atomic<uint64_t> jobsRelayed{0};
  std::atomic<uint64_t> jobsBlocked{0};
  std::atomic<uint64_t> sharesSeen{0};
  std::atomic<uint64_t> sharesForwarded{0};
  std::atomic<uint64_t> sharesBlocked{0};
  std::atomic<uint64_t> poolAccepted{0};
  std::atomic<uint64_t> poolRejected{0};
  std::atomic<int64_t> lastShareMs{0};
  std::atomic<int64_t> height{0};
  std::atomic<uint64_t> lastVerifyUs{0};

  std::mutex noteMu;
  std::string lastBlockReason;
};

struct ProxyCounters {
  std::atomic<uint64_t> connectionsTotal{0};
  std::atomic<uint64_t> connectionsActive{0};
  std::atomic<uint64_t> loginOk{0};
  std::atomic<uint64_t> loginError{0};
  std::atomic<uint64_t> jobsRelayed{0};
  std::atomic<uint64_t> jobsBlocked{0};
  std::atomic<uint64_t> sharesSeen{0};
  std::atomic<uint64_t> sharesForwarded{0};
  std::atomic<uint64_t> poolAccepted{0};
  std::atomic<uint64_t> poolRejected{0};
  std::atomic<uint64_t> upstreamConnects{0};
  std::atomic<uint64_t> upstreamFailures{0};

  std::atomic<uint64_t> blockInvalidHash{0};
  std::atomic<uint64_t> blockLowDiff{0};
  std::atomic<uint64_t> blockUnknownJob{0};
  std::atomic<uint64_t> blockDuplicate{0};
  std::atomic<uint64_t> blockMalformed{0};
  std::atomic<uint64_t> blockNoSession{0};
  std::atomic<uint64_t> blockVerifyUnavailable{0};
  std::atomic<uint64_t> blockAlgo{0};
};

class StatsRegistry {
 public:
  std::shared_ptr<SessionInfo> addSession(const std::string& peer, const std::string& upstream);
  void removeSession(int id);
  std::vector<std::shared_ptr<SessionInfo>> sessions();

  ProxyCounters counters;
  int64_t startedMs = 0;

 private:
  std::mutex mu_;
  std::vector<std::shared_ptr<SessionInfo>> sessions_;
  int nextId_ = 1;
};

class StatsServer {
 public:
  StatsServer(StatsRegistry* reg, RandomxEngine* rx, const Config* cfg)
      : reg_(reg), rx_(rx), cfg_(cfg) {}
  ~StatsServer() { stop(); }

  bool start(std::string* err);
  void stop();

 private:
  void loop();
  std::string renderJson();
  std::string renderHtml();
  void handleClient(Socket sock);

  StatsRegistry* reg_;
  RandomxEngine* rx_;
  const Config* cfg_;
  Socket listener_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  Endpoint ep_;
};
