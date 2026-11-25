#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "net.h"
#include "randomx_engine.h"
#include "stats.h"

class Proxy {
 public:
  Proxy(const Config& cfg, RandomxEngine* rx, StatsRegistry* stats);
  ~Proxy();

  bool start(std::string* err);
  void run();   // accept loop; blocks until stop()
  void stop();

  const Config& cfg() const { return cfg_; }
  RandomxEngine* engine() { return rx_; }
  StatsRegistry* stats() { return stats_; }

  // Live socket registry so stop() can unblock every session thread.
  void trackSocket(SOCKET s);
  void untrackSocket(SOCKET s);

 private:
  Config cfg_;
  RandomxEngine* rx_;
  StatsRegistry* stats_;
  Socket listener_;
  std::atomic<bool> stopping_{false};

  std::mutex socksMu_;
  std::vector<SOCKET> liveSockets_;
};
