#ifdef CROSSPOINT_TTF_PROBE

#include <Arduino.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <TtfCustomRasterizer.h>
#include <TtfProbe.h>
#include <TtfRuntimeFont.h>
#include <TtfStb.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr uint32_t LOG_INTERVAL_MS = 10000;
constexpr uint32_t STATUS_INTERVAL_MS = 30000;
constexpr char TTF_SCAN_DIR[] = "/TTF";
constexpr size_t READ_CHUNK_SIZE = 4096;
constexpr size_t TTF_HEADER_SIZE = 12;
constexpr size_t TTF_TABLE_RECORD_SIZE = 16;
constexpr size_t STB_FULL_BUFFER_LIMIT = 4 * 1024 * 1024;
constexpr size_t STB_FULL_BUFFER_SAFETY_MARGIN = 512 * 1024;
constexpr size_t TABLE_CACHE_SAFETY_MARGIN = 512 * 1024;
constexpr uint16_t METRICS_PIXEL_SIZE = 24;
constexpr size_t RASTER_SCRATCH_BYTES = 64 * 1024;
constexpr size_t GLYF_SLICE_SAFETY_MARGIN = 512 * 1024;

struct ProbeStatus {
  bool displayInitialized = false;
  bool storageReady = false;
  bool sfntOk = false;
  bool stbLinked = false;
  bool sdTtfFound = false;
  bool sdTtfDirectoryLoaded = false;
  bool sdTtfFullBufferLoaded = false;
  bool sdTtfSfntOk = false;
  bool sdTtfStbOk = false;
  bool sdTtfStbSkippedLarge = false;
  bool sdTtfMetricsOk = false;
  bool sdTtfRasterAttempted = false;
  bool sdTtfRasterOk = false;
  bool sdTtfRasterSkippedFullBuffer = false;
  bool sdTtfCustomRasterOk = false;
  uint16_t sfntTableCount = 0;
  uint16_t sdTtfTableCount = 0;
  uint8_t sdTtfMetricQueries = 0;
  uint8_t sdTtfMetricHits = 0;
  uint8_t sdTtfRasterQueries = 0;
  uint8_t sdTtfRasterHits = 0;
  uint32_t sdTtfRasterBytes = 0;
  uint8_t sdTtfCustomRasterQueries = 0;
  uint8_t sdTtfCustomRasterHits = 0;
  uint8_t sdTtfCustomRasterCompounds = 0;
  uint32_t sdTtfCustomRasterBytes = 0;
  uint16_t displayWidth = 0;
  uint16_t displayHeight = 0;
  uint32_t displayBufferSize = 0;
  uint32_t sdTtfHash = 0;
  uint64_t sdTtfSize = 0;
};

ProbeStatus probeStatus;

constexpr uint8_t kMinimalSfnt[] = {
    0x00, 0x01, 0x00, 0x00,  // scaler type
    0x00, 0x07,              // numTables
    0x00, 0x00,              // searchRange
    0x00, 0x00,              // entrySelector
    0x00, 0x00,              // rangeShift
    'c',  'm',  'a',  'p',   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x00,
    'g',  'l',  'y',  'f',   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x00,
    'h',  'e',  'a',  'd',   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x00,
    'h',  'h',  'e',  'a',   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x00,
    'h',  'm',  't',  'x',   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x00,
    'l',  'o',  'c',  'a',   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x00,
    'm',  'a',  'x',  'p',   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x00,
};

static_assert(sizeof(kMinimalSfnt) == 124, "minimal sfnt fixture size changed");

struct PsramFreeDeleter {
  void operator()(uint8_t* ptr) const {
    if (ptr) heap_caps_free(ptr);
  }
};

struct TtfTableRecord {
  uint32_t tag = 0;
  uint32_t offset = 0;
  uint32_t length = 0;
};

struct CachedTable {
  std::unique_ptr<uint8_t, PsramFreeDeleter> data;
  uint32_t length = 0;

  ttf::TableView view() const { return {data.get(), length}; }
};

struct MetricsTableCache {
  CachedTable cmap;
  CachedTable head;
  CachedTable hhea;
  CachedTable hmtx;
  CachedTable loca;
  CachedTable maxp;

  ttf::RuntimeTableSet views() const { return {cmap.view(), head.view(), hhea.view(), hmtx.view(), loca.view(), maxp.view()}; }
};

struct TtfSelection {
  std::string path;
  uint64_t size = 0;
};

void logMemory(const char* label) {
  LOG_INF("TTFP", "%s heap free=%lu min=%lu psram free=%lu size=%lu", label,
          static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMinFreeHeap()),
          static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
          static_cast<unsigned long>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)));
}

