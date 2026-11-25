#include "mockpool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "json.hpp"
#include "log.h"
#include "net.h"
#include "util.h"

using nlohmann::json;

namespace {

const char* kSeed = "39290909c4a76f2fee1df49b26d00228bcb63c1ac80535598cd71a9f5bf644c8";
// A structurally valid 76-byte blob (contents are arbitrary; nothing is mined here).
const char* kBlob =
    "1010f6a699d8066399628ebedc4b49f70776ff0c3960ea963d35dba0f23309bcddffee02926c9b"
    "0000000040be79f4ef1321d9438d357694a0f5c13ff327c1f95c2cb948f4245a4175868d06";

bool g_easyTarget = false;

json makeJob(const std::string& jobId, int64_t height) {
  json j;
  j["job_id"] = jobId;
  j["blob"] = kBlob;
  j["seed_hash"] = kSeed;
  j["next_seed_hash"] = "";
  // 0x3FFFFFFF compact target ~ difficulty 3: one in three hashes qualifies.
  j["target"] = g_easyTarget ? "ffffff3f" : "f1290000";
  j["height"] = height;
  j["algo"] = "rx/0";
  return j;
}

}  // namespace

void handleConnection(Socket sock, const std::string& peer, const std::string& mode);

int runMockPool(const MockPoolOptions& opt) {
  Endpoint ep;
  if (!parseEndpoint(opt.listen, &ep)) {
    std::printf("bad listen address: %s\n", opt.listen.c_str());
    return 2;
  }
  Socket listener;
  std::string err;
  if (!listener.listenOn(ep.host, ep.port, &err)) {
    std::printf("cannot listen on %s: %s\n", ep.str().c_str(), err.c_str());
    return 1;
  }
  LOG().setLevel(LogLevel::Info);
  g_easyTarget = opt.easyTarget;
  std::printf("== mock pool on %s (mode=%s%s) ==\n", ep.str().c_str(), opt.mode.c_str(),
              opt.easyTarget ? ", easy target" : "");

  for (;;) {
    std::string peer;
    Socket sock = listener.acceptOne(&peer);
    if (!sock.valid()) continue;
    std::printf("[%s] connected\n", peer.c_str());
    std::thread([sock = std::move(sock), peer, mode = opt.mode]() mutable {
      handleConnection(std::move(sock), peer, mode);
    }).detach();
  }
  return 0;
}

void handleConnection(Socket sock, const std::string& peer, const std::string& mode) {
    sock.setRecvTimeout(200);

    LineReader reader;
    bool loggedIn = false;
    int64_t lastMsgMs = util::nowMs();
    int step = 0;
    auto sendLine = [&](const json& msg) {
      const std::string s = msg.dump() + "\n";
      if (sock.sendAll(s.data(), s.size()) < 0) return false;
      std::printf("  -> %s\n", msg.dump().c_str());
      return true;
    };
    auto pushJob = [&](const json& job) {
      return sendLine({{"jsonrpc", "2.0"}, {"method", "job"}, {"params", job}});
    };

    while (true) {
      std::string line, rerr;
      const LineReader::Result r = reader.next(sock, &line, &rerr);
      if (r == LineReader::Result::Timeout) {
        if (!loggedIn || util::nowMs() - lastMsgMs < 1200) continue;
        lastMsgMs = util::nowMs();
        const int s = step++;

        const bool brokenMode = (mode != "valid" && mode != "all");
        if (brokenMode) {
          if (s == 0) {
            json j = makeJob("brokenjob001", 1);
            if (mode == "bad-blob") j["blob"] = std::string(kBlob).substr(0, 150);
            else if (mode == "bad-seed") j.erase("seed_hash");
            else if (mode == "bad-algo") j["algo"] = "cn/1";
            else if (mode == "bad-target") j["target"] = "00000000";
            if (!pushJob(j)) break;
          } else if (s == 1) {
            if (!pushJob(makeJob("goodjob00002", 1))) break;
          }
          continue;
        }

        if (mode == "all") {
          if (s == 0) {
            json j = makeJob("badblob00001", 1);
            j["blob"] = std::string(kBlob).substr(0, 150);  // 75 bytes, not 76
            if (!pushJob(j)) break;
          } else if (s == 1) {
            json j = makeJob("badseed00002", 1);
            j.erase("seed_hash");
            if (!pushJob(j)) break;
          } else if (s == 2) {
            json j = makeJob("badalgo00003", 1);
            j["algo"] = "cn/1";
            if (!pushJob(j)) break;
          } else if (s == 3) {
            json j = makeJob("badtarget004", 1);
            j["target"] = "00000000";
            if (!pushJob(j)) break;
          } else if (s == 4) {
            if (!pushJob(makeJob("goodjob00005", 1))) break;
          }
        } else {  // valid
          if (s == 0 && !pushJob(makeJob("goodjob00001", 1))) break;
        }
        continue;
      }
      if (r == LineReader::Result::Closed || r == LineReader::Result::Error) {
        std::printf("[%s] disconnected%s%s\n", peer.c_str(), rerr.empty() ? "" : ": ",
                    rerr.c_str());
        break;
      }
      if (line.empty()) continue;
      lastMsgMs = util::nowMs();
      std::printf("  <- %s\n", line.substr(0, 400).c_str());

      json msg;
      try {
        msg = json::parse(line);
      } catch (...) {
        continue;
      }
      const std::string method = msg.value("method", std::string());
      if (method == "login") {
        loggedIn = true;
        json resp;
        resp["id"] = msg["id"];
        resp["jsonrpc"] = "2.0";
        resp["result"] = {{"id", "1"}, {"job", makeJob("loginjob0000", 1)}, {"status", "OK"}};
        resp["error"] = nullptr;
        if (!sendLine(resp)) break;
      } else if (method == "submit") {
        const std::string jobId = msg["params"].value("job_id", std::string());
        json resp;
        resp["id"] = msg["id"];
        resp["error"] = nullptr;
        resp["result"] = {{"status", "OK"}};
        std::printf("  (would accept share for job %s)\n", jobId.c_str());
        if (!sendLine(resp)) break;
      } else if (method == "keepalived") {
        json resp;
        resp["id"] = msg["id"];
        resp["error"] = nullptr;
        resp["result"] = {{"status", "KEEPALIVED"}};
        if (!sendLine(resp)) break;
      } else {
        json resp;
        resp["id"] = msg.contains("id") ? msg["id"] : json(nullptr);
        resp["error"] = {{"code", -1}, {"message", "unknown method"}};
        resp["result"] = nullptr;
        if (!sendLine(resp)) break;
      }
    }
    sock.shutdownBoth();
}
