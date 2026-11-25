#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstddef>
#include <string>

struct Endpoint {
  std::string host;
  int port = 0;
  std::string str() const { return host + ":" + std::to_string(port); }
};

bool parseEndpoint(const std::string& s, Endpoint* out);
bool netInit(std::string* err);
void netCleanup();

class Socket {
 public:
  Socket() = default;
  explicit Socket(SOCKET s) : sock_(s) {}
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept : sock_(other.sock_) { other.sock_ = INVALID_SOCKET; }
  Socket& operator=(Socket&& other) noexcept {
    if (this != &other) {
      closeSocket();
      sock_ = other.sock_;
      other.sock_ = INVALID_SOCKET;
    }
    return *this;
  }
  ~Socket();

  bool valid() const { return sock_ != INVALID_SOCKET; }
  SOCKET raw() const { return sock_; }

  bool connectTo(const Endpoint& ep, int timeoutMs, std::string* err);
  bool listenOn(const std::string& host, int port, std::string* err);
  Socket acceptOne(std::string* peerOut);
  void setNoDelay(bool on);
  void setRecvTimeout(int ms);
  void setSendTimeout(int ms);
  void closeSocket();
  void shutdownBoth();

  // Returns bytes written (>0), or -1 on error.
  int sendAll(const char* data, size_t len);
  // Returns >0 bytes read, 0 on orderly close, -1 on error.
  int recvSome(char* buf, size_t cap);

  std::string peerAddress() const;

 private:
  SOCKET sock_ = INVALID_SOCKET;
};

// Splits a byte stream into '\n'-terminated lines.
class LineReader {
 public:
  explicit LineReader(size_t maxLine = 1u << 20) : maxLine_(maxLine) {}

  enum class Result { Line = 0, Closed = 1, Timeout = 2, Error = 3 };

  Result next(Socket& sock, std::string* line, std::string* err);

 private:
  std::string buf_;
  size_t maxLine_;
};
