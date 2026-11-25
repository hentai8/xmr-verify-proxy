#include "net.h"

#include <cstring>
#include <vector>

namespace {
constexpr int kRecvBufSize = 64 * 1024;
}

bool parseEndpoint(const std::string& s, Endpoint* out) {
  const size_t colon = s.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) return false;
  out->host = s.substr(0, colon);
  const std::string portStr = s.substr(colon + 1);
  char* end = nullptr;
  long port = std::strtol(portStr.c_str(), &end, 10);
  if (end == portStr.c_str() || port <= 0 || port > 65535) return false;
  out->port = static_cast<int>(port);
  return true;
}

bool netInit(std::string* err) {
  WSADATA wsa;
  const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
  if (rc != 0) {
    if (err) *err = "WSAStartup failed: " + std::to_string(rc);
    return false;
  }
  return true;
}

void netCleanup() { WSACleanup(); }

Socket::~Socket() { closeSocket(); }

void Socket::closeSocket() {
  if (sock_ != INVALID_SOCKET) {
    closesocket(sock_);
    sock_ = INVALID_SOCKET;
  }
}

void Socket::shutdownBoth() {
  if (sock_ != INVALID_SOCKET) shutdown(sock_, SD_BOTH);
}

void Socket::setNoDelay(bool on) {
  if (sock_ == INVALID_SOCKET) return;
  BOOL flag = on ? TRUE : FALSE;
  setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
}

void Socket::setRecvTimeout(int ms) {
  if (sock_ == INVALID_SOCKET) return;
  DWORD tv = static_cast<DWORD>(ms);
  setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
}

void Socket::setSendTimeout(int ms) {
  if (sock_ == INVALID_SOCKET) return;
  DWORD tv = static_cast<DWORD>(ms);
  setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
}

bool Socket::connectTo(const Endpoint& ep, int timeoutMs, std::string* err) {
  addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* res = nullptr;
  const std::string portStr = std::to_string(ep.port);
  const int rc = getaddrinfo(ep.host.c_str(), portStr.c_str(), &hints, &res);
  if (rc != 0 || res == nullptr) {
    if (err) {
      *err = "resolve " + ep.host + " failed (" + std::to_string(rc) + "): " + gai_strerrorA(rc);
    }
    if (res) freeaddrinfo(res);
    return false;
  }

  std::string lastErr = "no address";
  for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == INVALID_SOCKET) {
      lastErr = "socket() failed: " + std::to_string(WSAGetLastError());
      continue;
    }

    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);

    const int crc = connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
    if (crc == SOCKET_ERROR) {
      const int wsaErr = WSAGetLastError();
      if (wsaErr != WSAEWOULDBLOCK && wsaErr != WSAEINPROGRESS) {
        lastErr = "connect failed: " + std::to_string(wsaErr);
        closesocket(s);
        continue;
      }
      fd_set writeSet;
      FD_ZERO(&writeSet);
      FD_SET(s, &writeSet);
      timeval tv;
      tv.tv_sec = timeoutMs / 1000;
      tv.tv_usec = (timeoutMs % 1000) * 1000;
      const int sel = select(0, nullptr, &writeSet, nullptr, &tv);
      if (sel <= 0) {
        lastErr = (sel == 0) ? "connect timeout" : ("select failed: " + std::to_string(WSAGetLastError()));
        closesocket(s);
        continue;
      }
      int soErr = 0;
      int optLen = sizeof(soErr);
      getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &optLen);
      if (soErr != 0) {
        lastErr = "connect failed: " + std::to_string(soErr);
        closesocket(s);
        continue;
      }
    }

    nonBlocking = 0;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    sock_ = s;
    setNoDelay(true);
    freeaddrinfo(res);
    return true;
  }

  freeaddrinfo(res);
  if (err) *err = lastErr;
  return false;
}

