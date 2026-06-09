#include "JapaneseDictionary.h"

#include "JapaneseDeinflector.h"

#include <Utf8.h>

#include <algorithm>
#include <cstring>

namespace {
std::string joinPath(const char* base, const char* leaf) {
  std::string path(base);
  if (!path.empty() && path.back() != '/') {
    path += '/';
  }
  path += leaf;
  return path;
}

void addMissingIfAbsent(const char* base, const char* leaf, std::vector<std::string>& missingFiles) {
  const std::string path = joinPath(base, leaf);
  if (!Storage.exists(path.c_str())) {
    missingFiles.push_back(path);
  }
}

uint16_t readLe16(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8); }

uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

int32_t readLeI32(const uint8_t* p) { return static_cast<int32_t>(readLe32(p)); }

uint32_t fnv1a32(const char* data, uint32_t seed) {
  uint32_t value = seed;
  while (*data != '\0') {
    value ^= static_cast<uint8_t>(*data);
    value *= 0x01000193UL;
    ++data;
  }
  return value;
}

bool isJapaneseDictionaryTermChar(const uint32_t cp) {
  if (utf8IsJapaneseDictionaryStart(cp)) return true;
  return cp == 0x3005      // 々, ideographic iteration mark
         || cp == 0x3006   // 〆
         || cp == 0x30FC;  // ー, prolonged sound mark
}

std::vector<std::string> contextPrefixes(const std::string& context, const size_t maxChars) {
  std::vector<std::string> prefixes;
  prefixes.reserve(std::min<size_t>(maxChars, 24));
  const auto* start = reinterpret_cast<const unsigned char*>(context.c_str());
  const auto* p = start;
  size_t chars = 0;
  while (*p != '\0' && chars < maxChars) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (!isJapaneseDictionaryTermChar(cp)) break;
    prefixes.emplace_back(reinterpret_cast<const char*>(start), p - start);
    ++chars;
  }
  return prefixes;
}

