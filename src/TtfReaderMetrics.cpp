#ifdef CROSSPOINT_BOARD_MURPHY_M4

#include "TtfReaderMetrics.h"

#include <HalStorage.h>
#include <Logging.h>
#include <TtfCustomRasterizer.h>
#include <TtfProbe.h>
#include <Utf8.h>
#include <esp32-hal.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <climits>
#include <cstring>

#include "CrossPointSettings.h"

namespace {

constexpr size_t TTF_HEADER_SIZE = 12;
constexpr size_t TTF_TABLE_RECORD_SIZE = 16;
constexpr size_t TTF_RASTER_SCRATCH_BYTES = 16 * 1024;
constexpr size_t TTF_GLYPH_CACHE_MAX_BYTES = 512 * 1024;
constexpr uint32_t TTF_STATS_LOG_INTERVAL_MS = 3000;
constexpr uint32_t FNV_OFFSET = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

uint16_t readU16BE(const uint8_t* p) { return (static_cast<uint16_t>(p[0]) << 8) | p[1]; }

uint32_t readU32BE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

bool readExact(HalFile& file, uint8_t* dest, const size_t len) {
  size_t total = 0;
  while (total < len) {
    const int got = file.read(dest + total, len - total);
    if (got <= 0) return false;
    total += static_cast<size_t>(got);
  }
  return true;
}

bool findTableRecord(const uint8_t* directoryData, const size_t directorySize, const uint32_t tag,
                     ttf::TableRecord& out) {
  if (!directoryData || directorySize < TTF_HEADER_SIZE) return false;
  const uint16_t tableCount = readU16BE(directoryData + 4);
  const size_t needed = TTF_HEADER_SIZE + static_cast<size_t>(tableCount) * TTF_TABLE_RECORD_SIZE;
  if (needed > directorySize) return false;

  for (uint16_t i = 0; i < tableCount; ++i) {
    const uint8_t* record = directoryData + TTF_HEADER_SIZE + static_cast<size_t>(i) * TTF_TABLE_RECORD_SIZE;
    if (readU32BE(record) == tag) {
      out.tag = tag;
      out.checksum = readU32BE(record + 4);
      out.offset = readU32BE(record + 8);
      out.length = readU32BE(record + 12);
      return true;
    }
  }
  return false;
}

bool loadDirectoryReadOnly(const char* path, std::unique_ptr<uint8_t, TtfReaderMetrics::PsramFreeDeleter>& outData,
                           size_t& outSize, uint64_t& outFileSize) {
  outData.reset();
  outSize = 0;
  outFileSize = 0;

  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) return false;

  outFileSize = file.fileSize64();
  if (outFileSize < TTF_HEADER_SIZE || outFileSize > SIZE_MAX) {
    file.close();
    return false;
  }

  uint8_t header[TTF_HEADER_SIZE] = {};
  if (!readExact(file, header, sizeof(header))) {
    file.close();
    return false;
  }

  const uint16_t tableCount = readU16BE(header + 4);
  const size_t directorySize = TTF_HEADER_SIZE + static_cast<size_t>(tableCount) * TTF_TABLE_RECORD_SIZE;
  if (directorySize > static_cast<size_t>(outFileSize)) {
    file.close();
    return false;
  }

  uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(directorySize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!raw) {
    file.close();
    return false;
  }
  std::unique_ptr<uint8_t, TtfReaderMetrics::PsramFreeDeleter> buffer(raw);
  memcpy(buffer.get(), header, sizeof(header));
  if (directorySize > TTF_HEADER_SIZE &&
      !readExact(file, buffer.get() + TTF_HEADER_SIZE, directorySize - TTF_HEADER_SIZE)) {
    file.close();
    return false;
  }

  file.close();
  outSize = directorySize;
  outData = std::move(buffer);
  return true;
}

bool loadTableReadOnly(const char* path, const uint8_t* directoryData, const size_t directorySize, const uint32_t tag,
                       TtfReaderMetrics::OwnedTable& out) {
  out.reset();

  ttf::TableRecord record;
  if (!findTableRecord(directoryData, directorySize, tag, record) || record.length == 0) return false;

  const size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (record.length + 64 * 1024 > freePsram) {
    LOG_ERR("TTFR", "Not enough PSRAM for table tag=0x%08lX len=%lu free=%lu", static_cast<unsigned long>(tag),
            static_cast<unsigned long>(record.length), static_cast<unsigned long>(freePsram));
    return false;
  }

  uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(record.length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!raw) return false;

  std::unique_ptr<uint8_t, TtfReaderMetrics::PsramFreeDeleter> buffer(raw);
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) return false;
  if (!file.seek(record.offset) || !readExact(file, buffer.get(), record.length)) {
    file.close();
    return false;
  }
  file.close();

  out.data = std::move(buffer);
  out.length = record.length;
  return true;
}

