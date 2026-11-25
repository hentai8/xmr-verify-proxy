#pragma once

#include <string>
#include <vector>

struct Config {
  // --- network ---
  std::string listen = "0.0.0.0:3333";
  std::string upstream = "xmr.kryptex.network:7029";
  int connectTimeoutMs = 15000;

  // --- RandomX ---
  bool fastMode = true;              // FULL_MEM dataset (~2.1 GB); light mode = cache only
  bool prebuildNextSeed = true;      // build dataset for next_seed_hash ahead of the switch
  bool hugePages = false;            // needs SeLockMemoryPrivilege (run as admin)
  int datasetInitThreads = 0;        // 0 = hardware_concurrency / 2 (min 1)
  int vmPool = 4;                    // max concurrent verification VMs per seed
  int maxCachedSeeds = 3;            // how many seeds to keep caches for
  int verifyTimeoutMs = 20000;       // max wait for cache/dataset readiness

  // --- verification rules ---
  bool checkHash = true;             // recompute RandomX and compare with the submitted result
  bool checkTarget = true;           // share must meet the job target
  bool checkDuplicate = true;        // same job_id + nonce twice = blocked
  bool checkJobId = true;            // job_id must be one we actually handed out
  bool blockInvalidJobs = true;      // drop malformed jobs instead of forwarding them
  std::vector<std::string> allowedAlgos = {"rx/0"};

  // --- behaviour ---
  int keepaliveSec = 60;             // 0 = never inject our own keepalived
  int maxJobsPerSession = 64;
  int jobMaxAgeSec = 1800;

  // --- stats page ---
  bool statsEnabled = true;
  std::string statsListen = "127.0.0.1:3356";

  // --- logging ---
  std::string logLevel = "info";
  std::string logFile = "logs/proxy.log";
};

// Loads config from a JSON file. Missing file is not an error (defaults are used).
bool loadConfig(const std::string& path, Config* out, std::string* err, bool* loaded);