uint32_t fnv1aUpdate(uint32_t hash, const uint8_t* data, const size_t len) {
  constexpr uint32_t FNV_PRIME = 16777619u;
  for (size_t i = 0; i < len; ++i) {
    hash ^= data[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

bool endsWithTtf(const String& name) {
  const int len = name.length();
  if (len < 4) return false;
  return std::tolower(static_cast<unsigned char>(name[len - 4])) == '.' &&
         std::tolower(static_cast<unsigned char>(name[len - 3])) == 't' &&
         std::tolower(static_cast<unsigned char>(name[len - 2])) == 't' &&
         std::tolower(static_cast<unsigned char>(name[len - 1])) == 'f';
}

std::string normalizeListedPath(const String& listed) {
  const std::string value = listed.c_str();
  if (!value.empty() && value[0] == '/') {
    return value;
  }
  return std::string(TTF_SCAN_DIR) + "/" + value;
}

bool getReadOnlyFileSize(const char* path, uint64_t& outSize) {
  outSize = 0;
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) {
    LOG_ERR("TTFP", "failed to open TTF candidate read-only: %s", path);
    return false;
  }
  outSize = file.fileSize64();
  file.close();
  return outSize > 0;
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

uint16_t readU16BE(const uint8_t* p) { return (static_cast<uint16_t>(p[0]) << 8) | p[1]; }

uint32_t readU32BE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

bool loadSfntDirectoryReadOnly(const char* path, std::unique_ptr<uint8_t, PsramFreeDeleter>& outData, size_t& outSize,
                               uint32_t& outHash) {
  outData.reset();
  outSize = 0;
  outHash = 2166136261u;

  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) {
    LOG_ERR("TTFP", "failed to open TTF read-only: %s", path);
    return false;
  }

  const uint64_t fileSize = file.fileSize64();
  probeStatus.sdTtfSize = fileSize;
  if (fileSize < TTF_HEADER_SIZE || fileSize > SIZE_MAX) {
    LOG_ERR("TTFP", "invalid TTF size path=%s size=%llu", path, static_cast<unsigned long long>(fileSize));
    file.close();
    return false;
  }

  uint8_t header[TTF_HEADER_SIZE] = {};
  if (!readExact(file, header, sizeof(header))) {
    LOG_ERR("TTFP", "failed to read TTF header path=%s", path);
    file.close();
    return false;
  }
  outHash = fnv1aUpdate(outHash, header, sizeof(header));

  const uint16_t tableCount = readU16BE(header + 4);
  const size_t directorySize = TTF_HEADER_SIZE + static_cast<size_t>(tableCount) * TTF_TABLE_RECORD_SIZE;
  if (directorySize > static_cast<size_t>(fileSize)) {
    LOG_ERR("TTFP", "TTF table directory outside file path=%s tables=%u dir_size=%lu file_size=%llu", path, tableCount,
            static_cast<unsigned long>(directorySize), static_cast<unsigned long long>(fileSize));
    file.close();
    return false;
  }

  uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(directorySize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!raw) {
    LOG_ERR("TTFP", "failed to allocate PSRAM for TTF directory size=%lu", static_cast<unsigned long>(directorySize));
    file.close();
    return false;
  }
  std::unique_ptr<uint8_t, PsramFreeDeleter> buffer(raw);
  memcpy(buffer.get(), header, sizeof(header));

  const size_t remainingDirectory = directorySize - TTF_HEADER_SIZE;
  if (remainingDirectory > 0 && !readExact(file, buffer.get() + TTF_HEADER_SIZE, remainingDirectory)) {
    LOG_ERR("TTFP", "failed to read TTF directory path=%s", path);
    file.close();
    return false;
  }
  outHash = fnv1aUpdate(outHash, buffer.get() + TTF_HEADER_SIZE, remainingDirectory);

  uint8_t chunk[READ_CHUNK_SIZE];
  uint64_t streamed = directorySize;
  while (streamed < fileSize) {
    const size_t want = std::min<uint64_t>(READ_CHUNK_SIZE, fileSize - streamed);
    const int got = file.read(chunk, want);
    if (got <= 0) {
      LOG_ERR("TTFP", "short TTF hash read path=%s got=%llu expected=%llu", path,
              static_cast<unsigned long long>(streamed), static_cast<unsigned long long>(fileSize));
      file.close();
      return false;
    }
    outHash = fnv1aUpdate(outHash, chunk, static_cast<size_t>(got));
    streamed += static_cast<size_t>(got);
  }

  file.close();
  outSize = directorySize;
  outData = std::move(buffer);
  return true;
}

bool loadFullFileReadOnly(const char* path, std::unique_ptr<uint8_t, PsramFreeDeleter>& outData, size_t& outSize) {
  outData.reset();
  outSize = 0;

  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) {
    LOG_ERR("TTFP", "failed to open TTF read-only for full-buffer probe: %s", path);
    return false;
  }

  const uint64_t fileSize = file.fileSize64();
  if (fileSize == 0 || fileSize > SIZE_MAX) {
    file.close();
    return false;
  }
  const size_t size = static_cast<size_t>(fileSize);
  const size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (size > STB_FULL_BUFFER_LIMIT || size + STB_FULL_BUFFER_SAFETY_MARGIN > freePsram) {
    probeStatus.sdTtfStbSkippedLarge = true;
    LOG_INF("TTFP", "full-buffer stb probe skipped path=%s size=%lu limit=%lu free_psram=%lu margin=%lu", path,
            static_cast<unsigned long>(size), static_cast<unsigned long>(STB_FULL_BUFFER_LIMIT),
            static_cast<unsigned long>(freePsram), static_cast<unsigned long>(STB_FULL_BUFFER_SAFETY_MARGIN));
    file.close();
    return false;
  }

  uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!raw) {
    LOG_ERR("TTFP", "failed to allocate PSRAM for full-buffer stb probe size=%lu", static_cast<unsigned long>(size));
    file.close();
    return false;
  }
  std::unique_ptr<uint8_t, PsramFreeDeleter> buffer(raw);

  if (!readExact(file, buffer.get(), size)) {
    LOG_ERR("TTFP", "short full-buffer TTF read path=%s", path);
    file.close();
    return false;
  }

  file.close();
  outSize = size;
  outData = std::move(buffer);
  return true;
}

