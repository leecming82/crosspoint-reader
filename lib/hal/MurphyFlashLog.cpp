#include "MurphyFlashLog.h"

#include <Arduino.h>
#include <esp_partition.h>
#include <esp_system.h>

#include <cstring>

namespace MurphyFlashLog {

namespace {

constexpr uint32_t LOG_MAGIC = 0x474F4C46;  // FLOG
constexpr uint32_t LOG_VERSION = 1;
constexpr uint32_t LOG_REGION_SIZE = 64 * 1024;
constexpr uint32_t LOG_DATA_OFFSET = 4096;

struct FlashLogHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t regionSize;
  uint32_t dataOffset;
  uint32_t resetReason;
  uint32_t bootMillis;
  uint32_t app1Address;
  uint32_t app1Size;
  char banner[32];
};

const esp_partition_t* logPartition = nullptr;
uint32_t writeOffset = LOG_DATA_OFFSET;
bool initialized = false;

uint32_t paddedLength(size_t length) { return (static_cast<uint32_t>(length) + 3U) & ~3U; }

}  // namespace

bool begin() {
  logPartition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  if (!logPartition || logPartition->size < LOG_REGION_SIZE) {
    initialized = false;
    return false;
  }

  if (esp_partition_erase_range(logPartition, 0, LOG_REGION_SIZE) != ESP_OK) {
    initialized = false;
    return false;
  }

  FlashLogHeader header = {};
  header.magic = LOG_MAGIC;
  header.version = LOG_VERSION;
  header.regionSize = LOG_REGION_SIZE;
  header.dataOffset = LOG_DATA_OFFSET;
  header.resetReason = static_cast<uint32_t>(esp_reset_reason());
  header.bootMillis = millis();
  header.app1Address = static_cast<uint32_t>(logPartition->address);
  header.app1Size = static_cast<uint32_t>(logPartition->size);
  snprintf(header.banner, sizeof(header.banner), "murphy-m4 flash log");

  if (esp_partition_write(logPartition, 0, &header, sizeof(header)) != ESP_OK) {
    initialized = false;
    return false;
  }

  writeOffset = LOG_DATA_OFFSET;
  initialized = true;
  return true;
}

bool ready() { return initialized; }

bool append(const char* data, size_t length) {
  if (!initialized || !data || length == 0) {
    return false;
  }

  constexpr size_t MAX_CHUNK = 252;
  while (length > 0) {
    const size_t chunkLen = length > MAX_CHUNK ? MAX_CHUNK : length;
    char chunk[MAX_CHUNK + 4] = {};
    memcpy(chunk, data, chunkLen);

    const uint32_t writeLen = paddedLength(chunkLen);
    if (writeOffset + writeLen > LOG_REGION_SIZE) {
      initialized = false;
      return false;
    }

    if (esp_partition_write(logPartition, writeOffset, chunk, writeLen) != ESP_OK) {
      initialized = false;
      return false;
    }

    writeOffset += writeLen;
    data += chunkLen;
    length -= chunkLen;
  }

  return true;
}

bool append(const char* line) {
  if (!line) {
    return false;
  }
  return append(line, strnlen(line, 256));
}

}  // namespace MurphyFlashLog
