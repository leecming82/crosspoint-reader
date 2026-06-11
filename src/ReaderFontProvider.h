#pragma once

#include <GfxRenderer.h>

#include <cstddef>
#include <cstdint>

#include "ReaderFontConfig.h"

struct ReaderFontCacheStats {
  size_t glyphCount = 0;
  size_t bytes = 0;
  size_t byteLimit = 0;
  uint32_t hits = 0;
  uint32_t misses = 0;
  uint32_t rasterOk = 0;
  uint32_t rasterFailed = 0;
  uint32_t missingGlyphs = 0;
  uint32_t evictions = 0;
  bool persistentDirty = false;
};

class ReaderFontProvider : public ReaderFontMetricsProvider {
 public:
  ~ReaderFontProvider() override = default;

  virtual bool ensureLoaded(const ReaderFontConfig& config) = 0;
  virtual const ReaderFontConfig& activeConfig() const = 0;
  virtual int fontId() const = 0;
  virtual bool flushPersistentCache() const = 0;
  virtual ReaderFontCacheStats cacheStats() const = 0;
  virtual void unload() = 0;
};

namespace ReaderFontProviders {
ReaderFontProvider* providerForConfig(const ReaderFontConfig& config);
}
