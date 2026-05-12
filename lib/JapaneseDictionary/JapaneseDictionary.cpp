#include "JapaneseDictionary.h"

#include <Arduino.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr size_t KEY_BYTES = 96;
constexpr size_t RECORD_BYTES = 124;
constexpr size_t BUCKET_BYTES = 8;
constexpr uint32_t UNICODE_BUCKETS = 0x110000;
constexpr const char* TELEMETRY_PATH = "/.crosspoint/dict_bench.csv";

uint16_t readLe16(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8); }

uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

int32_t readLeI32(const uint8_t* p) { return static_cast<int32_t>(readLe32(p)); }

std::vector<std::string> contextPrefixes(const std::string& context, const size_t maxChars) {
  std::vector<std::string> prefixes;
  prefixes.reserve(std::min<size_t>(maxChars, 24));
  const auto* start = reinterpret_cast<const unsigned char*>(context.c_str());
  const auto* p = start;
  size_t chars = 0;
  while (*p != '\0' && chars < maxChars) {
    utf8NextCodepoint(&p);
    prefixes.emplace_back(reinterpret_cast<const char*>(start), p - start);
    ++chars;
  }
  return prefixes;
}

std::string csvSafePrefix(const std::string& value, const size_t maxBytes) {
  std::string out;
  out.reserve(std::min(value.size(), maxBytes));
  for (const char ch : value) {
    if (out.size() >= maxBytes) break;
    if (ch == ',' || ch == '\n' || ch == '\r') {
      out.push_back(' ');
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

}  // namespace

struct JapaneseDictionary::Record {
  std::string key;
  uint32_t readingOffset = 0;
  uint16_t readingLength = 0;
  uint32_t definitionOffset = 0;
  uint32_t definitionLength = 0;
  int32_t score = 0;
  int32_t sequence = 0;
  uint8_t tier = 0;
};

bool JapaneseDictionary::openDefault() {
  static constexpr const char* paths[] = {
      "/.crosspoint/dicts/jitendex-cpdict-ranked",
      "/.crosspoint/dicts/jitendex-cpdict-modern",
      "/dict/jitendex-cpdict-ranked",
      "/dict/jitendex-cpdict-modern",
      "/jitendex-cpdict-ranked",
      "/jitendex-cpdict-modern",
  };

  for (const char* path : paths) {
    if (openAt(path)) return true;
  }
  return false;
}

bool JapaneseDictionary::openAt(const char* path) {
  FsFile buckets;
  FsFile records;
  FsFile strings;
  const std::string base(path);
  if (!Storage.openFileForRead("JPD", base + "/buckets.bin", buckets)) return false;
  if (!Storage.openFileForRead("JPD", base + "/records.bin", records)) return false;
  if (!Storage.openFileForRead("JPD", base + "/strings.bin", strings)) return false;

  basePath = base;
  bucketsFile = std::move(buckets);
  recordsFile = std::move(records);
  stringsFile = std::move(strings);
  LOG_INF("JPD", "Opened Japanese dictionary at %s", basePath.c_str());
  return true;
}

bool JapaneseDictionary::readBucket(const uint32_t cp, uint32_t& start, uint32_t& count) {
  if (cp >= UNICODE_BUCKETS) return false;
  uint8_t buf[BUCKET_BYTES];
  if (!bucketsFile.seek(cp * BUCKET_BYTES)) return false;
  if (bucketsFile.read(buf, sizeof(buf)) != static_cast<int>(sizeof(buf))) return false;
  start = readLe32(buf);
  count = readLe32(buf + 4);
  return true;
}

bool JapaneseDictionary::readRecord(const uint32_t index, Record& record) {
  uint8_t buf[RECORD_BYTES];
  if (!recordsFile.seek(index * RECORD_BYTES)) return false;
  if (recordsFile.read(buf, sizeof(buf)) != static_cast<int>(sizeof(buf))) return false;

  const uint16_t keyLen = std::min<uint16_t>(readLe16(buf + KEY_BYTES), KEY_BYTES);
  record.key.assign(reinterpret_cast<const char*>(buf), keyLen);
  record.tier = buf[98];
  record.readingOffset = readLe32(buf + 100);
  record.readingLength = readLe16(buf + 104);
  record.definitionOffset = readLe32(buf + 108);
  record.definitionLength = readLe32(buf + 112);
  record.score = readLeI32(buf + 116);
  record.sequence = readLeI32(buf + 120);
  return true;
}

std::string JapaneseDictionary::readString(const uint32_t offset, const uint32_t length) {
  if (length == 0) return "";
  std::string result;
  result.resize(length);
  if (!stringsFile.seek(offset)) return "";
  if (stringsFile.read(result.data(), length) != length) return "";
  return result;
}

bool JapaneseDictionary::findExact(const std::string& term, std::vector<JapaneseDictionaryMatch>& outMatches,
                                   const size_t maxMatches, uint32_t* bucketUs, uint32_t* searchUs,
                                   uint32_t* stringUs) {
  if (term.empty() || outMatches.size() >= maxMatches) return true;

  const auto* p = reinterpret_cast<const unsigned char*>(term.c_str());
  const uint32_t firstCp = utf8NextCodepoint(&p);
  uint32_t start = 0;
  uint32_t count = 0;
  const uint32_t tBucket = micros();
  const bool bucketOk = readBucket(firstCp, start, count);
  if (bucketUs) *bucketUs += micros() - tBucket;
  if (!bucketOk || count == 0) return true;

  uint32_t lo = start;
  uint32_t hi = start + count;
  Record rec;
  const uint32_t tSearch = micros();
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (!readRecord(mid, rec)) return false;
    if (rec.key < term) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (searchUs) *searchUs += micros() - tSearch;

  for (uint32_t pos = lo; pos < start + count && outMatches.size() < maxMatches; ++pos) {
    const uint32_t tRecord = micros();
    if (!readRecord(pos, rec)) return false;
    if (searchUs) *searchUs += micros() - tRecord;
    if (rec.key != term) break;
    JapaneseDictionaryMatch match;
    match.term = rec.key;
    const uint32_t tString = micros();
    match.reading = readString(rec.readingOffset, rec.readingLength);
    match.definition =
        readString(rec.definitionOffset, static_cast<uint16_t>(std::min<uint32_t>(rec.definitionLength, 512)));
    if (stringUs) *stringUs += micros() - tString;
    match.score = rec.score;
    match.sequence = rec.sequence;
    match.tier = rec.tier;
    outMatches.push_back(std::move(match));
  }

  return true;
}

bool JapaneseDictionary::lookupContext(const std::string& context, std::vector<JapaneseDictionaryMatch>& outMatches,
                                       JapaneseDictionaryTelemetry* telemetry, const size_t maxMatches,
                                       const size_t maxPrefixChars) {
  if (telemetry) *telemetry = JapaneseDictionaryTelemetry();
  const uint32_t tTotal = micros();
  const uint32_t tOpen = micros();
  const bool opened = openDefault();
  if (telemetry) {
    telemetry->openUs = micros() - tOpen;
    telemetry->opened = opened;
  }
  if (!opened) {
    if (telemetry) telemetry->totalUs = micros() - tTotal;
    return false;
  }

  outMatches.clear();
  const auto prefixes = contextPrefixes(context, maxPrefixChars);
  uint32_t bucketUs = 0;
  uint32_t searchUs = 0;
  uint32_t stringUs = 0;

  for (auto it = prefixes.rbegin(); it != prefixes.rend() && outMatches.size() < maxMatches; ++it) {
    const size_t before = outMatches.size();
    findExact(*it, outMatches, maxMatches, &bucketUs, &searchUs, &stringUs);
    (void)before;
  }

  if (telemetry) {
    telemetry->bucketUs = bucketUs;
    telemetry->searchUs = searchUs;
    telemetry->stringUs = stringUs;
    telemetry->totalUs = micros() - tTotal;
    telemetry->matches = static_cast<uint16_t>(outMatches.size());
  }
  return true;
}

void JapaneseDictionary::appendTelemetryCsv(const std::string& query, const std::string& dictPath,
                                            const JapaneseDictionaryTelemetry& telemetry) {
  Storage.ensureDirectoryExists("/.crosspoint");
  const bool exists = Storage.exists(TELEMETRY_PATH);
  FsFile file = Storage.open(TELEMETRY_PATH, O_WRITE | O_CREAT | O_APPEND);
  if (!file) return;

  if (!exists) {
    static constexpr const char* header =
        "query,dict_path,opened,open_us,bucket_us,search_us,string_us,total_us,matches\n";
    file.write(header, strlen(header));
  }

  char line[256];
  const std::string safeQuery = csvSafePrefix(query, 48);
  const std::string safePath = csvSafePrefix(dictPath, 64);
  snprintf(line, sizeof(line), "%s,%s,%u,%lu,%lu,%lu,%lu,%lu,%u\n", safeQuery.c_str(), safePath.c_str(),
           telemetry.opened ? 1 : 0, static_cast<unsigned long>(telemetry.openUs),
           static_cast<unsigned long>(telemetry.bucketUs), static_cast<unsigned long>(telemetry.searchUs),
           static_cast<unsigned long>(telemetry.stringUs), static_cast<unsigned long>(telemetry.totalUs),
           telemetry.matches);
  file.write(line, strlen(line));
  file.close();
}
