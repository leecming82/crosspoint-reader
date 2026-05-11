#pragma once

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <stdint.h>

#include <cstdio>
#include <cstring>
#include <string>

// Cache buffer for storing 2-bit pixels (4 levels) during decode.
// Packs 4 pixels per byte, MSB first.
struct PixelCache {
  uint8_t* buffer;
  int width;
  int height;
  int bytesPerRow;
  int originX;  // config.x - to convert screen coords to cache coords
  int originY;  // config.y

  PixelCache() : buffer(nullptr), width(0), height(0), bytesPerRow(0), originX(0), originY(0) {}
  PixelCache(const PixelCache&) = delete;
  PixelCache& operator=(const PixelCache&) = delete;

  static constexpr size_t MAX_CACHE_BYTES = 256 * 1024;  // 256KB limit for embedded targets

  bool allocate(int w, int h, int ox, int oy) {
    width = w;
    height = h;
    originX = ox;
    originY = oy;
    bytesPerRow = (w + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
    size_t bufferSize = (size_t)bytesPerRow * h;
    if (bufferSize > MAX_CACHE_BYTES) {
      LOG_ERR("IMG", "Cache buffer too large: %d bytes for %dx%d (limit %d)", bufferSize, w, h, MAX_CACHE_BYTES);
      return false;
    }
    buffer = (uint8_t*)malloc(bufferSize);
    if (buffer) {
      memset(buffer, 0, bufferSize);
      LOG_DBG("IMG", "Allocated cache buffer: %d bytes for %dx%d", bufferSize, w, h);
    }
    return buffer != nullptr;
  }

  void setPixel(int screenX, int screenY, uint8_t value) {
    if (!buffer) return;
    int localX = screenX - originX;
    int localY = screenY - originY;
    if (localX < 0 || localX >= width || localY < 0 || localY >= height) return;

    int byteIdx = localY * bytesPerRow + localX / 4;
    int bitShift = 6 - (localX % 4) * 2;  // MSB first: pixel 0 at bits 6-7
    buffer[byteIdx] = (buffer[byteIdx] & ~(0x03 << bitShift)) | ((value & 0x03) << bitShift);
  }

  bool writeToFile(const std::string& cachePath) {
    if (!buffer) return false;

    FsFile cacheFile;
    if (!Storage.openFileForWrite("IMG", cachePath, cacheFile)) {
      LOG_ERR("IMG", "Failed to open cache file for writing: %s", cachePath.c_str());
      return false;
    }

    uint16_t w = width;
    uint16_t h = height;
    cacheFile.write(&w, 2);
    cacheFile.write(&h, 2);
    cacheFile.write(buffer, bytesPerRow * height);
    cacheFile.close();

    LOG_DBG("IMG", "Cache written: %s (%dx%d, %d bytes)", cachePath.c_str(), width, height, 4 + bytesPerRow * height);
    return true;
  }

  ~PixelCache() {
    if (buffer) {
      free(buffer);
      buffer = nullptr;
    }
  }
};

struct StreamingPixelCache {
  FsFile file;
  std::string path;
  uint8_t* rowBuffer;
  int width;
  int height;
  int bytesPerRow;
  int currentRow;
  int nextRowToWrite;
  bool active;
  bool failed;

  StreamingPixelCache()
      : rowBuffer(nullptr),
        width(0),
        height(0),
        bytesPerRow(0),
        currentRow(-1),
        nextRowToWrite(0),
        active(false),
        failed(false) {}

  StreamingPixelCache(const StreamingPixelCache&) = delete;
  StreamingPixelCache& operator=(const StreamingPixelCache&) = delete;

  bool begin(const std::string& cachePath, int w, int h) {
    path = cachePath;
    width = w;
    height = h;
    bytesPerRow = (w + 3) / 4;
    rowBuffer = static_cast<uint8_t*>(malloc(bytesPerRow));
    if (!rowBuffer) {
      failed = true;
      return false;
    }

    if (!Storage.openFileForWrite("IMG", cachePath, file)) {
      free(rowBuffer);
      rowBuffer = nullptr;
      failed = true;
      return false;
    }

    uint16_t headerWidth = width;
    uint16_t headerHeight = height;
    file.write(&headerWidth, 2);
    file.write(&headerHeight, 2);
    active = true;
    return true;
  }

  bool beginRow(int row) {
    if (!active || failed) return false;
    if (row == currentRow) return true;
    if (row < currentRow || row < nextRowToWrite) {
      fail();
      return false;
    }

    if (!flushCurrentRow()) return false;

    memset(rowBuffer, 0, bytesPerRow);
    while (nextRowToWrite < row) {
      if (file.write(rowBuffer, bytesPerRow) != static_cast<size_t>(bytesPerRow)) {
        fail();
        return false;
      }
      nextRowToWrite++;
    }

    currentRow = row;
    memset(rowBuffer, 0, bytesPerRow);
    return true;
  }

  void writePixel(int x, uint8_t value) {
    if (!active || failed || !rowBuffer || x < 0 || x >= width) return;
    const int byteIdx = x >> 2;
    const int bitShift = 6 - (x & 3) * 2;
    rowBuffer[byteIdx] = (rowBuffer[byteIdx] & ~(0x03 << bitShift)) | ((value & 0x03) << bitShift);
  }

  bool finish() {
    if (!active || failed) return false;
    if (!flushCurrentRow()) return false;

    memset(rowBuffer, 0, bytesPerRow);
    while (nextRowToWrite < height) {
      if (file.write(rowBuffer, bytesPerRow) != static_cast<size_t>(bytesPerRow)) {
        fail();
        return false;
      }
      nextRowToWrite++;
    }

    file.close();
    active = false;

    LOG_DBG("IMG", "Cache written: %s (%dx%d, %d bytes)", path.c_str(), width, height, 4 + bytesPerRow * height);
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

  ~StreamingPixelCache() {
    if (active) {
      fail();
    }
    if (rowBuffer) {
      free(rowBuffer);
      rowBuffer = nullptr;
    }
  }

 private:
  bool flushCurrentRow() {
    if (currentRow < 0) return true;
    if (file.write(rowBuffer, bytesPerRow) != static_cast<size_t>(bytesPerRow)) {
      fail();
      return false;
    }
    nextRowToWrite = currentRow + 1;
    currentRow = -1;
    return true;
  }
};
