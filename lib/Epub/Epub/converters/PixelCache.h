#pragma once

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <stdint.h>

#include <cstdlib>
#include <cstring>
#include <string>

// Banded streaming cache writer for 2-bit pixels (4 levels). Packs 4 pixels per byte,
// MSB first.
//
// The .pxc file is written incrementally in small row bands rather than holding
// the whole decoded image in one heap buffer. It is intended for decoders that
// emit rows monotonically, such as PNG. JPEG uses SeekablePixelCache below
// because its callback order is not treated as strictly row-sequential here.
struct PixelCache {
  uint8_t* buffer;   // band buffer: (bandRows + 1) rows; last row kept zeroed
  uint8_t* zeroRow;  // points at the spare zeroed row, for gap/clip fill
  int width;
  int height;
  int bytesPerRow;
  int originX;      // config.x - to convert screen coords to cache coords
  int originY;      // config.y
  int bandRows;     // rows held in the band buffer
  int bandStart;    // image-local row index of band buffer row 0
  int flushedRows;  // image-local rows already written to file
  HalFile file;
  std::string cachePathStr;
  bool ok;

  PixelCache()
      : buffer(nullptr),
        zeroRow(nullptr),
        width(0),
        height(0),
        bytesPerRow(0),
        originX(0),
        originY(0),
        bandRows(0),
        bandStart(0),
        flushedRows(0),
        ok(false) {}
  PixelCache(const PixelCache&) = delete;
  PixelCache& operator=(const PixelCache&) = delete;

  static constexpr int MIN_BAND_ROWS = 16;
  static constexpr size_t MAX_BAND_BYTES = 24 * 1024;  // band working-set ceiling

  // Open the cache file, write the header, and allocate a band buffer big enough
  // to hold the tallest single decode block (maxBlockDstRows output rows).
  bool begin(const std::string& cachePath, int w, int h, int ox, int oy, int maxBlockDstRows) {
    width = w;
    height = h;
    originX = ox;
    originY = oy;
    bytesPerRow = (w + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
    bandStart = 0;
    flushedRows = 0;
    ok = false;

    int wantRows = maxBlockDstRows + 2;
    if (wantRows < MIN_BAND_ROWS) wantRows = MIN_BAND_ROWS;
    if (wantRows > h) wantRows = h;

    size_t maxRowsByMem = MAX_BAND_BYTES / (size_t)bytesPerRow;
    if (maxRowsByMem < 1) maxRowsByMem = 1;
    if ((size_t)wantRows > maxRowsByMem) wantRows = (int)maxRowsByMem;

    // A single decode block must fit inside the band, otherwise streaming would
    // drop rows. This only fails for pathological upscales that could not be
    // cached at all; fall back to the no-cache path.
    if (wantRows < maxBlockDstRows) {
      LOG_ERR("IMG", "Cache band too small (%d < %d rows) for %dx%d", wantRows, maxBlockDstRows, w, h);
      return false;
    }
    bandRows = wantRows;

    const size_t bufSize = (size_t)(bandRows + 1) * bytesPerRow;  // +1 spare zero row
    buffer = (uint8_t*)malloc(bufSize);
    if (!buffer) {
      LOG_ERR("IMG", "OOM cache band: %u bytes", (unsigned)bufSize);
      return false;
    }
    memset(buffer, 0, bufSize);
    zeroRow = buffer + (size_t)bandRows * bytesPerRow;

    if (!Storage.openFileForWrite("IMG", cachePath, file)) {
      LOG_ERR("IMG", "Failed to open cache file for writing: %s", cachePath.c_str());
      free(buffer);
      buffer = nullptr;
      return false;
    }
    cachePathStr = cachePath;

    uint16_t w16 = (uint16_t)w;
    uint16_t h16 = (uint16_t)h;
    if (file.write(&w16, 2) != 2 || file.write(&h16, 2) != 2) {
      LOG_ERR("IMG", "Failed to write cache header: %s", cachePath.c_str());
      abort();
      return false;
    }

    LOG_DBG("IMG", "Cache stream started: %s (%dx%d, band %d rows)", cachePath.c_str(), w, h, bandRows);
    ok = true;
    return true;
  }

  // Flush every output row below newTopRow (they are final in raster order) and
  // reposition the band to start at newTopRow. Returns false if a write failed,
  // in which case the caller must stop caching for the rest of the decode.
  bool advanceTo(int newTopRow) {
    if (!ok) return false;
    if (newTopRow <= bandStart) return true;
    if (newTopRow > height) newTopRow = height;

    for (int r = bandStart; r < newTopRow; ++r) {
      const int idx = r - bandStart;
      const uint8_t* rowPtr = (idx < bandRows) ? (buffer + (size_t)idx * bytesPerRow) : zeroRow;
      if (file.write(rowPtr, (size_t)bytesPerRow) != (size_t)bytesPerRow) {
        LOG_ERR("IMG", "Cache write error at row %d", r);
        ok = false;
        return false;
      }
    }
    flushedRows = newTopRow;
    bandStart = newTopRow;
    memset(buffer, 0, (size_t)bandRows * bytesPerRow);  // fresh band (gaps stay black)
    return true;
  }

  // Flush the final band and zero-fill any rows never covered (image clipped by
  // the screen), then close the file.
  bool finalize() {
    if (!ok) {
      abort();
      return false;
    }
    for (int r = flushedRows; r < height; ++r) {
      const int idx = r - bandStart;
      const uint8_t* rowPtr = (idx >= 0 && idx < bandRows) ? (buffer + (size_t)idx * bytesPerRow) : zeroRow;
      if (file.write(rowPtr, (size_t)bytesPerRow) != (size_t)bytesPerRow) {
        LOG_ERR("IMG", "Cache write error at row %d", r);
        abort();
        return false;
      }
    }
    file.close();
    LOG_DBG("IMG", "Cache written: %s (%dx%d, %d bytes)", cachePathStr.c_str(), width, height,
            4 + bytesPerRow * height);
    ok = false;  // file handed off; nothing left to clean up
    return true;
  }

  // Drop a partial/failed cache so a later decode re-creates it cleanly.
  void abort() {
    if (file.isOpen()) file.close();
    if (!cachePathStr.empty()) {
      Storage.remove(cachePathStr.c_str());
    }
    ok = false;
  }

  ~PixelCache() {
    if (file.isOpen()) {
      // The file is still open, so neither finalize() nor abort() ran, or a
      // mid-stream write failed (advanceTo() cleared ok but left the file open).
      // Drop the partial cache so we leave no corrupt file behind.
      abort();
    }
    if (buffer) {
      free(buffer);
      buffer = nullptr;
    }
  }
};

struct SeekablePixelCache {
  HalFile file;
  std::string path;
  uint8_t* rowBuffer;
  int width;
  int height;
  int bytesPerRow;
  int currentRow;
  bool active;
  bool failed;
  bool rowDirty;

