#pragma once

#include <string>

struct MinerOptions {
  std::string target = "127.0.0.1:3333";
  std::string login;                 // "<address>/<worker>"
  std::string pass = "x";
  std::string agent = "xmr-verify-proxy-testminer/1.0";
  // none       : mine a real share and submit it (happy path)
  // bad-hash   : submit a result that meets the target but is not the real hash
  // low-diff   : submit a genuine hash that does not meet the job target
  // unknown-job: submit against a job_id the pool never issued
  // dup        : mine a real share, submit it twice
  // all        : bad-hash, low-diff, unknown-job, then a real share, then dup
  std::string fault = "none";
  int threads = 4;
  int mineTimeoutSec = 900;
  bool lightMode = false;
};

int runTestMiner(const MinerOptions& opt);
