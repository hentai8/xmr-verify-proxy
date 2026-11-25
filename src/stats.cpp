#include "stats.h"

#include <algorithm>
#include <cstdio>

#include "json.hpp"
#include "log.h"
#include "util.h"

using nlohmann::json;

std::shared_ptr<SessionInfo> StatsRegistry::addSession(const std::string& peer,
                                                       const std::string& upstream) {
  auto s = std::make_shared<SessionInfo>();
  s->peer = peer;
  s->upstream = upstream;
  s->startedMs = util::nowMs();
  std::lock_guard<std::mutex> lock(mu_);
  s->id = nextId_++;
  sessions_.push_back(s);
  return s;
}

void StatsRegistry::removeSession(int id) {
  std::lock_guard<std::mutex> lock(mu_);
  sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                 [id](const std::shared_ptr<SessionInfo>& s) { return s->id == id; }),
                  sessions_.end());
}

std::vector<std::shared_ptr<SessionInfo>> StatsRegistry::sessions() {
  std::lock_guard<std::mutex> lock(mu_);
  return sessions_;
}

bool StatsServer::start(std::string* err) {
  if (!cfg_->statsEnabled) return true;
  if (!parseEndpoint(cfg_->statsListen, &ep_)) {
    if (err) *err = "bad stats.listen: " + cfg_->statsListen;
    return false;
  }
  if (!listener_.listenOn(ep_.host, ep_.port, err)) return false;
  thread_ = std::thread([this] { loop(); });
  LOG().info("stats page on http://%s/ (JSON at /api/stats)", ep_.str().c_str());
  return true;
}

void StatsServer::stop() {
  if (!thread_.joinable()) return;
  stopping_ = true;
  listener_.closeSocket();
  thread_.join();
}

void StatsServer::loop() {
  while (!stopping_) {
    std::string peer;
    Socket client = listener_.acceptOne(&peer);
    if (!client.valid()) {
      if (stopping_) break;
      continue;
    }
    handleClient(std::move(client));
  }
}

void StatsServer::handleClient(Socket sock) {
  // Read the request headers (we only care about the path).
  std::string request;
  char buf[2048];
  while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
    const int n = sock.recvSome(buf, sizeof(buf));
    if (n <= 0) return;
    request.append(buf, static_cast<size_t>(n));
  }

  const size_t sp1 = request.find(' ');
  const size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : request.find(' ', sp1 + 1);
  std::string path = "/";
  if (sp1 != std::string::npos && sp2 != std::string::npos) {
    path = request.substr(sp1 + 1, sp2 - sp1 - 1);
  }
  const size_t q = path.find('?');
  if (q != std::string::npos) path = path.substr(0, q);

  std::string body;
  std::string contentType = "text/html; charset=utf-8";
  if (path == "/api/stats") {
    body = renderJson();
    contentType = "application/json; charset=utf-8";
  } else if (path == "/" || path == "/index.html") {
    body = renderHtml();
  } else {
    const std::string notFound =
        "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nnot found";
    sock.sendAll(notFound.c_str(), notFound.size());
    return;
  }

  char header[512];
  const int headerLen = std::snprintf(
      header, sizeof(header),
      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
      "Cache-Control: no-store\r\n\r\n",
      contentType.c_str(), body.size());
  sock.sendAll(header, static_cast<size_t>(headerLen));
  sock.sendAll(body.data(), body.size());
}