  SeekablePixelCache()
      : rowBuffer(nullptr),
        width(0),
        height(0),
        bytesPerRow(0),
        currentRow(-1),
        active(false),
        failed(false),
        rowDirty(false) {}

  SeekablePixelCache(const SeekablePixelCache&) = delete;
  SeekablePixelCache& operator=(const SeekablePixelCache&) = delete;

  bool begin(const std::string& cachePath, int w, int h) {
    path = cachePath;
    width = w;
    height = h;
    bytesPerRow = (w + 3) / 4;
    currentRow = -1;
    rowDirty = false;

    rowBuffer = static_cast<uint8_t*>(malloc(bytesPerRow));
    if (!rowBuffer) {
      failed = true;
      return false;
    }

    Storage.remove(path.c_str());
    file = Storage.open(path.c_str(), O_RDWR | O_CREAT);
    if (!file) {
      free(rowBuffer);
      rowBuffer = nullptr;
      failed = true;
      return false;
    }

    uint16_t headerWidth = width;
    uint16_t headerHeight = height;
    if (file.write(&headerWidth, 2) != 2 || file.write(&headerHeight, 2) != 2) {
      fail();
      return false;
    }

    memset(rowBuffer, 0, bytesPerRow);
    for (int row = 0; row < height; row++) {
      if (file.write(rowBuffer, bytesPerRow) != static_cast<size_t>(bytesPerRow)) {
        fail();
        return false;
      }
    }

    active = true;
    return true;
  }

  bool beginRow(int row) {
    if (!active || failed || row < 0 || row >= height) return false;
    if (row == currentRow) return true;
    if (!flushCurrentRow()) return false;

    if (!file.seek(rowOffset(row)) || file.read(rowBuffer, bytesPerRow) != bytesPerRow) {
      fail();
      return false;
    }

    currentRow = row;
    rowDirty = false;
    return true;
  }

  void writePixel(int x, uint8_t value) {
    if (!active || failed || !rowBuffer || currentRow < 0 || x < 0 || x >= width) return;
    const int byteIdx = x >> 2;
    const int bitShift = 6 - (x & 3) * 2;
    rowBuffer[byteIdx] = (rowBuffer[byteIdx] & ~(0x03 << bitShift)) | ((value & 0x03) << bitShift);
    rowDirty = true;
  }

  bool finish() {
    if (!active || failed) return false;
    if (!flushCurrentRow()) return false;

    file.close();
    active = false;
    LOG_DBG("IMG", "Seekable cache written: %s (%dx%d, %d bytes)", path.c_str(), width, height,
            4 + bytesPerRow * height);
    return true;
  }

  void fail() {
    failed = true;
    active = false;
    if (file) {
      file.close();
    }
    if (!path.empty()) {
      Storage.remove(path.c_str());
    }
  }

  void abort() { fail(); }

  ~SeekablePixelCache() {
    if (active) {
      fail();
    }
    if (rowBuffer) {
      free(rowBuffer);
      rowBuffer = nullptr;
    }
  }

 private:
  size_t rowOffset(int row) const { return 4 + static_cast<size_t>(row) * bytesPerRow; }

  bool flushCurrentRow() {
    if (currentRow < 0) return true;
    if (rowDirty) {
      if (!file.seek(rowOffset(currentRow)) || file.write(rowBuffer, bytesPerRow) != static_cast<size_t>(bytesPerRow)) {
        fail();
        return false;
      }
    }
    currentRow = -1;
    rowDirty = false;
    return true;
  }
};