uint32_t fnvStep(uint32_t hash, const uint8_t byte) { return (hash ^ byte) * FNV_PRIME; }

}  // namespace

TtfReaderMetrics TTF_READER_METRICS;

void TtfReaderMetrics::PsramFreeDeleter::operator()(uint8_t* ptr) const {
  if (ptr) heap_caps_free(ptr);
}

void TtfReaderMetrics::TableCache::reset() {
  cmap.reset();
  head.reset();
  hhea.reset();
  hmtx.reset();
  loca.reset();
  maxp.reset();
}

ttf::RuntimeTableSet TtfReaderMetrics::TableCache::views() const {
  return {cmap.view(), head.view(), hhea.view(), hmtx.view(), loca.view(), maxp.view()};
}

bool TtfReaderMetrics::ensureLoadedFromSettings() {
  if (SETTINGS.readerFontMode != CrossPointSettings::READER_FONT_TTF || SETTINGS.readerTtfPath[0] == '\0') {
    unload();
    return false;
  }

  const uint8_t size = std::max<uint8_t>(12, std::min<uint8_t>(SETTINGS.readerTtfSizePx, 72));
  const uint32_t expectedSize = SETTINGS.readerTtfFileSize;
  if (loaded_ && path_ == SETTINGS.readerTtfPath && pixelSize_ == size &&
      (expectedSize == 0 || fileSize_ == expectedSize)) {
    return true;
  }

  return loadFromPath(SETTINGS.readerTtfPath, size, expectedSize);
}

void TtfReaderMetrics::unload() {
  clearGlyphCache();
  tables_.reset();
  font_ = ttf::TtfRuntimeFont();
  path_.clear();
  pixelSize_ = 0;
  fileSize_ = 0;
  identityHash_ = 0;
  glyfTableOffset_ = 0;
  glyfTableLength_ = 0;
  fontId_ = INVALID_FONT_ID;
  loaded_ = false;
}

