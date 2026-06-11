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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>

#include "CrossPointSettings.h"

namespace {

constexpr size_t TTF_HEADER_SIZE = 12;
constexpr size_t TTF_TABLE_RECORD_SIZE = 16;
constexpr size_t TTF_RASTER_SCRATCH_BYTES = 64 * 1024;
constexpr size_t TTF_GLYPH_CACHE_MAX_BYTES = 512 * 1024;
constexpr uint32_t TTF_STATS_LOG_INTERVAL_MS = 3000;
constexpr uint8_t TTF_RASTER_SUPERSAMPLE = 2;
constexpr uint32_t FNV_OFFSET = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

#ifdef CROSSPOINT_TTF_USE_OPENFONTRENDER
struct OpenFontRenderFileHandle {
  HalFile file;
};

std::list<OpenFontRenderFileHandle> ofrFiles;

struct OpenFontRenderCanvas {
  uint8_t* pixels = nullptr;
  int width = 0;
  int height = 0;
  int minX = INT_MAX;
  int minY = INT_MAX;
  int maxX = INT_MIN;
  int maxY = INT_MIN;

  void putPixel(const int x, const int y, const uint8_t coverage) {
    if (!pixels || x < 0 || y < 0 || x >= width || y >= height || coverage == 0) return;
    uint8_t& pixel = pixels[static_cast<size_t>(y) * width + x];
    if (coverage > pixel) pixel = coverage;
    minX = std::min(minX, x);
    minY = std::min(minY, y);
    maxX = std::max(maxX, x);
    maxY = std::max(maxY, y);
  }
};
#endif

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

bool usesTuckedHorizontalAdvance(const uint32_t cp) {
  switch (cp) {
    case 0x002C:  // ,
    case 0x002E:  // .
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0xFF0C:
    case 0xFF0E:
      return true;
    default:
      return false;
  }
}

bool isVerticalColonPresentationForm(const uint32_t cp) { return cp == 0xFE13 || cp == 0xFE14; }

uint32_t standardVerticalPresentationForm(const uint32_t cp) {
  switch (cp) {
    case 0x002C:
    case 0xFF0C:
      return 0xFE10;
    case 0x3001:
      return 0xFE11;
    case 0x002E:
    case 0x3002:
    case 0xFF0E:
      return 0xFE12;
    case 0x003A:
    case 0xFF1A:
      return 0xFE13;
    case 0x003B:
    case 0xFF1B:
      return 0xFE14;
    case 0x0021:
    case 0xFF01:
      return 0xFE15;
    case 0x003F:
    case 0xFF1F:
      return 0xFE16;
    case 0x3016:
      return 0xFE17;
    case 0x3017:
      return 0xFE18;
    case 0x002D:
    case 0x2010:
    case 0x2011:
    case 0xFF0D:
      return 0xFE63;
    case 0x2013:
      return 0xFE32;
    case 0x2014:
    case 0x2015:
      return 0xFE31;
    case 0x2025:
      return 0xFE30;
    case 0x2026:
      return 0xFE19;
    case 0x0028:
    case 0xFF08:
      return 0xFE35;
    case 0x0029:
    case 0xFF09:
      return 0xFE36;
    case 0x007B:
    case 0xFF5B:
      return 0xFE37;
    case 0x007D:
    case 0xFF5D:
      return 0xFE38;
    case 0x3014:
      return 0xFE39;
    case 0x3015:
      return 0xFE3A;
    case 0x3010:
      return 0xFE3B;
    case 0x3011:
      return 0xFE3C;
    case 0x300A:
      return 0xFE3D;
    case 0x300B:
      return 0xFE3E;
    case 0x3008:
      return 0xFE3F;
    case 0x3009:
      return 0xFE40;
    case 0x300C:
      return 0xFE41;
    case 0x300D:
      return 0xFE42;
    case 0x300E:
      return 0xFE43;
    case 0x300F:
      return 0xFE44;
    case 0x005B:
    case 0xFF3B:
      return 0xFE47;
    case 0x005D:
    case 0xFF3D:
      return 0xFE48;
    case 0x2500:
      return 0x2502;
    case 0x2501:
      return 0x2503;
    default:
      return cp;
  }
}

template <typename T>
T positiveDelta(const T current, const T previous) {
  return current >= previous ? current - previous : current;
}

uint8_t coverageToPackedGrayValue(const uint8_t coverage) {
  // Match GfxRenderer's packed 2-bit font convention after inversion:
  // 0=black, 1=dark gray, 2=light gray, 3=white.
  const uint8_t level = std::min<uint8_t>(3, (coverage + 42) / 85);
  return static_cast<uint8_t>(3 - level);
}

bool shouldDrawCoveragePixel(const GfxRenderer::RenderMode renderMode, const uint8_t coverage) {
  const uint8_t val = coverageToPackedGrayValue(coverage);
  if (renderMode == GfxRenderer::BW) return val < 3;
  if (renderMode == GfxRenderer::GRAYSCALE_MSB) return val == 1 || val == 2;
  if (renderMode == GfxRenderer::GRAYSCALE_LSB) return val == 1;
  return false;
}

#ifdef CROSSPOINT_TTF_USE_OPENFONTRENDER
uint8_t rgb565ToCoverage(const uint16_t color) {
  const uint16_t r5 = (color >> 11) & 0x1F;
  const uint16_t g6 = (color >> 5) & 0x3F;
  const uint16_t b5 = color & 0x1F;
  const uint16_t r = (r5 * 255 + 15) / 31;
  const uint16_t g = (g6 * 255 + 31) / 63;
  const uint16_t b = (b5 * 255 + 15) / 31;
  const uint16_t luma = static_cast<uint16_t>((r * 77 + g * 150 + b * 29) >> 8);
  return static_cast<uint8_t>(255 - std::min<uint16_t>(255, luma));
}

uint8_t quantizeCoverage(const uint8_t coverage) {
  return static_cast<uint8_t>(std::min<uint8_t>(3, (coverage + 42) / 85) * 85);
}
#endif

bool downsample2BitCoverage(const uint8_t* highBitmap, const ttf::CustomRasterResult& highRaster, uint8_t* outBitmap,
                            const int outWidth, const int outHeight) {
  if (!highBitmap || !highRaster.ok || !outBitmap || outWidth <= 0 || outHeight <= 0) return false;

  for (int y = 0; y < outHeight; ++y) {
    for (int x = 0; x < outWidth; ++x) {
      int coverage = 0;
      int samples = 0;
      for (int sy = 0; sy < TTF_RASTER_SUPERSAMPLE; ++sy) {
        const int hy = y * TTF_RASTER_SUPERSAMPLE + sy;
        if (hy >= highRaster.height) continue;
        for (int sx = 0; sx < TTF_RASTER_SUPERSAMPLE; ++sx) {
          const int hx = x * TTF_RASTER_SUPERSAMPLE + sx;
          if (hx >= highRaster.width) continue;
          ++samples;
          if (highBitmap[static_cast<size_t>(hy) * highRaster.width + hx] != 0) ++coverage;
        }
      }
      const uint8_t level =
          samples == 0 ? 0 : static_cast<uint8_t>(std::min(3, (coverage * 3 + samples / 2) / samples));
      outBitmap[static_cast<size_t>(y) * outWidth + x] = static_cast<uint8_t>(level * 85);
    }
  }
  return true;
}

}  // namespace

