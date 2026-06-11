#pragma once

#include <cstdint>

struct ReaderFontIdentity {
  static constexpr uint16_t CURRENT_VERSION = 2;
  static constexpr uint8_t MODE_CPFONT = 0;
  static constexpr uint8_t MODE_TTF = 1;
  static constexpr uint8_t PROVIDER_CPFONT = 0;
  static constexpr uint8_t PROVIDER_TTF_OPENFONTRENDER = 1;
  static constexpr uint8_t PROVIDER_TTF_DIRECT_FREETYPE = 2;

  uint16_t version = CURRENT_VERSION;
  uint8_t mode = MODE_TTF;
  uint8_t provider = PROVIDER_TTF_DIRECT_FREETYPE;
  uint8_t pixelSize = 0;
  uint8_t reserved = 0;
  uint16_t weight = 400;
  uint32_t fileSize = 0;
  uint32_t fileHash = 0;
  uint32_t providerVersion = 0;
  int legacyFontId = 0;

  bool operator==(const ReaderFontIdentity& other) const {
    return version == other.version && mode == other.mode && provider == other.provider &&
           pixelSize == other.pixelSize && weight == other.weight && fileSize == other.fileSize &&
           fileHash == other.fileHash && providerVersion == other.providerVersion && legacyFontId == other.legacyFontId;
  }

  bool operator!=(const ReaderFontIdentity& other) const { return !(*this == other); }
};