std::string StatsServer::renderJson() {
  json out;
  const ProxyCounters& c = reg_->counters;
  out["uptimeSec"] = static_cast<double>(util::nowMs() - reg_->startedMs) / 1000.0;
  out["upstream"] = cfg_->upstream;
  out["listen"] = cfg_->listen;

  json conn;
  conn["total"] = c.connectionsTotal.load();
  conn["active"] = c.connectionsActive.load();
  conn["loginOk"] = c.loginOk.load();
  conn["loginError"] = c.loginError.load();
  conn["upstreamConnects"] = c.upstreamConnects.load();
  conn["upstreamFailures"] = c.upstreamFailures.load();
  out["connections"] = conn;

  json jobs;
  jobs["relayed"] = c.jobsRelayed.load();
  jobs["blocked"] = c.jobsBlocked.load();
  out["jobs"] = jobs;

  json shares;
  shares["seen"] = c.sharesSeen.load();
  shares["forwarded"] = c.sharesForwarded.load();
  shares["poolAccepted"] = c.poolAccepted.load();
  shares["poolRejected"] = c.poolRejected.load();
  json blocked;
  blocked["invalidHash"] = c.blockInvalidHash.load();
  blocked["lowDifficulty"] = c.blockLowDiff.load();
  blocked["unknownJob"] = c.blockUnknownJob.load();
  blocked["duplicate"] = c.blockDuplicate.load();
  blocked["malformed"] = c.blockMalformed.load();
  blocked["noSession"] = c.blockNoSession.load();
  blocked["verificationUnavailable"] = c.blockVerifyUnavailable.load();
  blocked["algo"] = c.blockAlgo.load();
  uint64_t blockedTotal = 0;
  for (auto it = blocked.begin(); it != blocked.end(); ++it) blockedTotal += it->get<uint64_t>();
  shares["blocked"] = blockedTotal;
  shares["blockedByReason"] = blocked;
  out["shares"] = shares;

  RandomxEngine* rx = rx_;
  if (rx) {
    RxSnapshot s = rx->snapshot();
    json r;
    r["mode"] = cfg_->fastMode ? "fast" : "light";
    r["currentSeed"] = s.currentSeed;
    r["nextSeed"] = s.nextSeed;
    r["cacheReady"] = s.currentCacheReady;
    r["datasetReady"] = s.currentDatasetReady;
    r["datasetInitSec"] = s.currentDatasetInitSec;
    r["lastVerifyMs"] = s.lastVerifyMs;
    r["datasetMemBytes"] = s.datasetMemBytes;
    r["cacheMemBytes"] = s.cacheMemBytes;
    r["hugePages"] = s.hugePagesActive;
    out["randomx"] = r;
  }

  json sessions = json::array();
  const int64_t now = util::nowMs();
  for (const auto& s : reg_->sessions()) {
    json j;
    j["id"] = s->id;
    j["peer"] = s->peer;
    j["login"] = s->login;
    j["uptimeSec"] = static_cast<double>(now - s->startedMs) / 1000.0;
    j["height"] = s->height.load();
    j["jobsRelayed"] = s->jobsRelayed.load();
    j["jobsBlocked"] = s->jobsBlocked.load();
    j["sharesSeen"] = s->sharesSeen.load();
    j["sharesForwarded"] = s->sharesForwarded.load();
    j["sharesBlocked"] = s->sharesBlocked.load();
    j["poolAccepted"] = s->poolAccepted.load();
    j["poolRejected"] = s->poolRejected.load();
    j["lastVerifyUs"] = s->lastVerifyUs.load();
    {
      std::lock_guard<std::mutex> lock(s->noteMu);
      j["lastBlockReason"] = s->lastBlockReason;
    }
    j["lastShareAgoSec"] = s->lastShareMs.load() > 0
                               ? static_cast<double>(now - s->lastShareMs.load()) / 1000.0
                               : -1.0;
    sessions.push_back(j);
  }
  out["sessions"] = sessions;
  return out.dump(2);
}

