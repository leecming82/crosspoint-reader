#pragma once

#include <cstddef>
#include <cstdint>

namespace ttf {

struct TableRecord {
  uint32_t tag = 0;
  uint32_t checksum = 0;
  uint32_t offset = 0;
  uint32_t length = 0;
};

struct ProbeResult {
  bool ok = false;
  bool isCollection = false;
  uint16_t tableCount = 0;
  bool hasCmap = false;
  bool hasGlyf = false;
  bool hasHead = false;
  bool hasHhea = false;
  bool hasHmtx = false;
  bool hasLoca = false;
  bool hasMaxp = false;
  bool hasKern = false;
  const char* error = nullptr;
};

constexpr uint32_t tag(char a, char b, char c, char d) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(d));
}

ProbeResult probeSfnt(const uint8_t* data, size_t len);
ProbeResult probeSfntDirectory(const uint8_t* data, size_t len, uint64_t fileSize);

}  // namespace ttf