bool Socket::listenOn(const std::string& host, int port, std::string* err) {
  addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;

  addrinfo* res = nullptr;
  const std::string portStr = std::to_string(port);
  const char* node = (host.empty() || host == "0.0.0.0") ? nullptr : host.c_str();
  int rc = getaddrinfo(node, portStr.c_str(), &hints, &res);
  if (rc != 0 || res == nullptr) {
    if (err) *err = "bind resolve failed: " + std::to_string(rc);
    if (res) freeaddrinfo(res);
    return false;
  }

  SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (s == INVALID_SOCKET) {
    if (err) *err = "socket() failed: " + std::to_string(WSAGetLastError());
    freeaddrinfo(res);
    return false;
  }
  BOOL reuse = TRUE;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  if (bind(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) == SOCKET_ERROR) {
    if (err) *err = "bind " + host + ":" + portStr + " failed: " + std::to_string(WSAGetLastError());
    closesocket(s);
    freeaddrinfo(res);
    return false;
  }
  freeaddrinfo(res);

  if (listen(s, SOMAXCONN) == SOCKET_ERROR) {
    if (err) *err = "listen failed: " + std::to_string(WSAGetLastError());
    closesocket(s);
    return false;
  }
  sock_ = s;
  return true;
}

Socket Socket::acceptOne(std::string* peerOut) {
  sockaddr_storage addr;
  int addrLen = sizeof(addr);
  SOCKET s = accept(sock_, reinterpret_cast<sockaddr*>(&addr), &addrLen);
  if (s == INVALID_SOCKET) return Socket();
  Socket out(s);
  out.setNoDelay(true);
  if (peerOut) {
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];
    if (getnameinfo(reinterpret_cast<sockaddr*>(&addr), addrLen, host, sizeof(host), serv,
                    sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
      *peerOut = std::string(host) + ":" + serv;
    } else {
      *peerOut = "unknown";
    }
  }
  return out;
}

int Socket::sendAll(const char* data, size_t len) {
  if (sock_ == INVALID_SOCKET) return -1;
  size_t sent = 0;
  while (sent < len) {
    const int n = send(sock_, data + sent, static_cast<int>(len - sent), 0);
    if (n == SOCKET_ERROR) return -1;
    if (n == 0) return -1;
    sent += static_cast<size_t>(n);
  }
  return static_cast<int>(sent);
}

int Socket::recvSome(char* buf, size_t cap) {
  if (sock_ == INVALID_SOCKET) return -1;
  const int n = recv(sock_, buf, static_cast<int>(cap), 0);
  return n;
}

std::string Socket::peerAddress() const {
  if (sock_ == INVALID_SOCKET) return "closed";
  sockaddr_storage addr;
  int len = sizeof(addr);
  if (getpeername(sock_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return "unknown";
  char host[NI_MAXHOST];
  char serv[NI_MAXSERV];
  if (getnameinfo(reinterpret_cast<sockaddr*>(&addr), len, host, sizeof(host), serv, sizeof(serv),
                  NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    return "unknown";
  }
  return std::string(host) + ":" + serv;
}

LineReader::Result LineReader::next(Socket& sock, std::string* line, std::string* err) {
  char chunk[kRecvBufSize];
  for (;;) {
    const size_t nl = buf_.find('\n');
    if (nl != std::string::npos) {
      std::string out = buf_.substr(0, nl);
      buf_.erase(0, nl + 1);
      if (!out.empty() && out.back() == '\r') out.pop_back();
      *line = std::move(out);
      return Result::Line;
    }
    if (buf_.size() > maxLine_) {
      if (err) *err = "line too long (" + std::to_string(buf_.size()) + " bytes)";
      return Result::Error;
    }
    const int n = sock.recvSome(chunk, sizeof(chunk));
    if (n == 0) return Result::Closed;
    if (n < 0) {
      const int e = WSAGetLastError();
      // SO_RCVTIMEO expiry: keep whatever partial data we have and let the
      // caller decide whether to retry (the proxy uses this for keepalives).
      if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) return Result::Timeout;
      if (err) *err = "recv failed: " + std::to_string(e);
      return Result::Error;
    }
    buf_.append(chunk, static_cast<size_t>(n));
  }
}
