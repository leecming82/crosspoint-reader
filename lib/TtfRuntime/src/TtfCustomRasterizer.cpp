#include "TtfCustomRasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace ttf {
namespace {

constexpr uint8_t FLAG_ON_CURVE = 0x01;
constexpr uint8_t FLAG_X_SHORT = 0x02;
constexpr uint8_t FLAG_Y_SHORT = 0x04;
constexpr uint8_t FLAG_REPEAT = 0x08;
constexpr uint8_t FLAG_X_SAME_OR_POSITIVE = 0x10;
constexpr uint8_t FLAG_Y_SAME_OR_POSITIVE = 0x20;

struct GlyfPoint {
  int16_t x = 0;
  int16_t y = 0;
  bool onCurve = false;
};

struct Segment {
  float x0 = 0.0f;
  float y0 = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
};

uint16_t readU16BE(const uint8_t* p) { return (static_cast<uint16_t>(p[0]) << 8) | p[1]; }

int16_t readS16BE(const uint8_t* p) { return static_cast<int16_t>(readU16BE(p)); }

bool fits(uint32_t len, uint32_t offset, uint32_t size) { return offset <= len && size <= len - offset; }

bool readI16Delta(const uint8_t* glyf, const uint32_t glyfLength, uint32_t& offset, int16_t& out) {
  if (!fits(glyfLength, offset, 2)) return false;
  out = readS16BE(glyf + offset);
  offset += 2;
  return true;
}

GlyfPoint midpoint(const GlyfPoint& a, const GlyfPoint& b) {
  GlyfPoint out;
  out.x = static_cast<int16_t>((static_cast<int32_t>(a.x) + b.x) / 2);
  out.y = static_cast<int16_t>((static_cast<int32_t>(a.y) + b.y) / 2);
  out.onCurve = true;
  return out;
}

float toPixelX(const int16_t x, const int16_t xMin, const float scale) {
  return (static_cast<float>(x) - static_cast<float>(xMin)) * scale;
}

float toPixelY(const int16_t y, const int16_t yMax, const float scale) {
  return (static_cast<float>(yMax) - static_cast<float>(y)) * scale;
}

void addLine(std::vector<Segment>& segments, const GlyfPoint& a, const GlyfPoint& b, const int16_t xMin,
             const int16_t yMax, const float scale) {
  segments.push_back({toPixelX(a.x, xMin, scale), toPixelY(a.y, yMax, scale), toPixelX(b.x, xMin, scale),
                      toPixelY(b.y, yMax, scale)});
}

GlyfPoint quadPoint(const GlyfPoint& p0, const GlyfPoint& p1, const GlyfPoint& p2, const float t) {
  const float mt = 1.0f - t;
  GlyfPoint out;
  out.x = static_cast<int16_t>(std::lround(mt * mt * p0.x + 2.0f * mt * t * p1.x + t * t * p2.x));
  out.y = static_cast<int16_t>(std::lround(mt * mt * p0.y + 2.0f * mt * t * p1.y + t * t * p2.y));
  out.onCurve = true;
  return out;
}

void addQuadratic(std::vector<Segment>& segments, const GlyfPoint& p0, const GlyfPoint& p1, const GlyfPoint& p2,
                  const int16_t xMin, const int16_t yMax, const float scale) {
  GlyfPoint last = p0;
  const int steps = std::max(4, std::min(24, static_cast<int>(std::ceil(std::max(std::abs(p2.x - p0.x),
                                                                                std::abs(p2.y - p0.y)) *
                                                                       scale / 4.0f))));
  for (int i = 1; i <= steps; ++i) {
    GlyfPoint next = quadPoint(p0, p1, p2, static_cast<float>(i) / static_cast<float>(steps));
    addLine(segments, last, next, xMin, yMax, scale);
    last = next;
  }
}

bool appendContourSegments(std::vector<Segment>& segments, const std::vector<GlyfPoint>& points, const uint16_t start,
                           const uint16_t end, const int16_t xMin, const int16_t yMax, const float scale) {
  if (end < start || end >= points.size()) return false;

  std::vector<GlyfPoint> contour;
  contour.reserve(end - start + 2);
  for (uint16_t i = start; i <= end; ++i) {
    const GlyfPoint& current = points[i];
    const GlyfPoint& next = i == end ? points[start] : points[i + 1];
    contour.push_back(current);
    if (!current.onCurve && !next.onCurve) {
      contour.push_back(midpoint(current, next));
    }
  }

  int firstOn = -1;
  for (size_t i = 0; i < contour.size(); ++i) {
    if (contour[i].onCurve) {
      firstOn = static_cast<int>(i);
      break;
    }
  }
  if (firstOn < 0) return false;

  GlyfPoint current = contour[firstOn];
  const size_t count = contour.size();
  for (size_t step = 1; step <= count; ++step) {
    const GlyfPoint& next = contour[(static_cast<size_t>(firstOn) + step) % count];
    if (next.onCurve) {
      addLine(segments, current, next, xMin, yMax, scale);
      current = next;
      continue;
    }

    const GlyfPoint& endPoint = contour[(static_cast<size_t>(firstOn) + step + 1) % count];
    if (!endPoint.onCurve) return false;
    addQuadratic(segments, current, next, endPoint, xMin, yMax, scale);
    current = endPoint;
    ++step;
  }

  return true;
}

void fillBitmap(const std::vector<Segment>& segments, uint8_t* bitmap, const int width, const int height) {
  std::vector<float> crossings;
  crossings.reserve(64);

  for (int y = 0; y < height; ++y) {
    const float scanY = static_cast<float>(y) + 0.5f;
    crossings.clear();
    for (const auto& segment : segments) {
      if (segment.y0 == segment.y1) continue;
      const float yMin = std::min(segment.y0, segment.y1);
      const float yMax = std::max(segment.y0, segment.y1);
      if (scanY < yMin || scanY >= yMax) continue;
      const float t = (scanY - segment.y0) / (segment.y1 - segment.y0);
      crossings.push_back(segment.x0 + t * (segment.x1 - segment.x0));
    }

    std::sort(crossings.begin(), crossings.end());
    for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
      int x0 = static_cast<int>(std::ceil(crossings[i]));
      int x1 = static_cast<int>(std::floor(crossings[i + 1]));
      x0 = std::max(0, std::min(width - 1, x0));
      x1 = std::max(0, std::min(width - 1, x1));
      for (int x = x0; x <= x1; ++x) {
        bitmap[static_cast<size_t>(y) * width + x] = 255;
      }
    }
  }
}

}  // namespace