#ifdef CROSSPOINT_TTF_USE_OPENFONTRENDER
void OFR_fclose(FT_FILE* stream) {
  if (!stream) return;
  for (auto it = ofrFiles.begin(); it != ofrFiles.end(); ++it) {
    if (static_cast<void*>(&(*it)) == stream) {
      it->file.close();
      ofrFiles.erase(it);
      return;
    }
  }
}

FT_FILE* OFR_fopen(const char* filename, const char*) {
  if (!filename || filename[0] == '\0') return nullptr;
  HalFile file = Storage.open(filename, O_RDONLY);
  if (!file) return nullptr;
  ofrFiles.push_back({std::move(file)});
  return static_cast<FT_FILE*>(&ofrFiles.back());
}

size_t OFR_fread(void* ptr, const size_t size, const size_t nmemb, FT_FILE* stream) {
  if (!ptr || !stream || size == 0 || nmemb == 0) return 0;
  auto* handle = static_cast<OpenFontRenderFileHandle*>(stream);
  static uint32_t readCount = 0;
  if ((++readCount & 0x3F) == 0) yield();
  const int got = handle->file.read(ptr, size * nmemb);
  return got <= 0 ? 0 : static_cast<size_t>(got);
}

int OFR_fseek(FT_FILE* stream, const long int offset, const int whence) {
  if (!stream) return -1;
  auto* handle = static_cast<OpenFontRenderFileHandle*>(stream);
  if (whence == SEEK_SET) return handle->file.seek64(static_cast<uint64_t>(std::max<long int>(0, offset))) ? 0 : -1;
  if (whence == SEEK_CUR) return handle->file.seekCur(offset) ? 0 : -1;
  if (whence == SEEK_END) {
    const int64_t target = static_cast<int64_t>(handle->file.fileSize64()) + offset;
    if (target < 0) return -1;
    return handle->file.seek64(static_cast<uint64_t>(target)) ? 0 : -1;
  }
  return -1;
}

