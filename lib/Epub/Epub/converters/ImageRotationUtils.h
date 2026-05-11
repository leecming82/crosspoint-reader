#pragma once

#include "DirectPixelWriter.h"
#include "ImageToFramebufferDecoder.h"

inline bool imageIsRotated(const RenderConfig& config) { return config.rotation != ImageRotation::None; }

inline void mapRotatedImagePixel(const RenderConfig& config, int dstX, int dstY, int& outX, int& outY) {
  if (config.rotation == ImageRotation::Clockwise) {
    outX = config.x + (config.maxWidth - 1 - dstY);
    outY = config.y + dstX;
  } else if (config.rotation == ImageRotation::CounterClockwise) {
    outX = config.x + dstY;
    outY = config.y + (config.maxHeight - 1 - dstX);
  } else {
    outX = config.x + dstX;
    outY = config.y + dstY;
  }
}

inline void writeImagePixel(DirectPixelWriter& pw, DirectCacheWriter& cw, bool caching, const RenderConfig& config,
                            int dstX, int dstY, uint8_t value) {
  int outX, outY;
  mapRotatedImagePixel(config, dstX, dstY, outX, outY);
  if (imageIsRotated(config)) {
    pw.writePixelAt(outX, outY, value);
    if (caching) {
      cw.beginRow(outY, config.y);
      cw.writePixel(outX, value);
    }
  } else {
    pw.writePixel(outX, value);
    if (caching) cw.writePixel(outX, value);
  }
}
