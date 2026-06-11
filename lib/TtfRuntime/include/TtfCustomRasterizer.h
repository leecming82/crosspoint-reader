#pragma once

#include <cstddef>
#include <cstdint>

namespace ttf {

struct CustomRasterResult {
  bool ok = false;
  bool compound = false;
  uint16_t contourCount = 0;
  uint16_t pointCount = 0;
  int16_t xMin = 0;
  int16_t yMin = 0;
  int16_t xMax = 0;
  int16_t yMax = 0;
  int width = 0;
  int height = 0;
  int xOffset = 0;
  int yOffset = 0;
  size_t bitmapBytes = 0;
  const char* error = nullptr;
};

CustomRasterResult rasterizeSimpleGlyf(const uint8_t* glyf, uint32_t glyfLength, uint16_t unitsPerEm,
                                       uint16_t pixelSize, uint8_t* bitmap, size_t bitmapCapacity);

}  // namespace ttf