long int OFR_ftell(FT_FILE* stream) {
  if (!stream) return -1;
  auto* handle = static_cast<OpenFontRenderFileHandle*>(stream);
  return static_cast<long int>(handle->file.position());
}
#endif

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
#ifdef CROSSPOINT_TTF_USE_OPENFONTRENDER
  if (openFontRenderLoaded_) {
    openFontRender_.unloadFont();
    openFontRenderLoaded_ = false;
  }
  ofrFiles.clear();
#endif
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

#ifdef CROSSPOINT_TTF_USE_OPENFONTRENDER
  openFontRender_.setUseRenderTask(false);
  openFontRender_.setFontSize(pixelSize_);
  openFontRender_.setFontColor(0x0000, 0xFFFF);
  openFontRender_.setBackgroundFillMethod(BgFillMethod::None);
  openFontRender_.setAlignment(Align::TopLeft);
  openFontRender_.setLayout(Layout::Horizontal);
  openFontRender_.setCacheSize(1, 4, 768 * 1024);
  const auto ofrError = openFontRender_.loadFont(path);
  if (ofrError != 0) {
    LOG_ERR("TTFR", "OpenFontRender load failed path=%s error=%d", path, static_cast<int>(ofrError));
    unload();
    return false;
  }
  openFontRenderLoaded_ = true;
#endif

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
#ifdef CROSSPOINT_TTF_USE_OPENFONTRENDER
  LOG_INF("TTFR", "OpenFontRender rasterizer active size=%u cache_limit=%u", pixelSize_, 768 * 1024);
  logOpenFontRenderMetricSamples();
#endif
  LOG_INF("TTFR", "metrics upem=%u asc=%d desc=%d gap=%d line=%d glyphs=%u", m.unitsPerEm, m.ascender, m.descender,
          m.lineGap, lineHeightPx(), m.numGlyphs);
  LOG_INF("TTFR", "sample A present=%d glyph=%u adv=%d kana present=%d glyph=%u adv=%d kanji present=%d glyph=%u adv=%d",
          ascii.present ? 1 : 0, ascii.glyphId, ascii.advancePx, kana.present ? 1 : 0, kana.glyphId, kana.advancePx,
          kanji.present ? 1 : 0, kanji.glyphId, kanji.advancePx);
  return true;
}

bool TtfReaderMetrics::handlesFontId(const int fontId) const { return loaded_ && fontId == fontId_; }

