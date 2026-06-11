#include "TtfRuntimeFont.h"

namespace ttf {
namespace {

uint16_t readU16BE(const uint8_t* p) { return (static_cast<uint16_t>(p[0]) << 8) | p[1]; }

int16_t readS16BE(const uint8_t* p) { return static_cast<int16_t>(readU16BE(p)); }

uint32_t readU32BE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

bool tableFits(const TableView& table, const uint32_t offset, const uint32_t size) {
  return table.data && offset <= table.length && size <= table.length - offset;
}

bool setError(const char** error, const char* message) {
  if (error) *error = message;
  return false;
}

int cmapRank(uint16_t platform, uint16_t encoding, uint16_t format) {
  if (format == 12 && platform == 3 && encoding == 10) return 5;
  if (format == 12 && platform == 0) return 4;
  if (format == 4 && platform == 3 && encoding == 1) return 3;
  if (format == 4 && platform == 3 && encoding == 0) return 2;
  if (format == 4 && platform == 0) return 1;
  return 0;
}

uint32_t locaEntry(const TableView& loca, const int16_t format, const uint16_t glyphId) {
  if (format == 0) {
    const uint32_t offset = static_cast<uint32_t>(glyphId) * 2;
    if (!tableFits(loca, offset, 2)) return 0;
    return static_cast<uint32_t>(readU16BE(loca.data + offset)) * 2;
  }

  const uint32_t offset = static_cast<uint32_t>(glyphId) * 4;
  if (!tableFits(loca, offset, 4)) return 0;
  return readU32BE(loca.data + offset);
}

}  // namespace

bool TtfRuntimeFont::begin(const RuntimeTableSet& tables, const uint16_t pixelSize, const char** error) {
  tables_ = tables;
  metrics_ = {};
  cmapFormat_ = CmapFormat::None;
  cmapOffset_ = 0;

  if (pixelSize == 0) return setError(error, "zero pixel size");
  if (!tableFits(tables_.head, 0, 54)) return setError(error, "missing or short head table");
  if (!tableFits(tables_.hhea, 0, 36)) return setError(error, "missing or short hhea table");
  if (!tableFits(tables_.maxp, 0, 6)) return setError(error, "missing or short maxp table");
  if (!tableFits(tables_.hmtx, 0, 4)) return setError(error, "missing or short hmtx table");
  if (!tableFits(tables_.loca, 0, 2)) return setError(error, "missing or short loca table");
  if (!tableFits(tables_.cmap, 0, 4)) return setError(error, "missing or short cmap table");

  metrics_.unitsPerEm = readU16BE(tables_.head.data + 18);
  metrics_.indexToLocFormat = readS16BE(tables_.head.data + 50);
  metrics_.ascender = readS16BE(tables_.hhea.data + 4);
  metrics_.descender = readS16BE(tables_.hhea.data + 6);
  metrics_.lineGap = readS16BE(tables_.hhea.data + 8);
  metrics_.numLongHorMetrics = readU16BE(tables_.hhea.data + 34);
  metrics_.numGlyphs = readU16BE(tables_.maxp.data + 4);
  metrics_.pixelSize = pixelSize;

  if (metrics_.unitsPerEm == 0) return setError(error, "invalid unitsPerEm");
  if (metrics_.numGlyphs == 0) return setError(error, "invalid glyph count");
  if (metrics_.numLongHorMetrics == 0) return setError(error, "invalid horizontal metric count");
  if (metrics_.indexToLocFormat != 0 && metrics_.indexToLocFormat != 1) {
    return setError(error, "unsupported loca format");
  }

  const uint32_t minHmtxBytes = static_cast<uint32_t>(metrics_.numLongHorMetrics) * 4;
  if (!tableFits(tables_.hmtx, 0, minHmtxBytes)) return setError(error, "short hmtx table");

  const uint32_t locaBytes = metrics_.indexToLocFormat == 0 ? static_cast<uint32_t>(metrics_.numGlyphs + 1) * 2
                                                            : static_cast<uint32_t>(metrics_.numGlyphs + 1) * 4;
  if (!tableFits(tables_.loca, 0, locaBytes)) return setError(error, "short loca table");

  const uint16_t cmapTables = readU16BE(tables_.cmap.data + 2);
  if (!tableFits(tables_.cmap, 4, static_cast<uint32_t>(cmapTables) * 8)) {
    return setError(error, "short cmap records");
  }

  int bestRank = 0;
  for (uint16_t i = 0; i < cmapTables; ++i) {
    const uint8_t* record = tables_.cmap.data + 4 + static_cast<uint32_t>(i) * 8;
    const uint16_t platform = readU16BE(record);
    const uint16_t encoding = readU16BE(record + 2);
    const uint32_t offset = readU32BE(record + 4);
    if (!tableFits(tables_.cmap, offset, 2)) continue;
    const uint16_t format = readU16BE(tables_.cmap.data + offset);
    const int rank = cmapRank(platform, encoding, format);
    if (rank > bestRank) {
      bestRank = rank;
      cmapOffset_ = offset;
      cmapFormat_ = format == 12 ? CmapFormat::Format12 : CmapFormat::Format4;
    }
  }

  if (cmapFormat_ == CmapFormat::None) return setError(error, "no supported cmap subtable");
  if (cmapFormat_ == CmapFormat::Format4 && !tableFits(tables_.cmap, cmapOffset_, 16)) {
    return setError(error, "short cmap format 4");
  }
  if (cmapFormat_ == CmapFormat::Format12 && !tableFits(tables_.cmap, cmapOffset_, 16)) {
    return setError(error, "short cmap format 12");
  }

  if (error) *error = nullptr;
  return true;
}

GlyphMetrics TtfRuntimeFont::metricsForCodepoint(const uint32_t codepoint) const {
  GlyphMetrics out{};
  out.codepoint = codepoint;
  out.glyphId = glyphIdForCodepoint(codepoint);
  out.present = out.glyphId != 0 && out.glyphId < metrics_.numGlyphs;
  if (!out.present) return out;

  uint32_t hmtxOffset = 0;
  if (out.glyphId < metrics_.numLongHorMetrics) {
    hmtxOffset = static_cast<uint32_t>(out.glyphId) * 4;
    out.advanceWidth = readU16BE(tables_.hmtx.data + hmtxOffset);
    out.leftSideBearing = readS16BE(tables_.hmtx.data + hmtxOffset + 2);
  } else {
    hmtxOffset = static_cast<uint32_t>(metrics_.numLongHorMetrics - 1) * 4;
    out.advanceWidth = readU16BE(tables_.hmtx.data + hmtxOffset);
    const uint32_t lsbOffset = static_cast<uint32_t>(metrics_.numLongHorMetrics) * 4 +
                               static_cast<uint32_t>(out.glyphId - metrics_.numLongHorMetrics) * 2;
    if (tableFits(tables_.hmtx, lsbOffset, 2)) {
      out.leftSideBearing = readS16BE(tables_.hmtx.data + lsbOffset);
    }
  }

  out.advancePx = toPixels(out.advanceWidth);
  out.leftSideBearingPx = toPixels(out.leftSideBearing);
  glyphRange(out.glyphId, out.glyphOffset, out.glyphLength);
  return out;
}

uint16_t TtfRuntimeFont::glyphIdForCodepoint(const uint32_t codepoint) const {
  switch (cmapFormat_) {
    case CmapFormat::Format4:
      return glyphIdForFormat4(codepoint);
    case CmapFormat::Format12:
      return glyphIdForFormat12(codepoint);
    case CmapFormat::None:
    default:
      return 0;
  }
}

uint16_t TtfRuntimeFont::glyphIdForFormat4(const uint32_t codepoint) const {
  if (codepoint > 0xFFFF || !tableFits(tables_.cmap, cmapOffset_, 16)) return 0;

  const uint8_t* table = tables_.cmap.data + cmapOffset_;
  const uint16_t length = readU16BE(table + 2);
  if (!tableFits(tables_.cmap, cmapOffset_, length)) return 0;

  const uint16_t segCount = readU16BE(table + 6) / 2;
  const uint32_t endCodeOffset = 14;
  const uint32_t startCodeOffset = endCodeOffset + static_cast<uint32_t>(segCount) * 2 + 2;
  const uint32_t idDeltaOffset = startCodeOffset + static_cast<uint32_t>(segCount) * 2;
  const uint32_t idRangeOffsetOffset = idDeltaOffset + static_cast<uint32_t>(segCount) * 2;
  if (idRangeOffsetOffset + static_cast<uint32_t>(segCount) * 2 > length) return 0;

  const uint16_t cp = static_cast<uint16_t>(codepoint);
  for (uint16_t i = 0; i < segCount; ++i) {
    const uint16_t endCode = readU16BE(table + endCodeOffset + static_cast<uint32_t>(i) * 2);
    if (cp > endCode) continue;
    const uint16_t startCode = readU16BE(table + startCodeOffset + static_cast<uint32_t>(i) * 2);
    if (cp < startCode) return 0;

    const int16_t idDelta = readS16BE(table + idDeltaOffset + static_cast<uint32_t>(i) * 2);
    const uint16_t idRangeOffset = readU16BE(table + idRangeOffsetOffset + static_cast<uint32_t>(i) * 2);
    if (idRangeOffset == 0) {
      return static_cast<uint16_t>(cp + idDelta);
    }

    const uint32_t glyphOffset = idRangeOffsetOffset + static_cast<uint32_t>(i) * 2 + idRangeOffset +
                                 static_cast<uint32_t>(cp - startCode) * 2;
    if (glyphOffset + 2 > length) return 0;
    const uint16_t glyph = readU16BE(table + glyphOffset);
    if (glyph == 0) return 0;
    return static_cast<uint16_t>(glyph + idDelta);
  }

  return 0;
}

uint16_t TtfRuntimeFont::glyphIdForFormat12(const uint32_t codepoint) const {
  if (!tableFits(tables_.cmap, cmapOffset_, 16)) return 0;

  const uint8_t* table = tables_.cmap.data + cmapOffset_;
  const uint32_t length = readU32BE(table + 4);
  if (!tableFits(tables_.cmap, cmapOffset_, length)) return 0;

  const uint32_t groupCount = readU32BE(table + 12);
  if (16 + groupCount * 12 > length) return 0;

  uint32_t lo = 0;
  uint32_t hi = groupCount;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const uint8_t* group = table + 16 + mid * 12;
    const uint32_t start = readU32BE(group);
    const uint32_t end = readU32BE(group + 4);
    if (codepoint < start) {
      hi = mid;
    } else if (codepoint > end) {
      lo = mid + 1;
    } else {
      const uint32_t glyph = readU32BE(group + 8) + codepoint - start;
      return glyph <= 0xFFFF ? static_cast<uint16_t>(glyph) : 0;
    }
  }

  return 0;
}

bool TtfRuntimeFont::glyphRange(const uint16_t glyphId, uint32_t& offset, uint32_t& length) const {
  offset = 0;
  length = 0;
  if (glyphId >= metrics_.numGlyphs) return false;

  const uint32_t start = locaEntry(tables_.loca, metrics_.indexToLocFormat, glyphId);
  const uint32_t end = locaEntry(tables_.loca, metrics_.indexToLocFormat, glyphId + 1);
  if (end < start) return false;
  offset = start;
  length = end - start;
  return true;
}

int16_t TtfRuntimeFont::toPixels(const int32_t fontUnits) const {
  const int32_t numerator = fontUnits * static_cast<int32_t>(metrics_.pixelSize);
  const int32_t rounded = numerator >= 0 ? numerator + metrics_.unitsPerEm / 2 : numerator - metrics_.unitsPerEm / 2;
  return static_cast<int16_t>(rounded / metrics_.unitsPerEm);
}

}  // namespace ttf
