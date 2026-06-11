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

namespace {

constexpr size_t TTF_HEADER_SIZE = 12;
constexpr size_t TTF_TABLE_RECORD_SIZE = 16;
constexpr size_t TTF_RASTER_SCRATCH_BYTES = 64 * 1024;
constexpr size_t TTF_GLYPH_CACHE_MAX_BYTES = 768 * 1024;
constexpr uint32_t TTF_STATS_LOG_INTERVAL_MS = 3000;
constexpr const char* TTF_GLYPH_CACHE_DIR = "/.crosspoint/ttf_cache";
constexpr uint16_t TTF_GLYPH_SIDECAR_VERSION = 1;
constexpr uint32_t TTF_GLYPH_SIDECAR_SAVE_INTERVAL_MS = 60000;
constexpr uint32_t TTF_GLYPH_SIDECAR_SAVE_DIRTY_GLYPHS = 128;
constexpr size_t TTF_GLYPH_SIDECAR_SAVE_DIRTY_BYTES = 64 * 1024;
constexpr uint8_t TTF_RASTER_SUPERSAMPLE = 2;

EpdFontFamily::Style firstStyleFromMask(const uint8_t styleMask) {
  for (uint8_t i = 0; i < 4; ++i) {
    if (styleMask & (1 << i)) return static_cast<EpdFontFamily::Style>(i);
  }
  return EpdFontFamily::REGULAR;
}

#ifdef CROSSPOINT_TTF_USE_OPENFONTRENDER
struct OpenFontRenderFileHandle {
  HalFile file;
};

std::list<OpenFontRenderFileHandle> ofrFiles;
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

bool readU16LE(HalFile& file, uint16_t& out) {
  uint8_t b[2] = {};
  if (!readExact(file, b, sizeof(b))) return false;
  out = static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
  return true;
}

bool readI16LE(HalFile& file, int16_t& out) {
  uint16_t raw = 0;
  if (!readU16LE(file, raw)) return false;
  out = static_cast<int16_t>(raw);
  return true;
}

bool readU32LE(HalFile& file, uint32_t& out) {
  uint8_t b[4] = {};
  if (!readExact(file, b, sizeof(b))) return false;
  out = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
        (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
  return true;
}

bool writeExact(HalFile& file, const void* src, const size_t len) { return file.write(src, len) == len; }

bool writeU16LE(HalFile& file, const uint16_t value) {
  const uint8_t b[2] = {static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF)};
  return writeExact(file, b, sizeof(b));
}

bool writeI16LE(HalFile& file, const int16_t value) {
  return writeU16LE(file, static_cast<uint16_t>(value));
}

bool writeU32LE(HalFile& file, const uint32_t value) {
  const uint8_t b[4] = {static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF),
                        static_cast<uint8_t>((value >> 16) & 0xFF), static_cast<uint8_t>((value >> 24) & 0xFF)};
  return writeExact(file, b, sizeof(b));
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
    case 0x30FC:
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

size_t packedGlyphBytes(const int width, const int height) {
  if (width <= 0 || height <= 0) return 0;
  return (static_cast<size_t>(width) * height + 3) / 4;
}

bool fitsInt16(const int value) { return value >= INT16_MIN && value <= INT16_MAX; }

void setPackedGlyphPixel(uint8_t* bitmap, const size_t pixelIndex, const uint8_t value) {
  const size_t byteIndex = pixelIndex >> 2;
  const uint8_t shift = static_cast<uint8_t>((3 - (pixelIndex & 0x03)) * 2);
  bitmap[byteIndex] = static_cast<uint8_t>((bitmap[byteIndex] & ~(0x03 << shift)) | ((value & 0x03) << shift));
}

uint8_t getPackedGlyphPixel(const uint8_t* bitmap, const size_t pixelIndex) {
  const size_t byteIndex = pixelIndex >> 2;
  const uint8_t shift = static_cast<uint8_t>((3 - (pixelIndex & 0x03)) * 2);
  return static_cast<uint8_t>((bitmap[byteIndex] >> shift) & 0x03);
}

bool shouldDrawPackedPixel(const GfxRenderer::RenderMode renderMode, const uint8_t val) {
  if (renderMode == GfxRenderer::BW) {
    return val < 3;
  }
  if (renderMode == GfxRenderer::GRAYSCALE_MSB) return val == 1 || val == 2;
  if (renderMode == GfxRenderer::GRAYSCALE_LSB) return val == 1;
  return false;
}

#ifdef CROSSPOINT_TTF_USE_OPENFONTRENDER
uint8_t quantizeCoverage(const uint8_t coverage) {
  // Match cpfont conversion buckets: white, light gray, dark gray, black.
  if (coverage < 64) return 0;
  if (coverage < 128) return 85;
  if (coverage < 192) return 170;
  return 255;
}
#endif

bool downsample2BitCoverage(const uint8_t* highBitmap, const ttf::CustomRasterResult& highRaster, uint8_t* outBitmap,
                            const int outWidth, const int outHeight) {
  if (!highBitmap || !highRaster.ok || !outBitmap || outWidth <= 0 || outHeight <= 0) return false;

  memset(outBitmap, 0xFF, packedGlyphBytes(outWidth, outHeight));
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
      setPackedGlyphPixel(outBitmap, static_cast<size_t>(y) * outWidth + x, static_cast<uint8_t>(3 - level));
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
  const ReaderFontConfig config = ReaderFontResolver::resolveGlobal();
  return ensureLoaded(config);
}

bool TtfReaderMetrics::ensureLoaded(const ReaderFontConfig& config) {
  if (!config.isTtf() || config.ttfPath.empty()) {
    unload();
    return false;
  }

  const bool loaded = ensureLoaded(config.ttfPath.c_str(), config.identity.pixelSize, config.identity.fileSize);
  if (loaded) activeConfig_ = config;
  return loaded;
}

bool TtfReaderMetrics::ensureLoaded(const char* path, const uint8_t pixelSize, const uint32_t expectedFileSize) {
  if (!path || path[0] == '\0') {
    unload();
    return false;
  }

  const uint8_t size = std::max<uint8_t>(12, std::min<uint8_t>(pixelSize, 72));
  if (loaded_ && path_ == path && pixelSize_ == size && (expectedFileSize == 0 || fileSize_ == expectedFileSize)) {
    return true;
  }

  return loadFromPath(path, size, expectedFileSize);
}

bool TtfReaderMetrics::flushGlyphSidecarCache() const { return flushPersistentCache(); }

bool TtfReaderMetrics::flushPersistentCache() const { return saveGlyphSidecarCache(); }

ReaderFontCacheStats TtfReaderMetrics::cacheStats() const {
  ReaderFontCacheStats stats;
  stats.glyphCount = glyphCache_.size();
  stats.bytes = glyphCacheBytes_;
  stats.byteLimit = TTF_GLYPH_CACHE_MAX_BYTES;
  stats.hits = cacheHits_;
  stats.misses = cacheMisses_;
  stats.rasterOk = rasterOk_;
  stats.rasterFailed = rasterFailed_;
  stats.missingGlyphs = missingGlyphs_;
  stats.evictions = cacheEvictions_;
  stats.persistentDirty = glyphSidecarDirty_;
  return stats;
}

void TtfReaderMetrics::unload() {
  saveGlyphSidecarCache();
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
  activeConfig_ = ReaderFontConfig{};
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
  loadGlyphSidecarCache();

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
  LOG_INF("TTFR", "OpenFontRender rasterizer active size=%u cache_limit=%u", pixelSize_,
          static_cast<unsigned>(TTF_GLYPH_CACHE_MAX_BYTES));
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

void TtfReaderMetrics::prewarmText(const int fontId, const char* utf8Text, const uint8_t styleMask) const {
  if (!handlesFontId(fontId) || !utf8Text || *utf8Text == '\0') return;

  const EpdFontFamily::Style style = firstStyleFromMask(styleMask);
  std::vector<uint32_t> uniqueCodepoints;
  uniqueCodepoints.reserve(128);
  uint32_t scannedGlyphs = 0;
  const char* cursor = utf8Text;
  while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&cursor))) {
    if (utf8IsCombiningMark(cp)) continue;
    ++scannedGlyphs;
    if (std::find(uniqueCodepoints.begin(), uniqueCodepoints.end(), cp) == uniqueCodepoints.end()) {
      uniqueCodepoints.push_back(cp);
    }
  }
  if (uniqueCodepoints.empty()) return;

  const uint32_t startMs = millis();
  const size_t cacheStart = glyphCache_.size();
  const size_t cacheBytesStart = glyphCacheBytes_;
  const uint32_t hitsStart = cacheHits_;
  const uint32_t missesStart = cacheMisses_;
  const uint32_t rasterOkStart = rasterOk_;
  const uint32_t rasterFailedStart = rasterFailed_;
  const uint32_t missingStart = missingGlyphs_;
  const uint32_t evictionsStart = cacheEvictions_;
  const uint64_t rasterUsStart = rasterTimeUs_;

  for (const uint32_t cp : uniqueCodepoints) {
    glyphForCodepoint(cp, style);
  }

  const uint32_t rasterDelta = positiveDelta(rasterOk_, rasterOkStart);
  const uint64_t rasterUsDelta = positiveDelta(rasterTimeUs_, rasterUsStart);
  const uint32_t avgRasterUs = rasterDelta > 0 ? static_cast<uint32_t>(rasterUsDelta / rasterDelta) : 0;
  const uint32_t cachePct =
      static_cast<uint32_t>((glyphCacheBytes_ * 100 + TTF_GLYPH_CACHE_MAX_BYTES / 2) / TTF_GLYPH_CACHE_MAX_BYTES);
  const bool sidecarSaved = maybeSaveGlyphSidecarCache();
  LOG_INF("TTFR",
          "prewarm font_id=%d scanned=%lu unique=%u cache=%u->%u bytes=%u->%u limit=%u pct=%lu "
          "hits_delta=%lu misses_delta=%lu raster_delta=%lu raster_fail_delta=%lu missing_delta=%lu "
          "evict_delta=%lu evictions=%lu raster_us=%llu avg_raster_us=%lu sidecar_saved=%d "
          "sidecar_dirty=%lu sidecar_dirty_bytes=%u elapsed_ms=%lu",
          fontId_, static_cast<unsigned long>(scannedGlyphs), static_cast<unsigned>(uniqueCodepoints.size()),
          static_cast<unsigned>(cacheStart), static_cast<unsigned>(glyphCache_.size()),
          static_cast<unsigned>(cacheBytesStart), static_cast<unsigned>(glyphCacheBytes_),
          static_cast<unsigned>(TTF_GLYPH_CACHE_MAX_BYTES), static_cast<unsigned long>(cachePct),
          static_cast<unsigned long>(positiveDelta(cacheHits_, hitsStart)),
          static_cast<unsigned long>(positiveDelta(cacheMisses_, missesStart)), static_cast<unsigned long>(rasterDelta),
          static_cast<unsigned long>(positiveDelta(rasterFailed_, rasterFailedStart)),
          static_cast<unsigned long>(positiveDelta(missingGlyphs_, missingStart)),
          static_cast<unsigned long>(positiveDelta(cacheEvictions_, evictionsStart)),
          static_cast<unsigned long>(cacheEvictions_),
          static_cast<unsigned long long>(rasterUsDelta), static_cast<unsigned long>(avgRasterUs), sidecarSaved ? 1 : 0,
          static_cast<unsigned long>(glyphSidecarDirtyGlyphs_), static_cast<unsigned>(glyphSidecarDirtyBytes_),
          static_cast<unsigned long>(millis() - startMs));
}

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

  for (auto& glyph : glyphCache_) {
    if (glyph.codepoint == cp) {
      ++cacheHits_;
      glyph.lastUsed = ++glyphUseClock_;
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
    cached.lastUsed = ++glyphUseClock_;
    glyphCache_.push_back(std::move(cached));
    markGlyphSidecarDirty(0);
    ++rasterOk_;
    return &glyphCache_.back();
  }

  OpenFontRender::GlyphBitmap glyph;
  const uint32_t rasterStartUs = micros();
  const FT_Error error = openFontRender_.renderGlyphBitmap(cp, glyph);
  rasterTimeUs_ += micros() - rasterStartUs;
  if (error || !glyph.buffer || glyph.width <= 0 || glyph.rows <= 0) {
    ++rasterFailed_;
    LOG_DBG("TTFR", "OpenFontRender primitive failed cp=U+%04lX glyph=%u error=%d", static_cast<unsigned long>(cp),
            metrics.glyphId, static_cast<int>(error));
    return nullptr;
  }

  cached.glyphId = static_cast<uint16_t>(glyph.glyph_index);
  cached.width = glyph.width;
  cached.height = glyph.rows;
  cached.xOffset = std::max(glyph.left, static_cast<int>(metrics.leftSideBearingPx));
  if (usesTuckedHorizontalAdvance(cp)) {
    cached.xOffset = 0;
  } else if (isVerticalColonPresentationForm(cp)) {
    cached.xOffset = std::max(0, (cached.advancePx - cached.width) / 2);
  }
  cached.yOffset = -glyph.top;
  cached.bitmapBytes = packedGlyphBytes(cached.width, cached.height);
  if (cached.bitmapBytes > 0) {
    if (!reserveGlyphCacheBytes(cached.bitmapBytes)) return nullptr;
    uint8_t* bitmapRaw =
        static_cast<uint8_t*>(heap_caps_malloc(cached.bitmapBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!bitmapRaw) {
      ++rasterFailed_;
      LOG_ERR("TTFR", "Failed to allocate OpenFontRender glyph cp=U+%04lX bytes=%u", static_cast<unsigned long>(cp),
              static_cast<unsigned>(cached.bitmapBytes));
      return nullptr;
    }
    cached.bitmap.reset(bitmapRaw);
    memset(cached.bitmap.get(), 0xFF, cached.bitmapBytes);
    const int sourcePitch = std::abs(glyph.pitch);
    for (int y = 0; y < cached.height; ++y) {
      const uint8_t* sourceRow = glyph.buffer + static_cast<size_t>(y) * sourcePitch;
      for (int x = 0; x < cached.width; ++x) {
        setPackedGlyphPixel(cached.bitmap.get(), static_cast<size_t>(y) * cached.width + x,
                            coverageToPackedGrayValue(quantizeCoverage(sourceRow[x])));
      }
    }
    glyphCacheBytes_ += cached.bitmapBytes;
  }

  cached.lastUsed = ++glyphUseClock_;
  const size_t cachedBytes = cached.bitmapBytes;
  glyphCache_.push_back(std::move(cached));
  markGlyphSidecarDirty(cachedBytes);
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
    cached.lastUsed = ++glyphUseClock_;
    glyphCache_.push_back(std::move(cached));
    markGlyphSidecarDirty(0);
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
  cached.bitmapBytes = packedGlyphBytes(cached.width, cached.height);
  if (cached.bitmapBytes > 0) {
    if (!reserveGlyphCacheBytes(cached.bitmapBytes)) return nullptr;
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

  cached.lastUsed = ++glyphUseClock_;
  const size_t cachedBytes = cached.bitmapBytes;
  glyphCache_.push_back(std::move(cached));
  markGlyphSidecarDirty(cachedBytes);
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
  const bool pixelState = renderMode == GfxRenderer::BW ? black : false;
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
        for (int gx = 0; gx < glyph->width; ++gx) {
          const uint8_t val = getPackedGlyphPixel(glyph->bitmap.get(), static_cast<size_t>(gy) * glyph->width + gx);
          if (shouldDrawPackedPixel(renderMode, val)) {
            renderer.drawPixel(drawX + gx, drawY + gy, pixelState);
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
  const bool pixelState = renderMode == GfxRenderer::BW ? black : false;
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
        const int screenX = drawX0 + gy;
        for (int gx = 0; gx < glyph->width; ++gx) {
          const uint8_t val = getPackedGlyphPixel(glyph->bitmap.get(), static_cast<size_t>(gy) * glyph->width + gx);
          if (shouldDrawPackedPixel(renderMode, val)) {
            renderer.drawPixel(screenX, cursorY - glyph->xOffset - gx, pixelState);
          }
        }
      }
      ++renderedGlyphs_;
    }

    prevAdvance = glyph->advancePx;
  }

  logRenderStats("draw_rotated");
}

bool TtfReaderMetrics::reserveGlyphCacheBytes(const size_t incomingBytes) const {
  if (incomingBytes == 0) return true;
  if (incomingBytes > TTF_GLYPH_CACHE_MAX_BYTES) {
    LOG_ERR("TTFR", "glyph too large for cache bytes=%u limit=%u", static_cast<unsigned>(incomingBytes),
            static_cast<unsigned>(TTF_GLYPH_CACHE_MAX_BYTES));
    ++rasterFailed_;
    return false;
  }

  uint32_t evicted = 0;
  size_t evictedBytes = 0;
  while (glyphCacheBytes_ + incomingBytes > TTF_GLYPH_CACHE_MAX_BYTES && !glyphCache_.empty()) {
    auto victim = std::min_element(glyphCache_.begin(), glyphCache_.end(), [](const CachedGlyph& a, const CachedGlyph& b) {
      return a.lastUsed < b.lastUsed;
    });
    if (victim == glyphCache_.end()) break;
    evictedBytes += victim->bitmapBytes;
    glyphCacheBytes_ = victim->bitmapBytes <= glyphCacheBytes_ ? glyphCacheBytes_ - victim->bitmapBytes : 0;
    glyphCache_.erase(victim);
    ++evicted;
  }

  if (evicted > 0) {
    cacheEvictions_ += evicted;
    LOG_INF("TTFR", "glyph cache evicted count=%lu bytes=%u cache=%u/%u glyphs=%u incoming=%u",
            static_cast<unsigned long>(evicted), static_cast<unsigned>(evictedBytes),
            static_cast<unsigned>(glyphCacheBytes_), static_cast<unsigned>(TTF_GLYPH_CACHE_MAX_BYTES),
            static_cast<unsigned>(glyphCache_.size()), static_cast<unsigned>(incomingBytes));
  }

  return glyphCacheBytes_ + incomingBytes <= TTF_GLYPH_CACHE_MAX_BYTES;
}

std::string TtfReaderMetrics::glyphSidecarPath() const {
  char filename[96] = {};
  snprintf(filename, sizeof(filename), "%s/%08lX_sz%u_v%u.gcache", TTF_GLYPH_CACHE_DIR,
           static_cast<unsigned long>(identityHash_), pixelSize_, TTF_GLYPH_SIDECAR_VERSION);
  return filename;
}

bool TtfReaderMetrics::loadGlyphSidecarCache() const {
  if (!loaded_ || !Storage.ready()) return false;

  const std::string path = glyphSidecarPath();
  if (!Storage.exists(path.c_str())) return false;

  const uint32_t startMs = millis();
  HalFile file = Storage.open(path.c_str(), O_RDONLY);
  if (!file) return false;

  uint8_t magic[4] = {};
  uint16_t version = 0;
  uint16_t reserved = 0;
  uint32_t identity = 0;
  uint32_t fileSize = 0;
  uint16_t pixelSize = 0;
  uint16_t glyphCount = 0;
  uint32_t payloadBytes = 0;

  bool ok = readExact(file, magic, sizeof(magic)) && readU16LE(file, version) && readU16LE(file, reserved) &&
            readU32LE(file, identity) && readU32LE(file, fileSize) && readU16LE(file, pixelSize) &&
            readU16LE(file, glyphCount) && readU32LE(file, payloadBytes);

  if (!ok || memcmp(magic, "TTGC", 4) != 0 || version != TTF_GLYPH_SIDECAR_VERSION || identity != identityHash_ ||
      fileSize != fileSize_ || pixelSize != pixelSize_ || payloadBytes > TTF_GLYPH_CACHE_MAX_BYTES) {
    file.close();
    LOG_DBG("TTFR", "glyph sidecar skipped path=%s valid=%d version=%u identity=0x%08lX file=%lu px=%u bytes=%lu",
            path.c_str(), ok ? 1 : 0, version, static_cast<unsigned long>(identity),
            static_cast<unsigned long>(fileSize), pixelSize, static_cast<unsigned long>(payloadBytes));
    return false;
  }

  std::vector<CachedGlyph> loadedGlyphs;
  loadedGlyphs.reserve(glyphCount);
  size_t loadedBytes = 0;

  for (uint16_t i = 0; i < glyphCount; ++i) {
    uint32_t codepoint = 0;
    uint16_t glyphId = 0;
    int16_t width = 0;
    int16_t height = 0;
    int16_t xOffset = 0;
    int16_t yOffset = 0;
    int16_t advancePx = 0;
    uint32_t bitmapBytes = 0;

    ok = readU32LE(file, codepoint) && readU16LE(file, glyphId) && readI16LE(file, width) && readI16LE(file, height) &&
         readI16LE(file, xOffset) && readI16LE(file, yOffset) && readI16LE(file, advancePx) &&
         readU32LE(file, bitmapBytes);
    if (!ok || width < 0 || height < 0 || bitmapBytes != packedGlyphBytes(width, height) ||
        loadedBytes + bitmapBytes > TTF_GLYPH_CACHE_MAX_BYTES) {
      ok = false;
      break;
    }

    CachedGlyph glyph;
    glyph.codepoint = codepoint;
    glyph.glyphId = glyphId;
    glyph.width = width;
    glyph.height = height;
    glyph.xOffset = xOffset;
    glyph.yOffset = yOffset;
    glyph.advancePx = advancePx;
    glyph.bitmapBytes = bitmapBytes;
    glyph.lastUsed = ++glyphUseClock_;

    if (bitmapBytes > 0) {
      uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(bitmapBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!raw) {
        ok = false;
        break;
      }
      glyph.bitmap.reset(raw);
      if (!readExact(file, glyph.bitmap.get(), bitmapBytes)) {
        ok = false;
        break;
      }
    }

    loadedBytes += bitmapBytes;
    loadedGlyphs.push_back(std::move(glyph));
  }

  file.close();

  if (!ok) {
    LOG_DBG("TTFR", "glyph sidecar invalid path=%s partial_glyphs=%u bytes=%u", path.c_str(),
            static_cast<unsigned>(loadedGlyphs.size()), static_cast<unsigned>(loadedBytes));
    return false;
  }

  glyphCache_ = std::move(loadedGlyphs);
  glyphCacheBytes_ = loadedBytes;
  glyphSidecarDirty_ = false;
  glyphSidecarDirtyGlyphs_ = 0;
  glyphSidecarDirtyBytes_ = 0;
  LOG_INF("TTFR", "glyph sidecar loaded path=%s glyphs=%u bytes=%u elapsed_ms=%lu", path.c_str(),
          static_cast<unsigned>(glyphCache_.size()), static_cast<unsigned>(glyphCacheBytes_),
          static_cast<unsigned long>(millis() - startMs));
  return true;
}

bool TtfReaderMetrics::saveGlyphSidecarCache() const {
  if (!loaded_ || !Storage.ready() || glyphCache_.empty() || !glyphSidecarDirty_) return false;

  Storage.mkdir("/.crosspoint");
  Storage.mkdir(TTF_GLYPH_CACHE_DIR);

  const std::string path = glyphSidecarPath();
  const std::string tmpPath = path + ".tmp";
  const uint32_t startMs = millis();

  if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());
  HalFile file = Storage.open(tmpPath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!file) return false;

  bool ok = writeExact(file, "TTGC", 4) && writeU16LE(file, TTF_GLYPH_SIDECAR_VERSION) && writeU16LE(file, 0) &&
            writeU32LE(file, identityHash_) && writeU32LE(file, fileSize_) && writeU16LE(file, pixelSize_) &&
            writeU16LE(file, static_cast<uint16_t>(std::min<size_t>(glyphCache_.size(), UINT16_MAX))) &&
            writeU32LE(file, static_cast<uint32_t>(std::min<size_t>(glyphCacheBytes_, UINT32_MAX)));

  uint16_t writtenGlyphs = 0;
  size_t writtenBytes = 0;
  for (const auto& glyph : glyphCache_) {
    if (!ok || writtenGlyphs == UINT16_MAX) break;
    if (!fitsInt16(glyph.width) || !fitsInt16(glyph.height) || !fitsInt16(glyph.xOffset) ||
        !fitsInt16(glyph.yOffset) || !fitsInt16(glyph.advancePx) || glyph.bitmapBytes > UINT32_MAX ||
        glyph.bitmapBytes != packedGlyphBytes(glyph.width, glyph.height)) {
      ok = false;
      break;
    }

    ok = writeU32LE(file, glyph.codepoint) && writeU16LE(file, glyph.glyphId) &&
         writeI16LE(file, static_cast<int16_t>(glyph.width)) && writeI16LE(file, static_cast<int16_t>(glyph.height)) &&
         writeI16LE(file, static_cast<int16_t>(glyph.xOffset)) &&
         writeI16LE(file, static_cast<int16_t>(glyph.yOffset)) &&
         writeI16LE(file, static_cast<int16_t>(glyph.advancePx)) &&
         writeU32LE(file, static_cast<uint32_t>(glyph.bitmapBytes));
    if (ok && glyph.bitmapBytes > 0) ok = writeExact(file, glyph.bitmap.get(), glyph.bitmapBytes);
    if (ok) {
      ++writtenGlyphs;
      writtenBytes += glyph.bitmapBytes;
    }
  }

  file.flush();
  file.close();

  if (!ok || writtenGlyphs != glyphCache_.size() || writtenBytes != glyphCacheBytes_) {
    Storage.remove(tmpPath.c_str());
    LOG_DBG("TTFR", "glyph sidecar save failed path=%s glyphs=%u/%u bytes=%u/%u", path.c_str(), writtenGlyphs,
            static_cast<unsigned>(glyphCache_.size()), static_cast<unsigned>(writtenBytes),
            static_cast<unsigned>(glyphCacheBytes_));
    return false;
  }

  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    Storage.remove(tmpPath.c_str());
    return false;
  }

  glyphSidecarDirty_ = false;
  glyphSidecarDirtyGlyphs_ = 0;
  glyphSidecarDirtyBytes_ = 0;
  lastGlyphSidecarSaveMs_ = millis();
  LOG_INF("TTFR", "glyph sidecar saved path=%s glyphs=%u bytes=%u elapsed_ms=%lu", path.c_str(), writtenGlyphs,
          static_cast<unsigned>(writtenBytes), static_cast<unsigned long>(lastGlyphSidecarSaveMs_ - startMs));
  return true;
}