#ifdef CROSSPOINT_TTF_USE_OPENFONTRENDER
void TtfReaderMetrics::logOpenFontRenderMetricSamples() const {
  if (!loaded_ || !openFontRenderLoaded_) return;

  struct Sample {
    const char* label;
    const char* text;
  };

  static constexpr Sample samples[] = {
      {"ascii_A", "A"},
      {"kana_a", "\xE3\x81\x82"},
      {"kanji_day", "\xE6\x97\xA5"},
      {"fullwidth_punct", "\xE3\x80\x81"},
      {"mixed", "A\xE3\x81\x82\xE6\x97\xA5"},
  };

  openFontRender_.setFontSize(pixelSize_);
  openFontRender_.setAlignment(Align::TopLeft);
  openFontRender_.setLayout(Layout::Horizontal);

  for (const auto& sample : samples) {
    const int metricsAdvance = getTextAdvanceX(fontId_, sample.text, EpdFontFamily::REGULAR);
    const int ofrWidth = static_cast<int>(openFontRender_.getTextWidth(sample.text));
    LOG_INF("TTFR", "metric compare %s metrics_adv=%d ofr_width=%d delta=%d", sample.label, metricsAdvance, ofrWidth,
            ofrWidth - metricsAdvance);
  }
}
#endif

#ifdef CROSSPOINT_TTF_PROBE
void TtfReaderMetrics::probeRasterText(const char* text) const {
  if (!loaded_ || !text) return;
  uint32_t glyphs = 0;
  uint32_t missing = 0;
  while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
    if (utf8IsCombiningMark(cp)) continue;
    const CachedGlyph* glyph = glyphForCodepoint(cp, EpdFontFamily::REGULAR);
    if (glyph) {
      ++glyphs;
    } else {
      ++missing;
    }
  }
  LOG_INF("TTFR", "probe provider raster text glyphs=%lu missing=%lu font_id=%d cache=%u bytes=%u",
          static_cast<unsigned long>(glyphs), static_cast<unsigned long>(missing), fontId_,
          static_cast<unsigned>(glyphCache_.size()), static_cast<unsigned>(glyphCacheBytes_));
  logRenderStats("probe");
}
#endif

