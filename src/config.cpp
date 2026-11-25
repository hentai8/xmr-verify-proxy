#include "config.h"

#include <algorithm>
#include <fstream>
#include <thread>

#include "json.hpp"
#include "log.h"

using nlohmann::json;

namespace {

template <typename T>
void pick(const json& j, const char* key, T* out) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return;
  try {
    *out = it->get<T>();
  } catch (const std::exception& e) {
    LOG().warn("config: bad value for '%s': %s", key, e.what());
  }
}

void pickAlgos(const json& j, const char* key, std::vector<std::string>* out) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return;
  if (!it->is_array()) {
    LOG().warn("config: '%s' must be an array of algorithm names", key);
    return;
  }
  out->clear();
  for (const auto& a : *it) {
    if (a.is_string()) out->push_back(a.get<std::string>());
  }
}

}  // namespace


bool loadConfig(const std::string& path, Config* out, std::string* err, bool* loaded) {
  if (loaded) *loaded = false;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return true;  // defaults are fine
  }

  json j;
  try {
    in >> j;
  } catch (const std::exception& e) {
    if (err) *err = std::string("config parse error: ") + e.what();
    return false;
  }
  if (!j.is_object()) {
    if (err) *err = "config root must be a JSON object";
    return false;
  }
  if (loaded) *loaded = true;

  pick(j, "listen", &out->listen);
  pick(j, "upstream", &out->upstream);
  pick(j, "connectTimeoutMs", &out->connectTimeoutMs);



  if (j.contains("randomx") && j["randomx"].is_object()) {
    const json& r = j["randomx"];
    pick(r, "fastMode", &out->fastMode);
    pick(r, "prebuildNextSeed", &out->prebuildNextSeed);
    pick(r, "hugePages", &out->hugePages);
    pick(r, "datasetInitThreads", &out->datasetInitThreads);
    pick(r, "vmPool", &out->vmPool);
    pick(r, "maxCachedSeeds", &out->maxCachedSeeds);
  }

  if (j.contains("verify") && j["verify"].is_object()) {
    const json& v = j["verify"];
    pick(v, "checkHash", &out->checkHash);
    pick(v, "checkTarget", &out->checkTarget);
    pick(v, "checkDuplicate", &out->checkDuplicate);
    pick(v, "checkJobId", &out->checkJobId);
    pick(v, "blockInvalidJobs", &out->blockInvalidJobs);
    pick(v, "timeoutMs", &out->verifyTimeoutMs);
    pickAlgos(v, "allowedAlgos", &out->allowedAlgos);
  }

  pick(j, "keepaliveSec", &out->keepaliveSec);
  pick(j, "maxJobsPerSession", &out->maxJobsPerSession);
  pick(j, "jobMaxAgeSec", &out->jobMaxAgeSec);

  if (j.contains("stats") && j["stats"].is_object()) {
    const json& s = j["stats"];
    pick(s, "enabled", &out->statsEnabled);
    pick(s, "listen", &out->statsListen);
  }

  if (j.contains("log") && j["log"].is_object()) {
    const json& l = j["log"];
    pick(l, "level", &out->logLevel);
    pick(l, "file", &out->logFile);
  }

  if (out->datasetInitThreads <= 0) {
    const unsigned hw = std::thread::hardware_concurrency();
    out->datasetInitThreads = static_cast<int>(hw / 2 > 0 ? hw / 2 : 1);
  }
  out->vmPool = std::max(1, out->vmPool);
  out->maxCachedSeeds = std::max(1, out->maxCachedSeeds);
  out->verifyTimeoutMs = std::max(1000, out->verifyTimeoutMs);
  return true;
}