bool findTableRecord(const uint8_t* directoryData, const size_t directorySize, const uint32_t tableTag,
                     TtfTableRecord& out) {
  if (!directoryData || directorySize < TTF_HEADER_SIZE) return false;
  const uint16_t tableCount = readU16BE(directoryData + 4);
  const size_t recordsSize = TTF_HEADER_SIZE + static_cast<size_t>(tableCount) * TTF_TABLE_RECORD_SIZE;
  if (recordsSize > directorySize) return false;

  for (uint16_t i = 0; i < tableCount; ++i) {
    const uint8_t* record = directoryData + TTF_HEADER_SIZE + static_cast<size_t>(i) * TTF_TABLE_RECORD_SIZE;
    const uint32_t tag = readU32BE(record);
    if (tag != tableTag) continue;
    out.tag = tag;
    out.offset = readU32BE(record + 8);
    out.length = readU32BE(record + 12);
    return true;
  }

  return false;
}

bool loadTableReadOnly(const char* path, const uint8_t* directoryData, const size_t directorySize,
                       const uint32_t tableTag, CachedTable& out) {
  out.data.reset();
  out.length = 0;

  TtfTableRecord record;
  if (!findTableRecord(directoryData, directorySize, tableTag, record)) {
    LOG_ERR("TTFP", "missing TTF table tag=0x%08lX", static_cast<unsigned long>(tableTag));
    return false;
  }
  if (record.length == 0) {
    LOG_ERR("TTFP", "empty TTF table tag=0x%08lX", static_cast<unsigned long>(tableTag));
    return false;
  }

  const size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (record.length + TABLE_CACHE_SAFETY_MARGIN > freePsram) {
    LOG_ERR("TTFP", "skip table cache tag=0x%08lX len=%lu free_psram=%lu margin=%lu",
            static_cast<unsigned long>(tableTag), static_cast<unsigned long>(record.length),
            static_cast<unsigned long>(freePsram), static_cast<unsigned long>(TABLE_CACHE_SAFETY_MARGIN));
    return false;
  }

  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) {
    LOG_ERR("TTFP", "failed to open TTF read-only for table load: %s", path);
    return false;
  }
  if (!file.seek64(record.offset)) {
    LOG_ERR("TTFP", "failed to seek TTF table path=%s tag=0x%08lX offset=%lu", path,
            static_cast<unsigned long>(tableTag), static_cast<unsigned long>(record.offset));
    file.close();
    return false;
  }

  uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(record.length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!raw) {
    LOG_ERR("TTFP", "failed to allocate table cache tag=0x%08lX len=%lu", static_cast<unsigned long>(tableTag),
            static_cast<unsigned long>(record.length));
    file.close();
    return false;
  }
  std::unique_ptr<uint8_t, PsramFreeDeleter> buffer(raw);
  if (!readExact(file, buffer.get(), record.length)) {
    LOG_ERR("TTFP", "short TTF table read path=%s tag=0x%08lX len=%lu", path,
            static_cast<unsigned long>(tableTag), static_cast<unsigned long>(record.length));
    file.close();
    return false;
  }
  file.close();

  out.length = record.length;
  out.data = std::move(buffer);
  LOG_INF("TTFP", "cached table tag=0x%08lX offset=%lu len=%lu", static_cast<unsigned long>(tableTag),
          static_cast<unsigned long>(record.offset), static_cast<unsigned long>(record.length));
  return true;
}

