#ifdef CROSSPOINT_BOARD_MURPHY_M4

#include "TtfFontScanner.h"

#include <HalStorage.h>
#include <Logging.h>
#include <TtfProbe.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

constexpr size_t TTF_HEADER_SIZE = 12;
constexpr size_t TTF_TABLE_RECORD_SIZE = 16;

struct HeapFreeDeleter {
  void operator()(uint8_t* ptr) const {
    if (ptr) free(ptr);
  }
};

uint16_t readU16BE(const uint8_t* p) { return (static_cast<uint16_t>(p[0]) << 8) | p[1]; }

bool endsWithTtf(const String& name) {
  const int len = name.length();
  if (len < 4) return false;
  return std::tolower(static_cast<unsigned char>(name[len - 4])) == '.' &&
         std::tolower(static_cast<unsigned char>(name[len - 3])) == 't' &&
         std::tolower(static_cast<unsigned char>(name[len - 2])) == 't' &&
         std::tolower(static_cast<unsigned char>(name[len - 1])) == 'f';
}

std::string normalizeListedPath(const char* directory, const String& listed) {
  const std::string value = listed.c_str();
  if (!value.empty() && value[0] == '/') return value;
  return std::string(directory) + "/" + value;
}

bool readExact(HalFile& file, uint8_t* dest, const size_t len) {
  size_t total = 0;
  while (total < len) {
    const int got = file.read(dest + total, len - total);
    if (got <= 0) return false;
    total += static_cast<size_t>(got);
  }
  return true;
}

bool probeCandidate(const std::string& path, TtfFontCandidate& out) {
  HalFile file = Storage.open(path.c_str(), O_RDONLY);
  if (!file) {
    LOG_DBG("TTFS", "skip unreadable TTF candidate: %s", path.c_str());
    return false;
  }

  const uint64_t fileSize = file.fileSize64();
  if (fileSize < TTF_HEADER_SIZE || fileSize > SIZE_MAX) {
    file.close();
    return false;
  }

  uint8_t header[TTF_HEADER_SIZE] = {};
  if (!readExact(file, header, sizeof(header))) {
    file.close();
    return false;
  }

  const uint16_t tableCount = readU16BE(header + 4);
  const size_t directorySize = TTF_HEADER_SIZE + static_cast<size_t>(tableCount) * TTF_TABLE_RECORD_SIZE;
  if (directorySize > static_cast<size_t>(fileSize)) {
    file.close();
    return false;
  }

  std::unique_ptr<uint8_t, HeapFreeDeleter> directoryData(static_cast<uint8_t*>(malloc(directorySize)));
  if (!directoryData) {
    file.close();
    return false;
  }
  memcpy(directoryData.get(), header, sizeof(header));
  if (directorySize > TTF_HEADER_SIZE &&
      !readExact(file, directoryData.get() + TTF_HEADER_SIZE, directorySize - TTF_HEADER_SIZE)) {
    file.close();
    return false;
  }
  file.close();

  const auto probe = ttf::probeSfntDirectory(directoryData.get(), directorySize, fileSize);
  if (!probe.ok) {
    LOG_DBG("TTFS", "skip invalid TTF candidate path=%s error=%s", path.c_str(), probe.error ? probe.error : "unknown");
    return false;
  }

  out.path = path;
  out.fileSize = fileSize;
  out.tableCount = probe.tableCount;
  out.hasKern = probe.hasKern;
  return true;
}

}  // namespace

std::vector<TtfFontCandidate> TtfFontScanner::scan(const char* directory, const size_t maxEntries) {
  std::vector<TtfFontCandidate> candidates;
  if (!directory || directory[0] == '\0') return candidates;

  const std::vector<String> entries = Storage.listFiles(directory, maxEntries);
  for (const auto& entry : entries) {
    if (!endsWithTtf(entry)) continue;

    TtfFontCandidate candidate;
    const std::string path = normalizeListedPath(directory, entry);
    if (probeCandidate(path, candidate)) {
      candidates.push_back(std::move(candidate));
    }
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const TtfFontCandidate& a, const TtfFontCandidate& b) { return a.path < b.path; });
  LOG_DBG("TTFS", "discovered %u valid TTF candidate(s) in %s", static_cast<unsigned>(candidates.size()), directory);
  return candidates;
}

#endif
