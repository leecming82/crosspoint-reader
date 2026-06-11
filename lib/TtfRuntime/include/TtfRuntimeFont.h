#pragma once

#include <cstddef>
#include <cstdint>

namespace ttf {

struct TableView {
  const uint8_t* data = nullptr;
  uint32_t length = 0;
};

struct RuntimeTableSet {
  TableView cmap;
  TableView head;
  TableView hhea;
  TableView hmtx;
  TableView loca;
  TableView maxp;
};

struct FontMetrics {
  uint16_t unitsPerEm = 0;
  int16_t ascender = 0;
  int16_t descender = 0;
  int16_t lineGap = 0;
  uint16_t numGlyphs = 0;
  uint16_t numLongHorMetrics = 0;
  int16_t indexToLocFormat = 0;
  uint16_t pixelSize = 0;
};

struct GlyphMetrics {
  uint32_t codepoint = 0;
  uint16_t glyphId = 0;
  bool present = false;
  uint16_t advanceWidth = 0;
  int16_t leftSideBearing = 0;
  int16_t advancePx = 0;
  int16_t leftSideBearingPx = 0;
  uint32_t glyphOffset = 0;
  uint32_t glyphLength = 0;
};

class TtfRuntimeFont {
 public:
  bool begin(const RuntimeTableSet& tables, uint16_t pixelSize, const char** error = nullptr);
  const FontMetrics& metrics() const { return metrics_; }
  GlyphMetrics metricsForCodepoint(uint32_t codepoint) const;
  uint16_t glyphIdForCodepoint(uint32_t codepoint) const;

 private:
  enum class CmapFormat : uint8_t {
    None,
    Format4,
    Format12,
  };

  RuntimeTableSet tables_{};
  FontMetrics metrics_{};
  CmapFormat cmapFormat_ = CmapFormat::None;
  uint32_t cmapOffset_ = 0;

  uint16_t glyphIdForFormat4(uint32_t codepoint) const;
  uint16_t glyphIdForFormat12(uint32_t codepoint) const;
  bool glyphRange(uint16_t glyphId, uint32_t& offset, uint32_t& length) const;
  int16_t toPixels(int32_t fontUnits) const;
};

}  // namespace ttf
