#include "TtfProbe.h"

namespace ttf {
namespace {

constexpr size_t SFNT_HEADER_SIZE = 12;
constexpr size_t TABLE_RECORD_SIZE = 16;

uint16_t readU16BE(const uint8_t* p) { return (static_cast<uint16_t>(p[0]) << 8) | p[1]; }

uint32_t readU32BE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

bool rangeFits(uint64_t len, uint32_t offset, uint32_t size) {
  return offset <= len && size <= len - offset;
}

void markTable(ProbeResult& result, uint32_t tableTag) {
  switch (tableTag) {
    case tag('c', 'm', 'a', 'p'):
      result.hasCmap = true;
      break;
    case tag('g', 'l', 'y', 'f'):
      result.hasGlyf = true;
      break;
    case tag('h', 'e', 'a', 'd'):
      result.hasHead = true;
      break;
    case tag('h', 'h', 'e', 'a'):
      result.hasHhea = true;
      break;
    case tag('h', 'm', 't', 'x'):
      result.hasHmtx = true;
      break;
    case tag('l', 'o', 'c', 'a'):
      result.hasLoca = true;
      break;
    case tag('m', 'a', 'x', 'p'):
      result.hasMaxp = true;
      break;
    case tag('k', 'e', 'r', 'n'):
      result.hasKern = true;
      break;
    default:
      break;
  }
}

}  // namespace

ProbeResult probeSfntDirectory(const uint8_t* data, const size_t len, const uint64_t fileSize) {
  ProbeResult result{};
  if (!data || len < SFNT_HEADER_SIZE) {
    result.error = "short sfnt header";
    return result;
  }

  const uint32_t scalerType = readU32BE(data);
  if (scalerType == tag('t', 't', 'c', 'f')) {
    result.isCollection = true;
    result.error = "ttc collection not supported";
    return result;
  }

  if (scalerType != 0x00010000u && scalerType != tag('t', 'r', 'u', 'e')) {
    result.error = "not a truetype sfnt";
    return result;
  }

  result.tableCount = readU16BE(data + 4);
  const size_t directoryBytes = SFNT_HEADER_SIZE + static_cast<size_t>(result.tableCount) * TABLE_RECORD_SIZE;
  if (directoryBytes > len) {
    result.error = "short table directory";
    return result;
  }

  for (uint16_t i = 0; i < result.tableCount; ++i) {
    const uint8_t* record = data + SFNT_HEADER_SIZE + static_cast<size_t>(i) * TABLE_RECORD_SIZE;
    TableRecord table{};
    table.tag = readU32BE(record);
    table.checksum = readU32BE(record + 4);
    table.offset = readU32BE(record + 8);
    table.length = readU32BE(record + 12);
    if (!rangeFits(fileSize, table.offset, table.length)) {
      result.error = "table outside file";
      return result;
    }
    markTable(result, table.tag);
  }

  if (!result.hasCmap || !result.hasGlyf || !result.hasHead || !result.hasHhea || !result.hasHmtx ||
      !result.hasLoca || !result.hasMaxp) {
    result.error = "missing required truetype table";
    return result;
  }

  result.ok = true;
  return result;
}

ProbeResult probeSfnt(const uint8_t* data, const size_t len) { return probeSfntDirectory(data, len, len); }

}  // namespace ttf
