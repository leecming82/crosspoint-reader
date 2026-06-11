#pragma once

#include <cstddef>
#include <cstdint>

#include "TtfProbe.h"

namespace ttf {

struct StbProbeResult {
  bool ok = false;
  ProbeResult sfnt{};
  int ascent = 0;
  int descent = 0;
  int lineGap = 0;
  int glyphA = 0;
  const char* error = nullptr;
};

struct StbRasterResult {
  bool ok = false;
  ProbeResult sfnt{};
  uint32_t codepoint = 0;
  int glyph = 0;
  int width = 0;
  int height = 0;
  int xOffset = 0;
  int yOffset = 0;
  int advanceWidth = 0;
  int leftSideBearing = 0;
  size_t bitmapBytes = 0;
  const char* error = nullptr;
};

StbProbeResult probeStbTruetype(const uint8_t* data, size_t len);
StbRasterResult rasterizeStbGlyph(const uint8_t* data, size_t len, uint32_t codepoint, float pixelHeight,
                                  uint8_t* bitmap, size_t bitmapCapacity);

}  // namespace ttf