bool loadMetricsTablesReadOnly(const char* path, const uint8_t* directoryData, const size_t directorySize,
                               MetricsTableCache& cache) {
  return loadTableReadOnly(path, directoryData, directorySize, ttf::tag('c', 'm', 'a', 'p'), cache.cmap) &&
         loadTableReadOnly(path, directoryData, directorySize, ttf::tag('h', 'e', 'a', 'd'), cache.head) &&
         loadTableReadOnly(path, directoryData, directorySize, ttf::tag('h', 'h', 'e', 'a'), cache.hhea) &&
         loadTableReadOnly(path, directoryData, directorySize, ttf::tag('h', 'm', 't', 'x'), cache.hmtx) &&
         loadTableReadOnly(path, directoryData, directorySize, ttf::tag('l', 'o', 'c', 'a'), cache.loca) &&
         loadTableReadOnly(path, directoryData, directorySize, ttf::tag('m', 'a', 'x', 'p'), cache.maxp);
}

ttf::GlyphMetrics logGlyphMetrics(ttf::TtfRuntimeFont& font, const char* label, const uint32_t codepoint) {
  const auto glyph = font.metricsForCodepoint(codepoint);
  ++probeStatus.sdTtfMetricQueries;
  if (glyph.present) ++probeStatus.sdTtfMetricHits;
  LOG_INF("TTFP", "metric %s cp=U+%04lX present=%d glyph=%u adv=%u lsb=%d adv_px=%d lsb_px=%d glyf_off=%lu glyf_len=%lu",
          label, static_cast<unsigned long>(codepoint), glyph.present ? 1 : 0, glyph.glyphId, glyph.advanceWidth,
          glyph.leftSideBearing, glyph.advancePx, glyph.leftSideBearingPx, static_cast<unsigned long>(glyph.glyphOffset),
          static_cast<unsigned long>(glyph.glyphLength));
  return glyph;
}

bool loadGlyphSliceReadOnly(const char* path, const uint8_t* directoryData, const size_t directorySize,
                            const ttf::GlyphMetrics& glyph, std::unique_ptr<uint8_t, PsramFreeDeleter>& outData,
                            uint32_t& outLength) {
  outData.reset();
  outLength = 0;
  if (!glyph.present || glyph.glyphLength == 0) return false;

  TtfTableRecord glyfTable;
  if (!findTableRecord(directoryData, directorySize, ttf::tag('g', 'l', 'y', 'f'), glyfTable)) {
    LOG_ERR("TTFP", "missing glyf table for custom raster");
    return false;
  }
  if (glyph.glyphOffset > glyfTable.length || glyph.glyphLength > glyfTable.length - glyph.glyphOffset) {
    LOG_ERR("TTFP", "glyph slice outside glyf table glyph=%u off=%lu len=%lu glyf_len=%lu", glyph.glyphId,
            static_cast<unsigned long>(glyph.glyphOffset), static_cast<unsigned long>(glyph.glyphLength),
            static_cast<unsigned long>(glyfTable.length));
    return false;
  }

  const size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (glyph.glyphLength + GLYF_SLICE_SAFETY_MARGIN > freePsram) {
    LOG_ERR("TTFP", "skip glyph slice glyph=%u len=%lu free_psram=%lu margin=%lu", glyph.glyphId,
            static_cast<unsigned long>(glyph.glyphLength), static_cast<unsigned long>(freePsram),
            static_cast<unsigned long>(GLYF_SLICE_SAFETY_MARGIN));
    return false;
  }

  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) {
    LOG_ERR("TTFP", "failed to open TTF read-only for glyph slice: %s", path);
    return false;
  }
  const uint64_t fileOffset = static_cast<uint64_t>(glyfTable.offset) + glyph.glyphOffset;
  if (!file.seek64(fileOffset)) {
    LOG_ERR("TTFP", "failed to seek glyph slice path=%s glyph=%u file_off=%llu", path, glyph.glyphId,
            static_cast<unsigned long long>(fileOffset));
    file.close();
    return false;
  }

  uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(glyph.glyphLength, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!raw) {
    LOG_ERR("TTFP", "failed to allocate glyph slice glyph=%u len=%lu", glyph.glyphId,
            static_cast<unsigned long>(glyph.glyphLength));
    file.close();
    return false;
  }
  std::unique_ptr<uint8_t, PsramFreeDeleter> buffer(raw);
  if (!readExact(file, buffer.get(), glyph.glyphLength)) {
    LOG_ERR("TTFP", "short glyph slice read glyph=%u len=%lu", glyph.glyphId,
            static_cast<unsigned long>(glyph.glyphLength));
    file.close();
    return false;
  }
  file.close();

  outLength = glyph.glyphLength;
  outData = std::move(buffer);
  return true;
}

