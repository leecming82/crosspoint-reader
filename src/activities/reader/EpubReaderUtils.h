#pragma once

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace EpubReaderUtils {

struct EpubFontOverride {
  bool enabled = false;
  std::string path;
  uint8_t sizePx = 36;
  uint32_t fileSize = 0;
};

inline std::string fontOverridePath(const Epub& epub) { return epub.getCachePath() + "/epub_font.bin"; }

inline uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline void writeLe32(uint8_t* p, const uint32_t value) {
  p[0] = static_cast<uint8_t>(value & 0xFF);
  p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

inline bool loadFontOverride(const Epub& epub, EpubFontOverride& out) {
  out = EpubFontOverride{};
  HalFile f;
  if (!Storage.openFileForRead("ERS", fontOverridePath(epub), f)) {
    return false;
  }

  uint8_t header[14] = {};
  if (f.read(header, sizeof(header)) != sizeof(header)) {
    return false;
  }
  if (memcmp(header, "XPEF", 4) != 0 || header[4] != 1) {
    LOG_DBG("ERS", "Ignoring unsupported EPUB font override");
    return false;
  }

  const uint32_t fileSize = readLe32(header + 6);
  const uint32_t pathLen = readLe32(header + 10);
  if (pathLen == 0 || pathLen > 180) {
    return false;
  }

  std::string path(pathLen, '\0');
  if (f.read(reinterpret_cast<uint8_t*>(path.data()), pathLen) != static_cast<int>(pathLen)) {
    return false;
  }

  out.enabled = true;
  out.sizePx = std::clamp<uint8_t>(header[5], 18, 72);
  out.fileSize = fileSize;
  out.path = std::move(path);
  return out.enabled && !out.path.empty();
}

inline bool saveFontOverride(const Epub& epub, const EpubFontOverride& value) {
  if (!value.enabled || value.path.empty()) {
    Storage.remove(fontOverridePath(epub).c_str());
    return true;
  }

  HalFile f;
  if (!Storage.openFileForWrite("ERS", fontOverridePath(epub), f)) {
    LOG_ERR("ERS", "Could not open EPUB font override for write");
    return false;
  }

  const uint32_t pathLen = static_cast<uint32_t>(std::min<size_t>(value.path.size(), 180));
  uint8_t header[14] = {};
  memcpy(header, "XPEF", 4);
  header[4] = 1;
  header[5] = std::clamp<uint8_t>(value.sizePx, 18, 72);
  writeLe32(header + 6, value.fileSize);
  writeLe32(header + 10, pathLen);

  if (f.write(header, sizeof(header)) != sizeof(header) ||
      f.write(reinterpret_cast<const uint8_t*>(value.path.data()), pathLen) != pathLen) {
    LOG_ERR("ERS", "Short write saving EPUB font override");
    return false;
  }
  return true;
}

inline bool clearFontOverride(const Epub& epub) {
  const std::string path = fontOverridePath(epub);
  if (Storage.exists(path.c_str())) {
    return Storage.remove(path.c_str());
  }
  return true;
}

inline void clearSectionCache(const Epub& epub) { Storage.removeDir((epub.getCachePath() + "/sections").c_str()); }

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(Epub& epub, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  HalFile f;
  if (!Storage.openFileForWrite("ERS", epub.getCachePath() + "/progress.bin", f)) {
    LOG_ERR("ERS", "Could not open progress file for write!");
    return false;
  }
  uint8_t data[6];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  const size_t written = f.write(data, sizeof(data));
  if (written != sizeof(data)) {
    LOG_ERR("ERS", "Short write saving progress: %u/%u bytes", (unsigned)written, (unsigned)sizeof(data));
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d", spineIndex, pageNumber);
  return true;
}

}  // namespace EpubReaderUtils