CustomRasterResult rasterizeSimpleGlyf(const uint8_t* glyf, const uint32_t glyfLength, const uint16_t unitsPerEm,
                                       const uint16_t pixelSize, uint8_t* bitmap, const size_t bitmapCapacity) {
  CustomRasterResult result{};
  if (!glyf || glyfLength < 10) {
    result.error = "empty or short glyf";
    return result;
  }
  if (unitsPerEm == 0 || pixelSize == 0) {
    result.error = "invalid scale";
    return result;
  }
  if (!bitmap || bitmapCapacity == 0) {
    result.error = "missing bitmap buffer";
    return result;
  }

  const int16_t contourCount = readS16BE(glyf);
  result.xMin = readS16BE(glyf + 2);
  result.yMin = readS16BE(glyf + 4);
  result.xMax = readS16BE(glyf + 6);
  result.yMax = readS16BE(glyf + 8);
  if (contourCount < 0) {
    result.compound = true;
    result.error = "compound glyph not supported";
    return result;
  }
  if (contourCount == 0 || result.xMax <= result.xMin || result.yMax <= result.yMin) {
    result.ok = true;
    return result;
  }
  result.contourCount = static_cast<uint16_t>(contourCount);

  uint32_t offset = 10;
  if (!fits(glyfLength, offset, static_cast<uint32_t>(result.contourCount) * 2)) {
    result.error = "short contour endpoints";
    return result;
  }

  std::vector<uint16_t> endPts;
  endPts.reserve(result.contourCount);
  for (uint16_t i = 0; i < result.contourCount; ++i) {
    endPts.push_back(readU16BE(glyf + offset));
    offset += 2;
  }

  const uint16_t pointCount = endPts.back() + 1;
  result.pointCount = pointCount;
  if (pointCount == 0) {
    result.ok = true;
    return result;
  }

  if (!fits(glyfLength, offset, 2)) {
    result.error = "missing instruction length";
    return result;
  }
  const uint16_t instructionLength = readU16BE(glyf + offset);
  offset += 2;
  if (!fits(glyfLength, offset, instructionLength)) {
    result.error = "short instructions";
    return result;
  }
  offset += instructionLength;

  std::vector<uint8_t> flags;
  flags.reserve(pointCount);
  while (flags.size() < pointCount) {
    if (!fits(glyfLength, offset, 1)) {
      result.error = "short flags";
      return result;
    }
    const uint8_t flag = glyf[offset++];
    flags.push_back(flag);
    if (flag & FLAG_REPEAT) {
      if (!fits(glyfLength, offset, 1)) {
        result.error = "short repeat flag";
        return result;
      }
      const uint8_t repeat = glyf[offset++];
      for (uint8_t i = 0; i < repeat && flags.size() < pointCount; ++i) {
        flags.push_back(flag);
      }
    }
  }

  std::vector<GlyfPoint> points(pointCount);
  int16_t x = 0;
  for (uint16_t i = 0; i < pointCount; ++i) {
    const uint8_t flag = flags[i];
    int16_t delta = 0;
    if (flag & FLAG_X_SHORT) {
      if (!fits(glyfLength, offset, 1)) {
        result.error = "short x coordinates";
        return result;
      }
      delta = glyf[offset++];
      if (!(flag & FLAG_X_SAME_OR_POSITIVE)) delta = -delta;
    } else if (!(flag & FLAG_X_SAME_OR_POSITIVE)) {
      if (!readI16Delta(glyf, glyfLength, offset, delta)) {
        result.error = "short x coordinate";
        return result;
      }
    }
    x = static_cast<int16_t>(x + delta);
    points[i].x = x;
    points[i].onCurve = (flag & FLAG_ON_CURVE) != 0;
  }

  int16_t y = 0;
  for (uint16_t i = 0; i < pointCount; ++i) {
    const uint8_t flag = flags[i];
    int16_t delta = 0;
    if (flag & FLAG_Y_SHORT) {
      if (!fits(glyfLength, offset, 1)) {
        result.error = "short y coordinates";
        return result;
      }
      delta = glyf[offset++];
      if (!(flag & FLAG_Y_SAME_OR_POSITIVE)) delta = -delta;
    } else if (!(flag & FLAG_Y_SAME_OR_POSITIVE)) {
      if (!readI16Delta(glyf, glyfLength, offset, delta)) {
        result.error = "short y coordinate";
        return result;
      }
    }
    y = static_cast<int16_t>(y + delta);
    points[i].y = y;
  }

  const float scale = static_cast<float>(pixelSize) / static_cast<float>(unitsPerEm);
  result.width = std::max(1, static_cast<int>(std::ceil((result.xMax - result.xMin) * scale)) + 1);
  result.height = std::max(1, static_cast<int>(std::ceil((result.yMax - result.yMin) * scale)) + 1);
  result.xOffset = static_cast<int>(std::floor(result.xMin * scale));
  result.yOffset = -static_cast<int>(std::ceil(result.yMax * scale));
  result.bitmapBytes = static_cast<size_t>(result.width) * result.height;
  if (result.bitmapBytes > bitmapCapacity) {
    result.error = "bitmap buffer too small";
    return result;
  }

  std::vector<Segment> segments;
  segments.reserve(pointCount * 2);
  uint16_t start = 0;
  for (const uint16_t end : endPts) {
    if (!appendContourSegments(segments, points, start, end, result.xMin, result.yMax, scale)) {
      result.error = "failed to build contour";
      return result;
    }
    start = end + 1;
  }

  std::memset(bitmap, 0, result.bitmapBytes);
  fillBitmap(segments, bitmap, result.width, result.height);
  result.ok = true;
  return result;
}

}  // namespace ttf
