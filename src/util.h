#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace util {

bool isHex(const std::string& s, size_t expectedLen);
std::vector<uint8_t> hexDecode(const std::string& s);
std::string hexEncode(const uint8_t* p, size_t n);
std::string hexEncode(const std::vector<uint8_t>& v);
bool hexDecodeInto(const std::string& s, uint8_t* out, size_t outLen);

// Monero compact target (4 bytes on the wire, little-endian u32) -> 64-bit target.
uint64_t target64FromCompact(uint32_t compact);
uint32_t compactFromHex(const std::string& hex);

// Hash tail (last 8 bytes) read as little-endian u64 -> target for that hash.
uint64_t target64FromHash(const uint8_t hash[32]);

// Network difficulty convention: max_target / target.
uint64_t difficultyFromTarget64(uint64_t target64);

uint64_t parseU64LE(const uint8_t* p);

int64_t nowMs();
int64_t nowSec();
double  nowSteady();

int64_t parseI64(const std::string& s, int64_t def);

std::string trim(const std::string& s);
std::string toLower(std::string s);

}  // namespace util