void logCustomRasterGlyph(const char* path, const uint8_t* directoryData, const size_t directorySize,
                          const ttf::FontMetrics& metrics, const ttf::GlyphMetrics& glyph, const char* label,
                          uint8_t* scratch, const size_t scratchSize) {
  ++probeStatus.sdTtfCustomRasterQueries;
  if (!glyph.present || glyph.glyphLength == 0) {
    LOG_INF("TTFP", "raster_custom %s cp=U+%04lX skipped present=%d glyf_len=%lu", label,
            static_cast<unsigned long>(glyph.codepoint), glyph.present ? 1 : 0,
            static_cast<unsigned long>(glyph.glyphLength));
    return;
  }

  std::unique_ptr<uint8_t, PsramFreeDeleter> glyphData;
  uint32_t glyphLength = 0;
  const uint32_t startUs = micros();
  if (!loadGlyphSliceReadOnly(path, directoryData, directorySize, glyph, glyphData, glyphLength)) {
    LOG_ERR("TTFP", "raster_custom %s glyph slice load failed glyph=%u", label, glyph.glyphId);
    return;
  }

  const auto raster = ttf::rasterizeSimpleGlyf(glyphData.get(), glyphLength, metrics.unitsPerEm, metrics.pixelSize,
                                               scratch, scratchSize);
  const uint32_t elapsedUs = micros() - startUs;
  if (raster.ok) {
    ++probeStatus.sdTtfCustomRasterHits;
    probeStatus.sdTtfCustomRasterBytes += raster.bitmapBytes;
    probeStatus.sdTtfCustomRasterOk = true;
  }
  if (raster.compound) ++probeStatus.sdTtfCustomRasterCompounds;
  LOG_INF("TTFP", "raster_custom %s cp=U+%04lX ok=%d compound=%d contours=%u points=%u glyph=%u %dx%d "
                  "bytes=%lu xoff=%d yoff=%d us=%lu error=%s",
          label, static_cast<unsigned long>(glyph.codepoint), raster.ok ? 1 : 0, raster.compound ? 1 : 0,
          raster.contourCount, raster.pointCount, glyph.glyphId, raster.width, raster.height,
          static_cast<unsigned long>(raster.bitmapBytes), raster.xOffset, raster.yOffset,
          static_cast<unsigned long>(elapsedUs), raster.error ? raster.error : "none");
}

void logStbRasterGlyph(const uint8_t* fullData, const size_t fullSize, const char* label, const uint32_t codepoint,
                       uint8_t* scratch, const size_t scratchSize) {
  ++probeStatus.sdTtfRasterQueries;
  const uint32_t startUs = micros();
  const auto raster = ttf::rasterizeStbGlyph(fullData, fullSize, codepoint, static_cast<float>(METRICS_PIXEL_SIZE),
                                             scratch, scratchSize);
  const uint32_t elapsedUs = micros() - startUs;
  if (raster.ok) {
    ++probeStatus.sdTtfRasterHits;
    probeStatus.sdTtfRasterBytes += raster.bitmapBytes;
  }
  LOG_INF("TTFP", "raster_stb %s cp=U+%04lX ok=%d glyph=%d %dx%d bytes=%lu xoff=%d yoff=%d adv=%d lsb=%d us=%lu error=%s",
          label, static_cast<unsigned long>(codepoint), raster.ok ? 1 : 0, raster.glyph, raster.width, raster.height,
          static_cast<unsigned long>(raster.bitmapBytes), raster.xOffset, raster.yOffset, raster.advanceWidth,
          raster.leftSideBearing, static_cast<unsigned long>(elapsedUs), raster.error ? raster.error : "none");
}