bool TtfReaderMetrics::maybeSaveGlyphSidecarCache() const {
  if (!glyphSidecarDirty_) return false;
  const unsigned long now = millis();
  if (lastGlyphSidecarSaveMs_ != 0 && now - lastGlyphSidecarSaveMs_ < TTF_GLYPH_SIDECAR_SAVE_INTERVAL_MS) {
    return false;
  }
  if (glyphSidecarDirtyGlyphs_ < TTF_GLYPH_SIDECAR_SAVE_DIRTY_GLYPHS &&
      glyphSidecarDirtyBytes_ < TTF_GLYPH_SIDECAR_SAVE_DIRTY_BYTES) {
    return false;
  }
  return saveGlyphSidecarCache();
}

void TtfReaderMetrics::markGlyphSidecarDirty(const size_t bytes) const {
  glyphSidecarDirty_ = true;
  ++glyphSidecarDirtyGlyphs_;
  glyphSidecarDirtyBytes_ += bytes;
}

void TtfReaderMetrics::clearGlyphCache() const {
  glyphCache_.clear();
  glyphCacheBytes_ = 0;
  glyphUseClock_ = 0;
  cacheHits_ = 0;
  cacheMisses_ = 0;
  rasterOk_ = 0;
  rasterFailed_ = 0;
  compoundGlyphs_ = 0;
  missingGlyphs_ = 0;
  renderedGlyphs_ = 0;
  cacheResets_ = 0;
  cacheEvictions_ = 0;
  glyphSidecarDirty_ = false;
  glyphSidecarDirtyGlyphs_ = 0;
  glyphSidecarDirtyBytes_ = 0;
  lastGlyphSidecarSaveMs_ = 0;
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
  const uint32_t cachePct =
      static_cast<uint32_t>((glyphCacheBytes_ * 100 + TTF_GLYPH_CACHE_MAX_BYTES / 2) / TTF_GLYPH_CACHE_MAX_BYTES);
  LOG_INF("TTFR",
          "render stats label=%s drawn=%lu cached_glyphs=%u cache_bytes=%u cache_limit=%u cache_pct=%lu "
          "hits=%lu misses=%lu raster_ok=%lu raster_fail=%lu compound=%lu missing=%lu cache_resets=%lu "
          "cache_evictions=%lu avg_raster_us=%lu "
          "delta_drawn=%lu delta_misses=%lu delta_raster=%lu delta_avg_raster_us=%lu",
          label ? label : "render", static_cast<unsigned long>(renderedGlyphs_),
          static_cast<unsigned>(glyphCache_.size()), static_cast<unsigned>(glyphCacheBytes_),
          static_cast<unsigned>(TTF_GLYPH_CACHE_MAX_BYTES), static_cast<unsigned long>(cachePct),
          static_cast<unsigned long>(cacheHits_), static_cast<unsigned long>(cacheMisses_),
          static_cast<unsigned long>(rasterOk_), static_cast<unsigned long>(rasterFailed_),
          static_cast<unsigned long>(compoundGlyphs_), static_cast<unsigned long>(missingGlyphs_),
          static_cast<unsigned long>(cacheResets_), static_cast<unsigned long>(cacheEvictions_),
          static_cast<unsigned long>(avgRasterUs),
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
  return ReaderFontResolver::computeTtfIdentityHash(path, pixelSize, fileSize);
}

#endif