bool containsString(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

int32_t sequenceGroupId(const int32_t sequence) { return sequence < 0 ? -sequence : sequence; }

bool hasTermVariant(const std::string& terms, const std::string& term) {
  size_t start = 0;
  while (start <= terms.size()) {
    const size_t end = terms.find("・", start);
    const size_t actualEnd = end == std::string::npos ? terms.size() : end;
    if (terms.substr(start, actualEnd - start) == term) {
      return true;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + strlen("・");
  }
  return false;
}

void appendTermVariant(JapaneseDictionaryMatch& match, const std::string& term) {
  if (term.empty() || hasTermVariant(match.terms, term)) {
    return;
  }
  if (!match.terms.empty()) {
    match.terms += "・";
  }
  match.terms += term;
  if (match.termCount < UINT8_MAX) {
    ++match.termCount;
  }
}

bool mergeMatched(JapaneseDictionaryMatch* matches, const size_t count, const JapaneseDictionaryMatch& candidate) {
  const int32_t candidateGroup = sequenceGroupId(candidate.sequence);
  for (size_t i = 0; i < count; ++i) {
    if (sequenceGroupId(matches[i].sequence) == candidateGroup && matches[i].reading == candidate.reading &&
        matches[i].definition == candidate.definition) {
      appendTermVariant(matches[i], candidate.term);
      return true;
    }
  }
  return false;
}
}  // namespace

uint32_t firstUtf8Codepoint(const std::string& text) {
  if (text.empty()) {
    return 0;
  }
  const auto* data = reinterpret_cast<const unsigned char*>(text.c_str());
  return utf8NextCodepoint(&data);
}

JapaneseDictionaryBundleStatus JapaneseDictionary::validateDefaultBundle() {
  return validateBundleAt(DEFAULT_JPDICT_PATH, DEFAULT_KANJI_PATH);
}

JapaneseDictionaryBundleStatus JapaneseDictionary::validateBundleAt(const char* jpdictPath, const char* kanjiPath) {
  JapaneseDictionaryBundleStatus status;
  status.storageReady = Storage.ready();
  if (!status.storageReady) {
    return status;
  }

  addMissingIfAbsent(jpdictPath, "manifest.json", status.missingFiles);
  addMissingIfAbsent(jpdictPath, "buckets.bin", status.missingFiles);
  addMissingIfAbsent(jpdictPath, "records.bin", status.missingFiles);
  addMissingIfAbsent(jpdictPath, "strings.bin", status.missingFiles);
  addMissingIfAbsent(jpdictPath, "key_filter.bin", status.missingFiles);
  addMissingIfAbsent(kanjiPath, "manifest.json", status.missingFiles);
  addMissingIfAbsent(kanjiPath, "lookup.records.bin", status.missingFiles);
  addMissingIfAbsent(kanjiPath, "lookup.strings.bin", status.missingFiles);

  status.complete = status.missingFiles.empty();
  return status;
}

bool JapaneseDictionary::openDefault() {
  if (isOpen() && basePath == DEFAULT_JPDICT_PATH) {
    return true;
  }
  return openAt(DEFAULT_JPDICT_PATH);
}

bool JapaneseDictionary::openAt(const char* path) {
  close();
  if (!Storage.exists(joinPath(path, "manifest.json").c_str())) return false;
  HalFile buckets;
  HalFile records;
  HalFile strings;
  if (!Storage.openFileForRead("JPD", joinPath(path, "buckets.bin"), buckets)) return false;
  if (!Storage.openFileForRead("JPD", joinPath(path, "records.bin"), records)) return false;
  if (!Storage.openFileForRead("JPD", joinPath(path, "strings.bin"), strings)) return false;

  basePath = path;
  bucketsFile = std::move(buckets);
  recordsFile = std::move(records);
  stringsFile = std::move(strings);
  if (!loadKeyFilter(path)) {
    close();
    return false;
  }
  return true;
}

void JapaneseDictionary::close() {
  keyFilter.clear();
  if (bucketsFile.isOpen()) bucketsFile.close();
  if (recordsFile.isOpen()) recordsFile.close();
  if (stringsFile.isOpen()) stringsFile.close();
  bucketsFile = HalFile();
  recordsFile = HalFile();
  stringsFile = HalFile();
  basePath.clear();
}

bool JapaneseDictionary::isOpen() const {
  return bucketsFile.isOpen() && recordsFile.isOpen() && stringsFile.isOpen() && hasKeyFilter();
}

bool JapaneseDictionary::hasKeyFilter() const { return keyFilter.size() == KEY_FILTER_BYTES; }

bool JapaneseDictionary::loadKeyFilter(const char* path) {
  HalFile filter;
  if (!Storage.openFileForRead("JPD", joinPath(path, "key_filter.bin"), filter)) return false;
  if (filter.fileSize() != KEY_FILTER_BYTES) return false;

  std::vector<uint8_t> data(KEY_FILTER_BYTES);
  const int read = filter.read(data.data(), data.size());
  filter.close();
  if (read != static_cast<int>(data.size())) return false;

  keyFilter = std::move(data);
  return true;
}

bool JapaneseDictionary::keyMightExist(const std::string& key) const {
  if (!hasKeyFilter() || key.empty()) {
    return false;
  }

  const uint32_t bitCount = keyFilter.size() * 8;
  const uint32_t h1 = fnv1a32(key.c_str(), 0x811C9DC5UL);
  const uint32_t h2 = fnv1a32(key.c_str(), 0xCBF29CE4UL) | 1UL;
  for (uint8_t i = 0; i < KEY_FILTER_HASHES; ++i) {
    const uint32_t pos = (static_cast<uint64_t>(h1) + static_cast<uint64_t>(i) * h2) % bitCount;
    if ((keyFilter[pos >> 3] & (1U << (pos & 7))) == 0) {
      return false;
    }
  }
  return true;
}

bool JapaneseDictionary::readBucket(const uint32_t codepoint, uint32_t& start, uint32_t& count) {
  if (!isOpen() || codepoint >= UNICODE_BUCKETS) {
    start = 0;
    count = 0;
    return false;
  }

  uint8_t data[BUCKET_BYTES] = {};
  if (!bucketsFile.seek(codepoint * BUCKET_BYTES) ||
      bucketsFile.read(data, sizeof(data)) != static_cast<int>(sizeof(data))) {
    start = 0;
    count = 0;
    return false;
  }

  start = readLe32(data);
  count = readLe32(data + 4);
  return true;
}

bool JapaneseDictionary::readRecord(const uint32_t index, Record& record) {
  uint8_t data[RECORD_BYTES] = {};
  if (!isOpen() || !recordsFile.seek(index * RECORD_BYTES) ||
      recordsFile.read(data, sizeof(data)) != static_cast<int>(sizeof(data))) {
    return false;
  }

  record.keyLen = readLe16(data + 96);
  if (record.keyLen > KEY_BYTES) {
    return false;
  }
  memcpy(record.key, data, record.keyLen);
  record.key[record.keyLen] = '\0';
  record.tier = data[98];
  record.flags = data[99];
  record.termOffset = readLe32(data + 100);
  record.termLen = readLe16(data + 104);
  record.readingOffset = readLe32(data + 106);
  record.readingLen = readLe16(data + 110);
  record.definitionOffset = readLe32(data + 112);
  record.definitionLen = readLe32(data + 116);
  record.score = readLeI32(data + 120);
  record.sequence = readLeI32(data + 124);
  return true;
}

std::string JapaneseDictionary::readString(const uint32_t offset, const uint32_t length) {
  if (!isOpen() || length == 0 || !stringsFile.seek(offset)) {
    return "";
  }

  std::string out;
  out.reserve(length);
  constexpr size_t CHUNK = 96;
  uint8_t buffer[CHUNK];
  uint32_t remaining = length;
  while (remaining > 0) {
    const size_t wanted = remaining > CHUNK ? CHUNK : remaining;
    const int read = stringsFile.read(buffer, wanted);
    if (read <= 0) break;
    out.append(reinterpret_cast<const char*>(buffer), read);
    remaining -= read;
  }
  return out;
}

bool JapaneseDictionary::lowerBoundInBucket(const std::string& key, uint32_t& bucketStart, uint32_t& bucketCount,
                                            uint32_t& lowerBound) {
  bucketStart = 0;
  bucketCount = 0;
  lowerBound = 0;
  if (!readBucket(firstUtf8Codepoint(key), bucketStart, bucketCount) || bucketCount == 0) {
    return false;
  }

  uint32_t lo = bucketStart;
  uint32_t hi = bucketStart + bucketCount;
  Record record;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (!readRecord(mid, record)) {
      return false;
    }
    const int cmp = strcmp(record.key, key.c_str());
    if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  lowerBound = lo;
  return true;
}

void JapaneseDictionary::populateMatch(const Record& record, const std::string& sourceText,
                                       const uint8_t deinflectionDepth, JapaneseDictionaryMatch& match) {
  match.key = record.key;
  match.term = readString(record.termOffset, record.termLen);
  match.terms = match.term;
  match.reading = readString(record.readingOffset, record.readingLen);
  match.definition = readString(record.definitionOffset, record.definitionLen);
  match.sourceText = sourceText;
  match.score = record.score;
  match.sequence = record.sequence;
  match.tier = record.tier;
  match.flags = record.flags;
  match.deinflectionDepth = deinflectionDepth;
  match.termCount = 1;
}

size_t JapaneseDictionary::appendExactMatches(const std::string& key, const std::string& sourceText,
                                              const uint8_t deinflectionDepth, JapaneseDictionaryMatch* outMatches,
                                              size_t found, const size_t maxMatches) {
  if (!isOpen() || key.empty() || outMatches == nullptr || found >= maxMatches || !keyMightExist(key)) {
    return found;
  }

  uint32_t start = 0;
  uint32_t count = 0;
  uint32_t lo = 0;
  if (!lowerBoundInBucket(key, start, count, lo)) {
    return found;
  }

  for (uint32_t pos = lo; pos < start + count && found < maxMatches; ++pos) {
    Record record;
    if (!readRecord(pos, record)) break;
    if (strcmp(record.key, key.c_str()) != 0) break;

    JapaneseDictionaryMatch candidate;
    populateMatch(record, sourceText, deinflectionDepth, candidate);
    if (!mergeMatched(outMatches, found, candidate)) {
      outMatches[found++] = std::move(candidate);
    }
  }
  return found;
}

bool JapaneseDictionary::beginExactLookup(const std::string& key, JapaneseDictionaryExactCursor& cursor) {
  cursor = JapaneseDictionaryExactCursor{};
  if (!isOpen() || key.empty() || !keyMightExist(key)) {
    return false;
  }

  uint32_t start = 0;
  uint32_t count = 0;
  uint32_t lo = 0;
  if (!lowerBoundInBucket(key, start, count, lo)) {
    return false;
  }

  cursor.key = key;
  cursor.pos = lo;
  cursor.end = start + count;
  cursor.active = true;
  cursor.exhausted = lo >= cursor.end;
  return true;
}

size_t JapaneseDictionary::lookupExactNext(JapaneseDictionaryExactCursor& cursor, JapaneseDictionaryMatch* outMatches,
                                           const size_t maxMatches) {
  if (!isOpen() || !cursor.active || cursor.exhausted || cursor.key.empty() || outMatches == nullptr ||
      maxMatches == 0) {
    return 0;
  }

  size_t found = 0;
  for (uint32_t pos = cursor.pos; pos < cursor.end && found < maxMatches; ++pos) {
    Record record;
    if (!readRecord(pos, record)) {
      cursor.pos = pos;
      cursor.exhausted = true;
      return found;
    }
    if (strcmp(record.key, cursor.key.c_str()) != 0) {
      cursor.pos = pos;
      cursor.exhausted = true;
      return found;
    }

    JapaneseDictionaryMatch candidate;
    populateMatch(record, cursor.key, 0, candidate);
    if (!mergeMatched(outMatches, found, candidate)) {
      outMatches[found++] = std::move(candidate);
    }
    cursor.pos = pos + 1;
  }
  if (cursor.pos >= cursor.end) {
    cursor.exhausted = true;
  }
  return found;
}

size_t JapaneseDictionary::lookupExact(const std::string& key, JapaneseDictionaryMatch* outMatches,
                                       const size_t maxMatches) {
  return appendExactMatches(key, key, 0, outMatches, 0, maxMatches);
}

bool JapaneseDictionary::hasExact(const std::string& key) {
  if (!isOpen() || key.empty() || !keyMightExist(key)) {
    return false;
  }
  uint32_t start = 0;
  uint32_t count = 0;
  uint32_t lo = 0;
  if (!lowerBoundInBucket(key, start, count, lo) || lo >= start + count) {
    return false;
  }
  Record record;
  return readRecord(lo, record) && strcmp(record.key, key.c_str()) == 0;
}

size_t JapaneseDictionary::appendDeinflectedMatches(const std::string& key, JapaneseDictionaryMatch* outMatches,
                                                    size_t found, const size_t maxMatches, const uint8_t minDepth,
                                                    const uint8_t maxDepth) {
  if (!isOpen() || key.empty() || outMatches == nullptr || found >= maxMatches || minDepth > maxDepth) {
    return found;
  }

  const auto candidates = jpdict::expandDeinflections(key, maxDepth, maxMatches * 4 + 8);
  for (const auto& candidate : candidates) {
    if (found >= maxMatches) return found;
    if (candidate.depth < minDepth || candidate.depth > maxDepth || candidate.term == key) continue;
    found = appendExactMatches(candidate.term, key, candidate.depth, outMatches, found, maxMatches);
  }
  return found;
}

size_t JapaneseDictionary::appendPrefixMatches(const std::string& key, JapaneseDictionaryMatch* outMatches,
                                               size_t found, const size_t maxMatches,
                                               const size_t maxPrefixRecords) {
  if (!isOpen() || key.empty() || outMatches == nullptr || found >= maxMatches || maxPrefixRecords == 0) {
    return found;
  }

  uint32_t start = 0;
  uint32_t count = 0;
  uint32_t lo = 0;
  if (!lowerBoundInBucket(key, start, count, lo)) {
    return found;
  }

  const size_t keyLen = key.size();
  size_t scanned = 0;
  for (uint32_t pos = lo; pos < start + count && found < maxMatches && scanned < maxPrefixRecords; ++pos, ++scanned) {
    Record record;
    if (!readRecord(pos, record)) break;
    if (strncmp(record.key, key.c_str(), keyLen) != 0) break;
    if (strcmp(record.key, key.c_str()) == 0) continue;

    JapaneseDictionaryMatch candidate;
    populateMatch(record, record.key, 0, candidate);
    if (!mergeMatched(outMatches, found, candidate)) {
      outMatches[found++] = std::move(candidate);
    }
  }
  return found;
}

size_t JapaneseDictionary::lookupExactThenPrefix(const std::string& key, JapaneseDictionaryMatch* outMatches,
                                                 const size_t maxMatches, const size_t maxPrefixRecords) {
  size_t found = appendExactMatches(key, key, 0, outMatches, 0, maxMatches);
  found = appendDeinflectedMatches(key, outMatches, found, maxMatches, 1, 1);
  if (found < maxMatches) {
    found = appendDeinflectedMatches(key, outMatches, found, maxMatches, 2, 2);
  }
  return appendPrefixMatches(key, outMatches, found, maxMatches, maxPrefixRecords);
}

size_t JapaneseDictionary::lookupDeinflected(const std::string& key, JapaneseDictionaryMatch* outMatches,
                                             const size_t maxMatches) {
  size_t found = appendDeinflectedMatches(key, outMatches, 0, maxMatches, 1, 1);
  if (found == 0) {
    found = appendDeinflectedMatches(key, outMatches, found, maxMatches, 2, 2);
  }
  return found;
}

size_t JapaneseDictionary::lookupDeinflectedThenPrefix(const std::string& key, JapaneseDictionaryMatch* outMatches,
                                                       const size_t maxMatches, const size_t maxPrefixRecords) {
  size_t found = appendDeinflectedMatches(key, outMatches, 0, maxMatches, 1, 1);
  if (found == 0) {
    found = appendDeinflectedMatches(key, outMatches, found, maxMatches, 2, 2);
  }
  return appendPrefixMatches(key, outMatches, found, maxMatches, maxPrefixRecords);
}

size_t JapaneseDictionary::lookupPrefix(const std::string& key, JapaneseDictionaryMatch* outMatches,
                                        const size_t maxMatches, const size_t maxPrefixRecords) {
  return appendPrefixMatches(key, outMatches, 0, maxMatches, maxPrefixRecords);
}

bool JapaneseDictionary::lookupContext(const std::string& context, std::vector<JapaneseDictionaryMatch>& outMatches,
                                       const size_t maxMatches, const size_t maxPrefixChars) {
  if (!openDefault()) return false;

  outMatches.clear();
  const auto prefixes = contextPrefixes(context, maxPrefixChars);
  std::vector<std::string> searchedTerms;
  searchedTerms.reserve(32);
  const size_t collectionLimit = std::min<size_t>(std::max<size_t>(maxMatches * 4, maxMatches + 8), 32);
  std::vector<JapaneseDictionaryMatch> staged(collectionLimit);
  for (auto it = prefixes.rbegin(); it != prefixes.rend() && outMatches.size() < collectionLimit; ++it) {
    size_t found = 0;
    if (!containsString(searchedTerms, *it)) {
      searchedTerms.push_back(*it);
      found = appendExactMatches(*it, *it, 0, staged.data(), found, collectionLimit);
      if (found > 0 && !jpdict::hasImmediateDeinflectionCandidate(*it)) {
        outMatches.insert(outMatches.end(), staged.begin(), staged.begin() + found);
        break;
      }
    }

    const auto candidates = jpdict::expandDeinflections(*it, 3, collectionLimit);
    for (const auto& candidate : candidates) {
      if (found >= collectionLimit) break;
      if (containsString(searchedTerms, candidate.term)) continue;
      searchedTerms.push_back(candidate.term);
      found = appendExactMatches(candidate.term, *it, candidate.depth, staged.data(), found, collectionLimit);
    }
    if (found > 0) {
      outMatches.insert(outMatches.end(), staged.begin(), staged.begin() + found);
      break;
    }
  }

  std::stable_sort(outMatches.begin(), outMatches.end(), [](const auto& a, const auto& b) {
    if (a.sourceText.size() != b.sourceText.size()) return a.sourceText.size() > b.sourceText.size();
    if (a.tier != b.tier) return a.tier < b.tier;
    if (a.score != b.score) return a.score > b.score;
    if (a.deinflectionDepth != b.deinflectionDepth) return a.deinflectionDepth < b.deinflectionDepth;
    return a.term < b.term;
  });
  if (outMatches.size() > maxMatches) outMatches.resize(maxMatches);

  return true;
}
