#pragma once

#ifdef CROSSPOINT_BOARD_MURPHY_M4

#include <cstdint>
#include <string>
#include <vector>

struct TtfFontCandidate {
  std::string path;
  uint64_t fileSize = 0;
  uint16_t tableCount = 0;
  bool hasKern = false;
};

class TtfFontScanner {
 public:
  static constexpr const char* DEFAULT_TTF_DIR = "/TTF";

  static std::vector<TtfFontCandidate> scan(const char* directory = DEFAULT_TTF_DIR, size_t maxEntries = 64);
};

#endif