bool TtfReaderMetrics::loadFromPath(const char* path, const uint8_t pixelSize, const uint32_t expectedFileSize) {
  unload();
  if (!path || path[0] == '\0') return false;

  PsramBuffer directoryData;
  size_t directorySize = 0;
  uint64_t actualFileSize = 0;
  if (!loadDirectoryReadOnly(path, directoryData, directorySize, actualFileSize)) {
    LOG_ERR("TTFR", "Failed to load TTF directory path=%s", path);
    return false;
  }
  if (actualFileSize > UINT32_MAX) {
    LOG_ERR("TTFR", "TTF too large for reader identity path=%s size=%llu", path,
            static_cast<unsigned long long>(actualFileSize));
    return false;
  }

  const auto probe = ttf::probeSfntDirectory(directoryData.get(), directorySize, actualFileSize);
  if (!probe.ok) {
    LOG_ERR("TTFR", "Invalid TTF path=%s error=%s", path, probe.error ? probe.error : "unknown");
    return false;
  }

  if (!loadTableReadOnly(path, directoryData.get(), directorySize, ttf::tag('c', 'm', 'a', 'p'), tables_.cmap) ||
      !loadTableReadOnly(path, directoryData.get(), directorySize, ttf::tag('h', 'e', 'a', 'd'), tables_.head) ||
      !loadTableReadOnly(path, directoryData.get(), directorySize, ttf::tag('h', 'h', 'e', 'a'), tables_.hhea) ||
      !loadTableReadOnly(path, directoryData.get(), directorySize, ttf::tag('h', 'm', 't', 'x'), tables_.hmtx) ||
      !loadTableReadOnly(path, directoryData.get(), directorySize, ttf::tag('l', 'o', 'c', 'a'), tables_.loca) ||
      !loadTableReadOnly(path, directoryData.get(), directorySize, ttf::tag('m', 'a', 'x', 'p'), tables_.maxp)) {
    LOG_ERR("TTFR", "Failed to load required metric tables path=%s", path);
    unload();
    return false;
  }

  ttf::TableRecord glyfRecord;
  if (!findTableRecord(directoryData.get(), directorySize, ttf::tag('g', 'l', 'y', 'f'), glyfRecord)) {
    LOG_ERR("TTFR", "Missing glyf table path=%s", path);
    unload();
    return false;
  }
  glyfTableOffset_ = glyfRecord.offset;
  glyfTableLength_ = glyfRecord.length;

  const char* error = nullptr;
  if (!font_.begin(tables_.views(), pixelSize, &error)) {
    LOG_ERR("TTFR", "Failed to initialize metrics path=%s error=%s", path, error ? error : "unknown");
    unload();
    return false;
  }

  path_ = path;
  pixelSize_ = pixelSize;
  fileSize_ = static_cast<uint32_t>(actualFileSize);
  identityHash_ = computeIdentityHash(path, pixelSize_, fileSize_);
  fontId_ = static_cast<int>(identityHash_ | 0x80000000u);
  if (fontId_ == INVALID_FONT_ID) fontId_ = static_cast<int>(0x80000001u);
  loaded_ = true;
  clearGlyphCache();

  if (expectedFileSize != 0 && expectedFileSize != fileSize_) {
    LOG_DBG("TTFR", "TTF file size changed settings=%lu actual=%lu path=%s", static_cast<unsigned long>(expectedFileSize),
            static_cast<unsigned long>(fileSize_), path);
  }

  const auto& m = font_.metrics();
  const auto ascii = font_.metricsForCodepoint('A');
  const auto kana = font_.metricsForCodepoint(0x3042);
  const auto kanji = font_.metricsForCodepoint(0x65E5);
  LOG_INF("TTFR", "reader metrics active id=%d path=%s size=%u file=%lu hash=0x%08lX", fontId_, path_.c_str(),
          pixelSize_, static_cast<unsigned long>(fileSize_), static_cast<unsigned long>(identityHash_));
  LOG_INF("TTFR", "metrics upem=%u asc=%d desc=%d gap=%d line=%d glyphs=%u", m.unitsPerEm, m.ascender, m.descender,
          m.lineGap, lineHeightPx(), m.numGlyphs);
  LOG_INF("TTFR", "sample A present=%d glyph=%u adv=%d kana present=%d glyph=%u adv=%d kanji present=%d glyph=%u adv=%d",
          ascii.present ? 1 : 0, ascii.glyphId, ascii.advancePx, kana.present ? 1 : 0, kana.glyphId, kana.advancePx,
          kanji.present ? 1 : 0, kanji.glyphId, kanji.advancePx);
  return true;
}

bool TtfReaderMetrics::handlesFontId(const int fontId) const { return loaded_ && fontId == fontId_; }

int TtfReaderMetrics::advanceForCodepoint(const uint32_t cp, const EpdFontFamily::Style style) const {
  if (!loaded_ || utf8IsCombiningMark(cp)) return 0;
  const auto metrics = font_.metricsForCodepoint(cp);
  int advance = metrics.present ? metrics.advancePx : pixelSize_;
  if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
    advance = (advance + 1) / 2;
  }
  return std::max(0, advance);
}

int TtfReaderMetrics::getSpaceWidth(const int fontId, const EpdFontFamily::Style style) const {
  if (!handlesFontId(fontId)) return 0;
  return advanceForCodepoint(' ', style);
}

int TtfReaderMetrics::getSpaceAdvance(const int fontId, const uint32_t, const uint32_t,
                                      const EpdFontFamily::Style style) const {
  return getSpaceWidth(fontId, style);
}

int TtfReaderMetrics::getKerning(const int, const uint32_t, const uint32_t, const EpdFontFamily::Style) const {
  return 0;
}

int TtfReaderMetrics::getTextAdvanceX(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  if (!handlesFontId(fontId) || !text) return 0;

  int width = 0;
  while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
    width += advanceForCodepoint(cp, style);
  }
  return width;
}

bool TtfReaderMetrics::canRenderText(const int fontId, const char* text, const EpdFontFamily::Style) const {
  if (!handlesFontId(fontId) || !text) return false;
  while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
    if (utf8IsCombiningMark(cp)) continue;
    if (!font_.metricsForCodepoint(cp).present) return false;
  }
  return true;
}

uint32_t TtfReaderMetrics::getVerticalSubstitution(const int, const uint32_t cp, const EpdFontFamily::Style) const {
  return cp;
}