void runStbRasterReference(const uint8_t* fullData, const size_t fullSize) {
  probeStatus.sdTtfRasterAttempted = true;
  uint8_t* scratchRaw = static_cast<uint8_t*>(heap_caps_malloc(RASTER_SCRATCH_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!scratchRaw) {
    LOG_ERR("TTFP", "failed to allocate raster scratch bytes=%lu", static_cast<unsigned long>(RASTER_SCRATCH_BYTES));
    return;
  }
  std::unique_ptr<uint8_t, PsramFreeDeleter> scratch(scratchRaw);

  logMemory("before stb raster reference");
  logStbRasterGlyph(fullData, fullSize, "ascii_A", 0x0041, scratch.get(), RASTER_SCRATCH_BYTES);
  logStbRasterGlyph(fullData, fullSize, "kana_a", 0x3042, scratch.get(), RASTER_SCRATCH_BYTES);
  logStbRasterGlyph(fullData, fullSize, "kanji_day", 0x65E5, scratch.get(), RASTER_SCRATCH_BYTES);
  logStbRasterGlyph(fullData, fullSize, "fullwidth_punct", 0x3001, scratch.get(), RASTER_SCRATCH_BYTES);
  logStbRasterGlyph(fullData, fullSize, "missing_probe", 0x10FFFF, scratch.get(), RASTER_SCRATCH_BYTES);
  probeStatus.sdTtfRasterOk = probeStatus.sdTtfRasterHits > 0;
  logMemory("after stb raster reference");
}

void runStbReferenceForSmallestTtf(const TtfSelection& selection) {
  if (selection.path.empty()) return;
  LOG_INF("TTFP", "6A selected smallest TTF path=%s size=%llu", selection.path.c_str(),
          static_cast<unsigned long long>(selection.size));

  std::unique_ptr<uint8_t, PsramFreeDeleter> fullData;
  size_t fullSize = 0;
  if (!loadFullFileReadOnly(selection.path.c_str(), fullData, fullSize)) {
    return;
  }

  probeStatus.sdTtfFullBufferLoaded = true;
  const auto stb = ttf::probeStbTruetype(fullData.get(), fullSize);
  probeStatus.sdTtfStbOk = stb.ok;
  LOG_INF("TTFP", "SD TTF stb ok=%d ascent=%d descent=%d lineGap=%d glyphA=%d error=%s", stb.ok ? 1 : 0,
          stb.ascent, stb.descent, stb.lineGap, stb.glyphA, stb.error ? stb.error : "none");
  if (stb.ok) {
    runStbRasterReference(fullData.get(), fullSize);
  }
}

void runTtfRuntimeSmoke() {
  const auto sfnt = ttf::probeSfnt(kMinimalSfnt, sizeof(kMinimalSfnt));
  probeStatus.sfntOk = sfnt.ok;
  probeStatus.sfntTableCount = sfnt.tableCount;
  LOG_INF("TTFP", "sfnt fixture ok=%d tables=%u error=%s", sfnt.ok ? 1 : 0, sfnt.tableCount,
          sfnt.error ? sfnt.error : "none");

  const auto stb = ttf::probeStbTruetype(nullptr, 0);
  probeStatus.stbLinked = !stb.ok && stb.error != nullptr;
  LOG_INF("TTFP", "stb linked ok=%d expected_error=%s", stb.ok ? 1 : 0, stb.error ? stb.error : "none");
}

void scanAndProbeSdTtfReadOnly() {
  if (!probeStatus.storageReady) {
    LOG_INF("TTFP", "storage not ready; skipping %s TTF scan", TTF_SCAN_DIR);
    return;
  }

  const std::vector<String> entries = Storage.listFiles(TTF_SCAN_DIR, 32);
  LOG_INF("TTFP", "scan %s entries=%u", TTF_SCAN_DIR, static_cast<unsigned>(entries.size()));

  TtfSelection smallest;
  TtfSelection largest;
  for (const auto& entry : entries) {
    LOG_INF("TTFP", "scan entry=%s", entry.c_str());
    if (!endsWithTtf(entry)) continue;

    const std::string candidatePath = normalizeListedPath(entry);
    uint64_t candidateSize = 0;
    if (!getReadOnlyFileSize(candidatePath.c_str(), candidateSize)) continue;
    LOG_INF("TTFP", "TTF candidate path=%s size=%llu", candidatePath.c_str(),
            static_cast<unsigned long long>(candidateSize));
    if (smallest.path.empty() || candidateSize < smallest.size) {
      smallest.path = candidatePath;
      smallest.size = candidateSize;
    }
    if (largest.path.empty() || candidateSize > largest.size) {
      largest.path = candidatePath;
      largest.size = candidateSize;
    }
  }

  if (largest.path.empty()) {
    LOG_INF("TTFP", "no .ttf files found in %s", TTF_SCAN_DIR);
    return;
  }

  probeStatus.sdTtfFound = true;
  LOG_INF("TTFP", "6B selected largest TTF path=%s size=%llu", largest.path.c_str(),
          static_cast<unsigned long long>(largest.size));
  if (!smallest.path.empty() && smallest.path != largest.path) {
    LOG_INF("TTFP", "6A smallest TTF retained for stb reference path=%s size=%llu", smallest.path.c_str(),
            static_cast<unsigned long long>(smallest.size));
  }

  std::unique_ptr<uint8_t, PsramFreeDeleter> directoryData;
  size_t directorySize = 0;
  uint32_t hash = 0;
  if (!loadSfntDirectoryReadOnly(largest.path.c_str(), directoryData, directorySize, hash)) {
    return;
  }

  probeStatus.sdTtfDirectoryLoaded = true;
  probeStatus.sdTtfHash = hash;
  LOG_INF("TTFP", "loaded TTF directory path=%s file_size=%llu directory_size=%lu hash=0x%08lX", largest.path.c_str(),
          static_cast<unsigned long long>(probeStatus.sdTtfSize), static_cast<unsigned long>(directorySize),
          static_cast<unsigned long>(hash));

  const auto sfnt = ttf::probeSfntDirectory(directoryData.get(), directorySize, probeStatus.sdTtfSize);
  probeStatus.sdTtfSfntOk = sfnt.ok;
  probeStatus.sdTtfTableCount = sfnt.tableCount;
  LOG_INF("TTFP", "SD TTF sfnt ok=%d tables=%u cmap=%d glyf=%d hhea=%d hmtx=%d loca=%d maxp=%d kern=%d error=%s",
          sfnt.ok ? 1 : 0, sfnt.tableCount, sfnt.hasCmap ? 1 : 0, sfnt.hasGlyf ? 1 : 0, sfnt.hasHhea ? 1 : 0,
          sfnt.hasHmtx ? 1 : 0, sfnt.hasLoca ? 1 : 0, sfnt.hasMaxp ? 1 : 0, sfnt.hasKern ? 1 : 0,
          sfnt.error ? sfnt.error : "none");

  runStbReferenceForSmallestTtf(smallest);

  if (!sfnt.ok) {
    LOG_INF("TTFP", "skipping runtime metrics because sfnt probe failed");
    return;
  }

  MetricsTableCache tableCache;
  if (!loadMetricsTablesReadOnly(largest.path.c_str(), directoryData.get(), directorySize, tableCache)) {
    LOG_ERR("TTFP", "failed to cache runtime metrics tables");
    return;
  }

  ttf::TtfRuntimeFont font;
  const char* metricsError = nullptr;
  if (!font.begin(tableCache.views(), METRICS_PIXEL_SIZE, &metricsError)) {
    LOG_ERR("TTFP", "TTF runtime metrics init failed error=%s", metricsError ? metricsError : "unknown");
    return;
  }

  probeStatus.sdTtfMetricsOk = true;
  const auto& metrics = font.metrics();
  LOG_INF("TTFP", "TTF metrics ready px=%u upem=%u asc=%d desc=%d gap=%d glyphs=%u hmetrics=%u loca_fmt=%d",
          metrics.pixelSize, metrics.unitsPerEm, metrics.ascender, metrics.descender, metrics.lineGap,
          metrics.numGlyphs, metrics.numLongHorMetrics, metrics.indexToLocFormat);
  const auto glyphAscii = logGlyphMetrics(font, "ascii_A", 0x0041);
  const auto glyphKana = logGlyphMetrics(font, "kana_a", 0x3042);
  const auto glyphKanji = logGlyphMetrics(font, "kanji_day", 0x65E5);
  const auto glyphPunct = logGlyphMetrics(font, "fullwidth_punct", 0x3001);
  logGlyphMetrics(font, "replacement", 0xFFFD);
  logGlyphMetrics(font, "missing_probe", 0x10FFFF);

  if (!probeStatus.sdTtfFullBufferLoaded) {
    probeStatus.sdTtfRasterSkippedFullBuffer = true;
    LOG_INF("TTFP", "stb raster reference skipped: no smallest font was loaded as full buffer");
  }

  uint8_t* customScratchRaw = static_cast<uint8_t*>(heap_caps_malloc(RASTER_SCRATCH_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!customScratchRaw) {
    LOG_ERR("TTFP", "failed to allocate custom raster scratch bytes=%lu", static_cast<unsigned long>(RASTER_SCRATCH_BYTES));
    return;
  }
  std::unique_ptr<uint8_t, PsramFreeDeleter> customScratch(customScratchRaw);
  logMemory("before custom raster");
  logCustomRasterGlyph(largest.path.c_str(), directoryData.get(), directorySize, metrics, glyphAscii, "ascii_A",
                       customScratch.get(), RASTER_SCRATCH_BYTES);
  logCustomRasterGlyph(largest.path.c_str(), directoryData.get(), directorySize, metrics, glyphKana, "kana_a",
                       customScratch.get(), RASTER_SCRATCH_BYTES);
  logCustomRasterGlyph(largest.path.c_str(), directoryData.get(), directorySize, metrics, glyphKanji, "kanji_day",
                       customScratch.get(), RASTER_SCRATCH_BYTES);
  logCustomRasterGlyph(largest.path.c_str(), directoryData.get(), directorySize, metrics, glyphPunct, "fullwidth_punct",
                       customScratch.get(), RASTER_SCRATCH_BYTES);
  logMemory("after custom raster");
}

void logProbeStatus(const char* label) {
  LOG_INF("TTFP", "%s status display=%d %ux%u buffer=%lu storage=%d sfnt=%d tables=%u stb_link=%d "
                  "sd_found=%d sd_dir=%d sd_full=%d sd_sfnt=%d sd_tables=%u sd_stb=%d sd_stb_skip=%d "
                  "sd_metrics=%d sd_metric_hits=%u/%u sd_raster=%d sd_raster_hits=%u/%u sd_raster_skip=%d "
                  "sd_raster_bytes=%lu sd_custom=%d sd_custom_hits=%u/%u sd_custom_compound=%u "
                  "sd_custom_bytes=%lu sd_size=%llu sd_hash=0x%08lX",
          label,
          probeStatus.displayInitialized ? 1 : 0, probeStatus.displayWidth, probeStatus.displayHeight,
          static_cast<unsigned long>(probeStatus.displayBufferSize), probeStatus.storageReady ? 1 : 0,
          probeStatus.sfntOk ? 1 : 0, probeStatus.sfntTableCount, probeStatus.stbLinked ? 1 : 0,
          probeStatus.sdTtfFound ? 1 : 0, probeStatus.sdTtfDirectoryLoaded ? 1 : 0,
          probeStatus.sdTtfFullBufferLoaded ? 1 : 0, probeStatus.sdTtfSfntOk ? 1 : 0, probeStatus.sdTtfTableCount,
          probeStatus.sdTtfStbOk ? 1 : 0, probeStatus.sdTtfStbSkippedLarge ? 1 : 0,
          probeStatus.sdTtfMetricsOk ? 1 : 0, probeStatus.sdTtfMetricHits, probeStatus.sdTtfMetricQueries,
          probeStatus.sdTtfRasterOk ? 1 : 0, probeStatus.sdTtfRasterHits, probeStatus.sdTtfRasterQueries,
          probeStatus.sdTtfRasterSkippedFullBuffer ? 1 : 0, static_cast<unsigned long>(probeStatus.sdTtfRasterBytes),
          probeStatus.sdTtfCustomRasterOk ? 1 : 0, probeStatus.sdTtfCustomRasterHits,
          probeStatus.sdTtfCustomRasterQueries, probeStatus.sdTtfCustomRasterCompounds,
          static_cast<unsigned long>(probeStatus.sdTtfCustomRasterBytes),
          static_cast<unsigned long long>(probeStatus.sdTtfSize), static_cast<unsigned long>(probeStatus.sdTtfHash));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  LOG_INF("TTFP", "CrossPoint TTF probe firmware boot");
  LOG_INF("TTFP", "reset_reason=%d cpu_mhz=%u sdk=%s", static_cast<int>(esp_reset_reason()),
          static_cast<unsigned>(getCpuFrequencyMhz()), ESP.getSdkVersion());

  logMemory("before init");

  gpio.begin();
  const auto& board = gpio.getBoardProfile();
  LOG_INF("TTFP", "board=%s soc=%s psram=%d display=%ux%u", board.label, socFamilyName(board.socFamily),
          board.hasPsram ? 1 : 0, board.displayWidth, board.displayHeight);

  display.begin();
  probeStatus.displayInitialized = true;
  probeStatus.displayWidth = display.getDisplayWidth();
  probeStatus.displayHeight = display.getDisplayHeight();
  probeStatus.displayBufferSize = display.getBufferSize();
  LOG_INF("TTFP", "display initialized width=%u height=%u buffer=%lu", display.getDisplayWidth(),
          display.getDisplayHeight(), static_cast<unsigned long>(display.getBufferSize()));

  probeStatus.storageReady = Storage.begin();
  LOG_INF("TTFP", "storage ready=%d", probeStatus.storageReady ? 1 : 0);

  runTtfRuntimeSmoke();
  scanAndProbeSdTtfReadOnly();
  logProbeStatus("initial");
  logMemory("after init");
  LOG_INF("TTFP", "probe milestone 6A active; SD access is read-only in this firmware");
}

void loop() {
  static uint32_t lastLog = 0;
  static uint32_t lastStatus = 0;
  const uint32_t now = millis();
  if (lastLog == 0 || now - lastLog >= LOG_INTERVAL_MS) {
    lastLog = now;
    logMemory("heartbeat");
  }
  if (lastStatus == 0 || now - lastStatus >= STATUS_INTERVAL_MS) {
    lastStatus = now;
    logProbeStatus("heartbeat");
  }
  delay(100);
}

#endif
