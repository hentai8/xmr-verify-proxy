#include "util.h"

#include <chrono>
#include <cstring>

namespace util {

namespace {
inline int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace

bool isHex(const std::string& s, size_t expectedLen) {
  if (s.size() != expectedLen) return false;
  for (char c : s) {
    if (hexVal(c) < 0) return false;
  }
  return true;
}

bool hexDecodeInto(const std::string& s, uint8_t* out, size_t outLen) {
  if (s.size() != outLen * 2) return false;
  for (size_t i = 0; i < outLen; ++i) {
    int hi = hexVal(s[i * 2]);
    int lo = hexVal(s[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

std::vector<uint8_t> hexDecode(const std::string& s) {
  std::vector<uint8_t> out;
  if (s.size() % 2 != 0) return out;
  out.resize(s.size() / 2);
  if (!hexDecodeInto(s, out.data(), out.size())) out.clear();
  return out;
}

std::string hexEncode(const uint8_t* p, size_t n) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.resize(n * 2);
  for (size_t i = 0; i < n; ++i) {
    out[i * 2] = kHex[p[i] >> 4];
    out[i * 2 + 1] = kHex[p[i] & 0x0F];
  }
  return out;
}

std::string hexEncode(const std::vector<uint8_t>& v) {
  return hexEncode(v.data(), v.size());
}

uint64_t target64FromCompact(uint32_t compact) {
  if (compact == 0) return 0;
  // XMRig: target64 = 0xFFFFFFFFFFFFFFFF / (0xFFFFFFFF / t)
  uint64_t d = 0xFFFFFFFFull / compact;  // integer division on 32-bit value
  if (d == 0) d = 1;
  return 0xFFFFFFFFFFFFFFFFull / d;
}

uint32_t compactFromHex(const std::string& hex) {
  if (!isHex(hex, 8)) return 0;
  uint8_t b[4];
  hexDecodeInto(hex, b, 4);
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

uint64_t parseU64LE(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) {
    v = (v << 8) | p[i];
  }
  return v;
}

uint64_t target64FromHash(const uint8_t hash[32]) {
  return parseU64LE(hash + 24);
}

uint64_t difficultyFromTarget64(uint64_t target64) {
  if (target64 == 0) return 0xFFFFFFFFFFFFFFFFull;
  return 0xFFFFFFFFFFFFFFFFull / target64;
}

int64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

int64_t nowSec() { return nowMs() / 1000; }

double nowSteady() {
  using namespace std::chrono;
  return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

int64_t parseI64(const std::string& s, int64_t def) {
  if (s.empty()) return def;
  char* end = nullptr;
  long long v = std::strtoll(s.c_str(), &end, 10);
  if (end == s.c_str()) return def;
  return v;
}

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && static_cast<unsigned char>(s[b]) <= ' ') ++b;
  while (e > b && static_cast<unsigned char>(s[e - 1]) <= ' ') --e;
  return s.substr(b, e - b);
}

std::string toLower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

}  // namespace util