int TtfReaderMetrics::advanceForCodepoint(const uint32_t cp, const EpdFontFamily::Style style) const {
  if (!loaded_ || utf8IsCombiningMark(cp)) return 0;
  const auto metrics = font_.metricsForCodepoint(cp);
  int advance = metrics.present ? metrics.advancePx : pixelSize_;
  if (metrics.present && usesTuckedHorizontalAdvance(cp)) {
    advance = std::max(1, std::min(advance, pixelSize_ / 2));
  }
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
  if (!loaded_) return cp;
  const uint32_t presentationForm = standardVerticalPresentationForm(cp);
  if (presentationForm != cp && font_.metricsForCodepoint(presentationForm).present) {
    return presentationForm;
  }
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

const TtfReaderMetrics::CachedGlyph* TtfReaderMetrics::rasterizeAndCacheGlyph(const uint32_t cp,
                                                                              const EpdFontFamily::Style style) const {
  const auto metrics = font_.metricsForCodepoint(cp);
  if (!metrics.present) {
    ++missingGlyphs_;
    return nullptr;
  }

#ifdef CROSSPOINT_TTF_USE_OPENFONTRENDER
  if (openFontRenderLoaded_) {
    return rasterizeAndCacheGlyphWithOpenFontRender(cp, style, metrics);
  }
#endif

  return rasterizeAndCacheGlyphWithCustomRasterizer(cp, style, metrics);
}

const TtfReaderMetrics::CachedGlyph* TtfReaderMetrics::rasterizeAndCacheGlyphWithOpenFontRender(
    const uint32_t cp, const EpdFontFamily::Style style, const ttf::GlyphMetrics& metrics) const {
#ifdef CROSSPOINT_TTF_USE_OPENFONTRENDER
  CachedGlyph cached;
  cached.codepoint = cp;
  cached.glyphId = metrics.glyphId;
  cached.advancePx = advanceForCodepoint(cp, style);

  if (metrics.glyphLength == 0) {
    glyphCache_.push_back(std::move(cached));
    ++rasterOk_;
    return &glyphCache_.back();
  }

  const int canvasWidth = std::max<int>(pixelSize_ * 4, cached.advancePx + pixelSize_ * 3);
  const int canvasHeight = std::max<int>(lineHeightPx() + pixelSize_ * 2, pixelSize_ * 4);
  const size_t canvasBytes = static_cast<size_t>(canvasWidth) * canvasHeight;
  uint8_t* canvasRaw = static_cast<uint8_t*>(heap_caps_calloc(canvasBytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!canvasRaw) {
    ++rasterFailed_;
    LOG_ERR("TTFR", "Failed to allocate OpenFontRender canvas cp=U+%04lX bytes=%u", static_cast<unsigned long>(cp),
            static_cast<unsigned>(canvasBytes));
    return nullptr;
  }
  PsramBuffer canvasHolder(canvasRaw);
  OpenFontRenderCanvas canvas{canvasRaw, canvasWidth, canvasHeight};

  const int originX = pixelSize_;
  std::string text;
  utf8AppendCodepoint(cp, text);

  openFontRender_.setFontSize(pixelSize_);
  openFontRender_.setBackgroundFillMethod(BgFillMethod::None);
  openFontRender_.setAlignment(Align::TopLeft);
  openFontRender_.setLayout(Layout::Horizontal);
  openFontRender_.set_drawPixel([&canvas](int32_t x, int32_t y, uint16_t color) {
    canvas.putPixel(x, y, quantizeCoverage(rgb565ToCoverage(color)));
  });
  openFontRender_.set_drawFastHLine([&canvas](int32_t x, int32_t y, int32_t w, uint16_t color) {
    const uint8_t coverage = quantizeCoverage(rgb565ToCoverage(color));
    for (int32_t i = 0; i < w; ++i) canvas.putPixel(x + i, y, coverage);
  });
  openFontRender_.set_startWrite([]() {});
  openFontRender_.set_endWrite([]() {});

  const uint32_t rasterStartUs = micros();
  const uint16_t chars = openFontRender_.drawString(text.c_str(), originX, 0, 0x0000, 0xFFFF, Layout::Horizontal);
  rasterTimeUs_ += micros() - rasterStartUs;
  if (chars == 0 || canvas.minX > canvas.maxX || canvas.minY > canvas.maxY) {
    ++rasterFailed_;
    LOG_DBG("TTFR", "OpenFontRender empty glyph cp=U+%04lX glyph=%u chars=%u", static_cast<unsigned long>(cp),
            metrics.glyphId, chars);
    return nullptr;
  }

  cached.width = canvas.maxX - canvas.minX + 1;
  cached.height = canvas.maxY - canvas.minY + 1;
  const int ofrXOffset = canvas.minX - originX;
  cached.xOffset = std::max(ofrXOffset, static_cast<int>(metrics.leftSideBearingPx));
  if (usesTuckedHorizontalAdvance(cp)) {
    cached.xOffset = 0;
  } else if (isVerticalColonPresentationForm(cp)) {
    cached.xOffset = std::max(0, (cached.advancePx - cached.width) / 2);
  }
  cached.yOffset = canvas.minY - ascenderPx();
  cached.bitmapBytes = static_cast<size_t>(cached.width) * cached.height;
  if (cached.bitmapBytes > 0) {
    if (glyphCacheBytes_ + cached.bitmapBytes > TTF_GLYPH_CACHE_MAX_BYTES) {
      LOG_INF("TTFR", "glyph cache reset bytes=%u limit=%u", static_cast<unsigned>(glyphCacheBytes_),
              static_cast<unsigned>(TTF_GLYPH_CACHE_MAX_BYTES));
      clearGlyphCache();
      ++cacheResets_;
    }
    uint8_t* bitmapRaw =
        static_cast<uint8_t*>(heap_caps_malloc(cached.bitmapBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!bitmapRaw) {
      ++rasterFailed_;
      LOG_ERR("TTFR", "Failed to allocate OpenFontRender glyph cp=U+%04lX bytes=%u", static_cast<unsigned long>(cp),
              static_cast<unsigned>(cached.bitmapBytes));
      return nullptr;
    }
    cached.bitmap.reset(bitmapRaw);
    for (int y = 0; y < cached.height; ++y) {
      memcpy(cached.bitmap.get() + static_cast<size_t>(y) * cached.width,
             canvas.pixels + static_cast<size_t>(canvas.minY + y) * canvas.width + canvas.minX, cached.width);
    }
    glyphCacheBytes_ += cached.bitmapBytes;
  }

  glyphCache_.push_back(std::move(cached));
  ++rasterOk_;
  return &glyphCache_.back();
#else
  (void)cp;
  (void)style;
  (void)metrics;
  return nullptr;
#endif
}

const TtfReaderMetrics::CachedGlyph* TtfReaderMetrics::rasterizeAndCacheGlyphWithCustomRasterizer(
    const uint32_t cp, const EpdFontFamily::Style style, const ttf::GlyphMetrics& metrics) const {

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

  const uint16_t highPixelSize = static_cast<uint16_t>(std::min<uint32_t>(
      UINT16_MAX, static_cast<uint32_t>(pixelSize_) * static_cast<uint32_t>(TTF_RASTER_SUPERSAMPLE)));
  const uint32_t rasterStartUs = micros();
  const auto result = ttf::rasterizeSimpleGlyfWithFillRule(glyfData.get(), glyfLength, font_.metrics().unitsPerEm,
                                                           highPixelSize, ttf::CustomFillRule::NonZero, scratch.get(),
                                                           TTF_RASTER_SCRATCH_BYTES);
  rasterTimeUs_ += micros() - rasterStartUs;
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

  cached.width = (result.width + TTF_RASTER_SUPERSAMPLE - 1) / TTF_RASTER_SUPERSAMPLE;
  cached.height = (result.height + TTF_RASTER_SUPERSAMPLE - 1) / TTF_RASTER_SUPERSAMPLE;
  cached.xOffset = result.xOffset / TTF_RASTER_SUPERSAMPLE;
  cached.yOffset = result.yOffset / TTF_RASTER_SUPERSAMPLE;
  cached.bitmapBytes = static_cast<size_t>(cached.width) * cached.height;
  if (cached.bitmapBytes > 0) {
    if (glyphCacheBytes_ + cached.bitmapBytes > TTF_GLYPH_CACHE_MAX_BYTES) {
      LOG_INF("TTFR", "glyph cache reset bytes=%u limit=%u", static_cast<unsigned>(glyphCacheBytes_),
              static_cast<unsigned>(TTF_GLYPH_CACHE_MAX_BYTES));
      clearGlyphCache();
      ++cacheResets_;
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
    const uint32_t downsampleStartUs = micros();
    const bool downsampleOk = downsample2BitCoverage(scratch.get(), result, cached.bitmap.get(), cached.width,
                                                     cached.height);
    downsampleTimeUs_ += micros() - downsampleStartUs;
    if (!downsampleOk) {
      ++rasterFailed_;
      LOG_ERR("TTFR", "Failed to downsample raster cp=U+%04lX", static_cast<unsigned long>(cp));
      return nullptr;
    }
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

  const GfxRenderer::RenderMode renderMode = renderer.getRenderMode();
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
      if (!renderer.glyphIntersectsStrip(drawX, drawY, drawX + glyph->width - 1, drawY + glyph->height - 1)) {
        cursorX += glyph->advancePx;
        continue;
      }
      for (int gy = 0; gy < glyph->height; ++gy) {
        const uint8_t* row = glyph->bitmap.get() + static_cast<size_t>(gy) * glyph->width;
        for (int gx = 0; gx < glyph->width; ++gx) {
          if (shouldDrawCoveragePixel(renderMode, row[gx])) {
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

void TtfReaderMetrics::drawTextRotated90CW(const GfxRenderer& renderer, const int fontId, const int x, const int y,
                                           const char* text, const bool black,
                                           const EpdFontFamily::Style style) const {
  if (!handlesFontId(fontId) || !text || *text == '\0') return;

  const GfxRenderer::RenderMode renderMode = renderer.getRenderMode();
  int cursorY = y;
  int prevAdvance = 0;
  while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
    if (utf8IsCombiningMark(cp)) continue;
    if (prevAdvance > 0) cursorY -= prevAdvance;

    const CachedGlyph* glyph = glyphForCodepoint(cp, style);
    if (!glyph) {
      prevAdvance = pixelSize_ / 2;
      continue;
    }

    if (glyph->bitmap && glyph->width > 0 && glyph->height > 0) {
      const int drawX0 = x + ascenderPx() + glyph->yOffset;
      const int drawX1 = drawX0 + glyph->height - 1;
      const int drawY0 = cursorY - glyph->xOffset - glyph->width + 1;
      const int drawY1 = cursorY - glyph->xOffset;
      if (!renderer.glyphIntersectsStrip(drawX0, drawY0, drawX1, drawY1)) {
        prevAdvance = glyph->advancePx;
        continue;
      }

      for (int gy = 0; gy < glyph->height; ++gy) {
        const uint8_t* row = glyph->bitmap.get() + static_cast<size_t>(gy) * glyph->width;
        const int screenX = drawX0 + gy;
        for (int gx = 0; gx < glyph->width; ++gx) {
          if (shouldDrawCoveragePixel(renderMode, row[gx])) {
            renderer.drawPixel(screenX, cursorY - glyph->xOffset - gx, black);
          }
        }
      }
      ++renderedGlyphs_;
    }

    prevAdvance = glyph->advancePx;
  }

  logRenderStats("draw_rotated");
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
  cacheResets_ = 0;
  rasterTimeUs_ = 0;
  downsampleTimeUs_ = 0;
  lastLoggedRasterOk_ = 0;
  lastLoggedCacheMisses_ = 0;
  lastLoggedRenderedGlyphs_ = 0;
  lastLoggedRasterTimeUs_ = 0;
  lastStatsLogMs_ = 0;
}

void TtfReaderMetrics::logRenderStats(const char* label) const {
  const unsigned long now = millis();
  if (lastStatsLogMs_ != 0 && now - lastStatsLogMs_ < TTF_STATS_LOG_INTERVAL_MS) return;
  lastStatsLogMs_ = now;
  const uint32_t deltaRasterOk = positiveDelta(rasterOk_, lastLoggedRasterOk_);
  const uint32_t deltaCacheMisses = positiveDelta(cacheMisses_, lastLoggedCacheMisses_);
  const uint32_t deltaRenderedGlyphs = positiveDelta(renderedGlyphs_, lastLoggedRenderedGlyphs_);
  const uint64_t deltaRasterUs = positiveDelta(rasterTimeUs_, lastLoggedRasterTimeUs_);
  const uint32_t avgRasterUs = rasterOk_ > 0 ? static_cast<uint32_t>(rasterTimeUs_ / rasterOk_) : 0;
  const uint32_t avgDeltaRasterUs = deltaRasterOk > 0 ? static_cast<uint32_t>(deltaRasterUs / deltaRasterOk) : 0;
  LOG_INF("TTFR",
          "render stats label=%s drawn=%lu cached_glyphs=%u cache_bytes=%u hits=%lu misses=%lu raster_ok=%lu "
          "raster_fail=%lu compound=%lu missing=%lu cache_resets=%lu avg_raster_us=%lu "
          "delta_drawn=%lu delta_misses=%lu delta_raster=%lu delta_avg_raster_us=%lu",
          label ? label : "render", static_cast<unsigned long>(renderedGlyphs_),
          static_cast<unsigned>(glyphCache_.size()), static_cast<unsigned>(glyphCacheBytes_),
          static_cast<unsigned long>(cacheHits_), static_cast<unsigned long>(cacheMisses_),
          static_cast<unsigned long>(rasterOk_), static_cast<unsigned long>(rasterFailed_),
          static_cast<unsigned long>(compoundGlyphs_), static_cast<unsigned long>(missingGlyphs_),
          static_cast<unsigned long>(cacheResets_), static_cast<unsigned long>(avgRasterUs),
          static_cast<unsigned long>(deltaRenderedGlyphs), static_cast<unsigned long>(deltaCacheMisses),
          static_cast<unsigned long>(deltaRasterOk), static_cast<unsigned long>(avgDeltaRasterUs));
  lastLoggedRasterOk_ = rasterOk_;
  lastLoggedCacheMisses_ = cacheMisses_;
  lastLoggedRenderedGlyphs_ = renderedGlyphs_;
  lastLoggedRasterTimeUs_ = rasterTimeUs_;
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
