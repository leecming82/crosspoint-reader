#include "ReaderFontConfig.h"

#include <Epub.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "activities/reader/EpubReaderUtils.h"
#include "HalStorage.h"

namespace {

constexpr uint32_t FNV_OFFSET = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;
constexpr uint32_t TTF_PROVIDER_VERSION = 1;

uint32_t fnvStep(const uint32_t hash, const uint8_t byte) { return (hash ^ byte) * FNV_PRIME; }

int legacyFontIdFromHash(const uint32_t hash) {
  int fontId = static_cast<int>(hash | 0x80000000u);
  return fontId == 0 ? static_cast<int>(0x80000001u) : fontId;
}

uint32_t fileSizeForPath(const std::string& path) {
  if (path.empty()) return 0;
  HalFile file = Storage.open(path.c_str(), O_RDONLY);
  if (!file) return 0;
  const uint64_t size = file.fileSize64();
  file.close();
  return size > UINT32_MAX ? 0 : static_cast<uint32_t>(size);
}

uint16_t clampWeight(const uint16_t weight) {
  return static_cast<uint16_t>(std::max<uint16_t>(100, std::min<uint16_t>(weight, 900)));
}

ReaderFontConfig makeTtfConfig(const char* path, const uint8_t pixelSize, const uint16_t weight,
                               const uint32_t fileSize,
                               const ReaderFontConfig::Source source) {
  ReaderFontConfig config;
  config.source = source;
  config.ttfPath = path ? path : "";
  config.identity.mode = ReaderFontIdentity::MODE_TTF;
  config.identity.provider = ReaderFontIdentity::PROVIDER_TTF_DIRECT_FREETYPE;
  config.identity.pixelSize = std::max<uint8_t>(12, std::min<uint8_t>(pixelSize, 72));
  config.identity.weight = clampWeight(weight);
  config.identity.fileSize = fileSize;
  config.identity.fileHash =
      ReaderFontResolver::computeTtfIdentityHash(config.ttfPath.c_str(), config.identity.pixelSize,
                                                 config.identity.weight, fileSize);
  config.identity.providerVersion = TTF_PROVIDER_VERSION;
  config.identity.legacyFontId = legacyFontIdFromHash(config.identity.fileHash);
  return config;
}

}  // namespace

namespace ReaderFontResolver {

uint32_t computeTtfIdentityHash(const char* path, const uint8_t pixelSize, const uint16_t weight,
                                const uint32_t fileSize) {
  uint32_t hash = FNV_OFFSET;
  for (const char* p = path; p && *p; ++p) {
    hash = fnvStep(hash, static_cast<uint8_t>(*p));
  }
  hash = fnvStep(hash, pixelSize);
  hash = fnvStep(hash, static_cast<uint8_t>(weight & 0xFF));
  hash = fnvStep(hash, static_cast<uint8_t>((weight >> 8) & 0xFF));
  for (uint8_t i = 0; i < 4; ++i) {
    hash = fnvStep(hash, static_cast<uint8_t>((fileSize >> (i * 8)) & 0xFF));
  }
  return hash == 0 ? 1 : hash;
}

ReaderFontConfig resolveGlobal() {
  if (SETTINGS.readerTtfPath[0] != '\0') {
    const uint32_t fileSize =
        SETTINGS.readerTtfFileSize != 0 ? SETTINGS.readerTtfFileSize : fileSizeForPath(SETTINGS.readerTtfPath);
    return makeTtfConfig(SETTINGS.readerTtfPath, SETTINGS.readerTtfSizePx, SETTINGS.readerTtfWeight * 10, fileSize,
                         ReaderFontConfig::Source::Global);
  }
  ReaderFontConfig config;
  config.source = ReaderFontConfig::Source::Global;
  config.identity.mode = ReaderFontIdentity::MODE_TTF;
  config.identity.provider = ReaderFontIdentity::PROVIDER_TTF_DIRECT_FREETYPE;
  config.identity.pixelSize = std::max<uint8_t>(12, std::min<uint8_t>(SETTINGS.readerTtfSizePx, 72));
  config.identity.weight = clampWeight(SETTINGS.readerTtfWeight * 10);
  config.identity.providerVersion = TTF_PROVIDER_VERSION;
  return config;
}

ReaderFontConfig resolveForEpub(const Epub* epub) {
  if (epub) {
    EpubReaderUtils::EpubFontOverride override;
    if (EpubReaderUtils::loadFontOverride(*epub, override) && override.enabled && !override.path.empty() &&
        Storage.exists(override.path.c_str())) {
      if (override.fileSize == 0) {
        override.fileSize = fileSizeForPath(override.path);
      }
      return makeTtfConfig(override.path.c_str(), override.sizePx, override.weight, override.fileSize,
                           ReaderFontConfig::Source::EpubOverride);
    }
  }
  return resolveGlobal();
}

}  // namespace ReaderFontResolver
