#pragma once

#include <string>

struct MockPoolOptions {
  std::string listen = "127.0.0.1:17029";
  // valid      : login job + one valid job push
  // bad-blob   : pushes a job whose blob is the wrong length
  // bad-seed   : pushes a job with no seed_hash
  // bad-algo   : pushes a job for an algorithm we cannot verify
  // bad-target : pushes a job with a zero target
  // all        : every broken variant, then a valid job (default)
  std::string mode = "all";
  // Use a very easy job target (difficulty 3) so a test miner finds a share
  // almost immediately instead of grinding for minutes.
  bool easyTarget = false;
};

int runMockPool(const MockPoolOptions& opt);