const TtfReaderMetrics::CachedGlyph* TtfReaderMetrics::glyphForCodepoint(const uint32_t cp,
                                                                         const EpdFontFamily::Style style) const {
  if (!loaded_ || utf8IsCombiningMark(cp)) return nullptr;

  for (const auto& glyph : glyphCache_) {
    if (glyph.codepoint == cp) {
      ++cacheHits_;
      return &glyph;
    }
  }

  ++cacheMisses_;
  return rasterizeAndCacheGlyph(cp, style);
}

const TtfReaderMetrics::CachedGlyph* TtfReaderMetrics::rasterizeAndCacheGlyph(
    const uint32_t cp, const EpdFontFamily::Style style) const {
  const auto metrics = font_.metricsForCodepoint(cp);
  if (!metrics.present) {
    ++missingGlyphs_;
    return nullptr;
  }

  CachedGlyph cached;
  cached.codepoint = cp;
  cached.glyphId = metrics.glyphId;
  cached.advancePx = advanceForCodepoint(cp, style);

  if (metrics.glyphLength == 0) {
    glyphCache_.push_back(std::move(cached));
    ++rasterOk_;
    return &glyphCache_.back();
  }

  PsramBuffer glyfData;
  uint32_t glyfLength = 0;
  if (!readGlyphSlice(metrics, glyfData, glyfLength)) {
    ++rasterFailed_;
    return nullptr;
  }

  uint8_t* scratchRaw =
      static_cast<uint8_t*>(heap_caps_malloc(TTF_RASTER_SCRATCH_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!scratchRaw) {
    ++rasterFailed_;
    LOG_ERR("TTFR", "Failed to allocate raster scratch for U+%04lX", static_cast<unsigned long>(cp));
    return nullptr;
  }
  PsramBuffer scratch(scratchRaw);

  const auto result = ttf::rasterizeSimpleGlyf(glyfData.get(), glyfLength, font_.metrics().unitsPerEm, pixelSize_,
                                              scratch.get(), TTF_RASTER_SCRATCH_BYTES);
  if (!result.ok) {
    if (result.compound) {
      ++compoundGlyphs_;
    } else {
      ++rasterFailed_;
    }
    LOG_DBG("TTFR", "raster skip cp=U+%04lX glyph=%u compound=%d error=%s", static_cast<unsigned long>(cp),
            metrics.glyphId, result.compound ? 1 : 0, result.error ? result.error : "unknown");
    return nullptr;
  }

  cached.width = result.width;
  cached.height = result.height;
  cached.xOffset = result.xOffset;
  cached.yOffset = result.yOffset;
  cached.bitmapBytes = result.bitmapBytes;
  if (cached.bitmapBytes > 0) {
    if (glyphCacheBytes_ + cached.bitmapBytes > TTF_GLYPH_CACHE_MAX_BYTES) {
      LOG_INF("TTFR", "glyph cache reset bytes=%u limit=%u", static_cast<unsigned>(glyphCacheBytes_),
              static_cast<unsigned>(TTF_GLYPH_CACHE_MAX_BYTES));
      clearGlyphCache();
    }
    uint8_t* bitmapRaw =
        static_cast<uint8_t*>(heap_caps_malloc(cached.bitmapBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!bitmapRaw) {
      ++rasterFailed_;
      LOG_ERR("TTFR", "Failed to allocate cached glyph bitmap cp=U+%04lX bytes=%u", static_cast<unsigned long>(cp),
              static_cast<unsigned>(cached.bitmapBytes));
      return nullptr;
    }
    cached.bitmap.reset(bitmapRaw);
    memcpy(cached.bitmap.get(), scratch.get(), cached.bitmapBytes);
    glyphCacheBytes_ += cached.bitmapBytes;
  }

  glyphCache_.push_back(std::move(cached));
  ++rasterOk_;
  return &glyphCache_.back();
}

bool TtfReaderMetrics::readGlyphSlice(const ttf::GlyphMetrics& glyph, PsramBuffer& outData, uint32_t& outLength) const {
  outData.reset();
  outLength = 0;
  if (!loaded_ || glyph.glyphLength == 0) return true;
  if (glyph.glyphOffset > glyfTableLength_ || glyph.glyphLength > glyfTableLength_ - glyph.glyphOffset) {
    return false;
  }

  uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(glyph.glyphLength, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!raw) return false;
  PsramBuffer buffer(raw);

  HalFile file = Storage.open(path_.c_str(), O_RDONLY);
  if (!file) return false;
  const uint64_t offset = static_cast<uint64_t>(glyfTableOffset_) + glyph.glyphOffset;
  if (!file.seek64(offset) || !readExact(file, buffer.get(), glyph.glyphLength)) {
    file.close();
    return false;
  }
  file.close();

  outLength = glyph.glyphLength;
  outData = std::move(buffer);
  return true;
}

void TtfReaderMetrics::drawText(const GfxRenderer& renderer, const int fontId, const int x, const int y,
                                const char* text, const bool black, const EpdFontFamily::Style style) const {
  if (!handlesFontId(fontId) || !text || *text == '\0') return;

  const int baselineY = y + ascenderPx();
  int cursorX = x;
  while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
    if (utf8IsCombiningMark(cp)) continue;
    const CachedGlyph* glyph = glyphForCodepoint(cp, style);
    if (!glyph) {
      cursorX += pixelSize_ / 2;
      continue;
    }

    if (glyph->bitmap && glyph->width > 0 && glyph->height > 0) {
      const int drawX = cursorX + glyph->xOffset;
      const int drawY = baselineY + glyph->yOffset;
      for (int gy = 0; gy < glyph->height; ++gy) {
        const uint8_t* row = glyph->bitmap.get() + static_cast<size_t>(gy) * glyph->width;
        for (int gx = 0; gx < glyph->width; ++gx) {
          if (row[gx] != 0) {
            renderer.drawPixel(drawX + gx, drawY + gy, black);
          }
        }
      }
      ++renderedGlyphs_;
    }

    cursorX += glyph->advancePx;
  }

  logRenderStats("draw");
}

void TtfReaderMetrics::clearGlyphCache() const {
  glyphCache_.clear();
  glyphCacheBytes_ = 0;
  cacheHits_ = 0;
  cacheMisses_ = 0;
  rasterOk_ = 0;
  rasterFailed_ = 0;
  compoundGlyphs_ = 0;
  missingGlyphs_ = 0;
  renderedGlyphs_ = 0;
  lastStatsLogMs_ = 0;
}

void TtfReaderMetrics::logRenderStats(const char* label) const {
  const unsigned long now = millis();
  if (lastStatsLogMs_ != 0 && now - lastStatsLogMs_ < TTF_STATS_LOG_INTERVAL_MS) return;
  lastStatsLogMs_ = now;
  LOG_INF("TTFR",
          "render stats label=%s drawn=%lu cached_glyphs=%u cache_bytes=%u hits=%lu misses=%lu raster_ok=%lu "
          "raster_fail=%lu compound=%lu missing=%lu",
          label ? label : "render", static_cast<unsigned long>(renderedGlyphs_),
          static_cast<unsigned>(glyphCache_.size()), static_cast<unsigned>(glyphCacheBytes_),
          static_cast<unsigned long>(cacheHits_), static_cast<unsigned long>(cacheMisses_),
          static_cast<unsigned long>(rasterOk_), static_cast<unsigned long>(rasterFailed_),
          static_cast<unsigned long>(compoundGlyphs_), static_cast<unsigned long>(missingGlyphs_));
}

int TtfReaderMetrics::ascenderPx() const {
  if (!loaded_) return 0;
  const auto& m = font_.metrics();
  return (static_cast<int32_t>(m.ascender) * m.pixelSize + m.unitsPerEm / 2) / m.unitsPerEm;
}

int TtfReaderMetrics::lineHeightPx() const {
  if (!loaded_) return 0;
  const auto& m = font_.metrics();
  const int32_t height = static_cast<int32_t>(m.ascender) - static_cast<int32_t>(m.descender) + m.lineGap;
  return std::max<int>(1, (height * m.pixelSize + m.unitsPerEm / 2) / m.unitsPerEm);
}

int TtfReaderMetrics::getFontAscenderSize(const int fontId) const { return handlesFontId(fontId) ? ascenderPx() : 0; }

int TtfReaderMetrics::getLineHeight(const int fontId) const { return handlesFontId(fontId) ? lineHeightPx() : 0; }

uint32_t TtfReaderMetrics::computeIdentityHash(const char* path, const uint8_t pixelSize, const uint32_t fileSize) {
  uint32_t hash = FNV_OFFSET;
  if (path) {
    for (const char* p = path; *p; ++p) {
      hash = fnvStep(hash, static_cast<uint8_t>(*p));
    }
  }
  hash = fnvStep(hash, pixelSize);
  for (int shift = 0; shift < 32; shift += 8) {
    hash = fnvStep(hash, static_cast<uint8_t>((fileSize >> shift) & 0xFF));
  }
  return hash == 0 ? 1 : hash;
}

#endif
