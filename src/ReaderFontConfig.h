#pragma once

#include <cstdint>
#include <string>

#include <Epub/ReaderFontIdentity.h>

struct ReaderFontConfig {
  enum class Source : uint8_t { Global, EpubOverride };

  ReaderFontIdentity identity;
  Source source = Source::Global;
  std::string ttfPath;

  bool isTtf() const { return identity.mode == ReaderFontIdentity::MODE_TTF; }
  int legacyFontId() const { return identity.legacyFontId; }
};

class Epub;

namespace ReaderFontResolver {

ReaderFontConfig resolveGlobal();
ReaderFontConfig resolveForEpub(const Epub* epub);
uint32_t computeTtfIdentityHash(const char* path, uint8_t pixelSize, uint16_t weight, uint32_t fileSize);

}  // namespace ReaderFontResolver