std::string StatsServer::renderHtml() {
  static const char* kPage = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>xmr-verify-proxy</title>
<style>
:root{color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;padding:24px;background:#0e1116;color:#d7dde5;
 font:14px/1.5 "Segoe UI",-apple-system,sans-serif}
h1{margin:0 0 4px;font-size:19px;font-weight:600;letter-spacing:.2px}
.sub{color:#7d8794;font-size:12px;margin-bottom:20px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(158px,1fr));gap:10px;margin-bottom:18px}
.card{background:#161b22;border:1px solid #232a33;border-radius:8px;padding:12px 14px}
.card .k{color:#7d8794;font-size:11px;text-transform:uppercase;letter-spacing:.7px}
.card .v{font-size:22px;font-weight:600;margin-top:4px;font-variant-numeric:tabular-nums}
.card .h{color:#5c6672;font-size:11px;margin-top:2px}
.ok{color:#4ec9a5}.warn{color:#d9a441}.bad{color:#e06c75}.mono{font-family:ui-monospace,Consolas,monospace}
h2{font-size:13px;font-weight:600;color:#8d97a5;margin:22px 0 8px;text-transform:uppercase;letter-spacing:.7px}
table{width:100%;border-collapse:collapse;font-size:13px}
th,td{text-align:left;padding:7px 10px;border-bottom:1px solid #1d242c}
th{color:#6f7a88;font-weight:500;font-size:11px;text-transform:uppercase;letter-spacing:.6px}
td.num{font-variant-numeric:tabular-nums}
td.mono{font-family:ui-monospace,Consolas,monospace;font-size:12px}
.empty{color:#5c6672;padding:14px 10px}
</style></head><body>
<h1>xmr-verify-proxy</h1>
<div class="sub" id="sub">loading</div>
<div class="grid" id="cards"></div>
<h2>Sessions</h2>
<table><thead><tr>
<th>#</th><th>Miner</th><th>Worker</th><th>Height</th><th>Jobs</th><th>Shares ok/blocked</th>
<th>Pool ok/rej</th><th>Last verify</th><th>Last block reason</th>
</tr></thead><tbody id="rows"><tr><td colspan="9" class="empty">no active sessions</td></tr></tbody></table>
<script>
function el(t,c,x){var e=document.createElement(t);if(c)e.className=c;if(x!==undefined)e.textContent=x;return e;}
function card(label,value,sub,cls){
  var d=el('div','card');d.appendChild(el('div','k',label));
  var v=el('div','v',value);if(cls)v.className='v '+cls;d.appendChild(v);
  if(sub)d.appendChild(el('div','h',sub));return d;
}
function fmt(n){return (n||0).toLocaleString('en-US');}
function dur(s){
  if(s<0)return '-';
  if(s<60)return s.toFixed(0)+'s';
  if(s<3600)return Math.floor(s/60)+'m '+Math.floor(s%60)+'s';
  return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m';
}
async function tick(){
  var d;
  try{ d=await (await fetch('/api/stats',{cache:'no-store'})).json(); }catch(e){ return; }
  document.getElementById('sub').textContent =
    d.upstream + '  |  listening ' + d.listen + '  |  uptime ' + dur(d.uptimeSec);
  var r = d.randomx || {};
  var c = document.getElementById('cards');
  c.innerHTML = '';
  c.appendChild(card('Shares seen', fmt(d.shares.seen), 'submit messages from miners'));
  c.appendChild(card('Forwarded', fmt(d.shares.forwarded), 'pool accepted ' + fmt(d.shares.poolAccepted)));
  c.appendChild(card('Blocked', fmt(d.shares.blocked), 'never reached the pool', 'bad'));
  c.appendChild(card('Pool rejected', fmt(d.shares.poolRejected), 'forwarded but refused', 'warn'));
  c.appendChild(card('Verification', r.lastVerifyMs ? (r.lastVerifyMs).toFixed(2) + ' ms' : '-',
      r.mode === 'fast' ? (r.datasetReady ? 'fast mode (dataset ready)' : 'fast mode (building)') : 'light mode'));
  c.appendChild(card('Dataset', r.datasetReady ? 'ready' : 'n/a',
      r.datasetReady ? ((r.datasetMemBytes/1073741824).toFixed(2) + ' GB, init ' + r.datasetInitSec.toFixed(1) + 's') : '',
      r.datasetReady ? 'ok' : 'warn'));
  c.appendChild(card('Seed', (r.currentSeed || '').slice(0,12), r.hugePages ? 'large pages' : 'default pages', 'mono'));
  c.appendChild(card('Miners online', fmt(d.connections.active), 'total ' + fmt(d.connections.total)));
  c.appendChild(card('Jobs blocked', fmt(d.jobs.blocked), 'malformed jobs never forwarded'));

  var tb = document.getElementById('rows');
  tb.innerHTML = '';
  if(!d.sessions.length){
    var tr = el('tr'); var td = el('td','empty','no active sessions');
    td.colSpan = 9; tr.appendChild(td); tb.appendChild(tr); return;
  }
  d.sessions.forEach(function(s){
    var tr = el('tr');
    tr.appendChild(el('td','num',s.id));
    tr.appendChild(el('td','mono',s.peer));
    tr.appendChild(el('td','mono',s.login || '-'));
    tr.appendChild(el('td','num',s.height || '-'));
    tr.appendChild(el('td','num',fmt(s.jobsRelayed)+(s.jobsBlocked?' ('+s.jobsBlocked+' dropped)':'')));
    tr.appendChild(el('td','num',fmt(s.sharesForwarded)+' / '+fmt(s.sharesBlocked)));
    tr.appendChild(el('td','num',fmt(s.poolAccepted)+' / '+fmt(s.poolRejected)));
    tr.appendChild(el('td','num',s.lastVerifyUs ? (s.lastVerifyUs/1000).toFixed(2)+' ms' : '-'));
    tr.appendChild(el('td','mono',s.lastBlockReason || '-'));
    tb.appendChild(tr);
  });
}
tick(); setInterval(tick,2000);
</script></body></html>)HTML";
  return std::string(kPage);
}
