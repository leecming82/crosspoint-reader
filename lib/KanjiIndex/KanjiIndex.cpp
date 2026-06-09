#include "KanjiIndex.h"

#include <JapaneseDictionary.h>
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

uint16_t readLe16(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8); }

uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string utf8CharAt(const std::string& text, const size_t pos) {
  if (pos >= text.size()) return {};
  size_t end = pos + 1;
  while (end < text.size() && (static_cast<uint8_t>(text[end]) & 0xC0) == 0x80) {
    ++end;
  }
  return text.substr(pos, end - pos);
}

bool packedContainsUtf8Candidate(const std::string& packed, const std::string& candidate) {
  for (size_t pos = 0; pos < packed.size();) {
    const std::string ch = utf8CharAt(packed, pos);
    if (ch.empty()) break;
    if (ch == candidate) return true;
    pos += ch.size();
  }
  return false;
}
}  // namespace

bool KanjiIndex::openDefault() { return open(JapaneseDictionary::DEFAULT_KANJI_PATH); }

bool KanjiIndex::open(const char* basePathIn) {
  close();
  if (!Storage.exists(joinPath(basePathIn, "manifest.json").c_str())) return false;

  HalFile records;
  HalFile strings;
  if (!Storage.openFileForRead("KANJI", joinPath(basePathIn, "lookup.records.bin"), records)) return false;
  if (!Storage.openFileForRead("KANJI", joinPath(basePathIn, "lookup.strings.bin"), strings)) return false;

  basePath = basePathIn;
  recordsFile = std::move(records);
  stringsFile = std::move(strings);
  recordCount = recordsFile.fileSize() / RECORD_BYTES;
  return true;
}

void KanjiIndex::close() {
  if (recordsFile.isOpen()) recordsFile.close();
  if (stringsFile.isOpen()) stringsFile.close();
  recordsFile = HalFile();
  stringsFile = HalFile();
  recordCount = 0;
  basePath.clear();
}

bool KanjiIndex::isOpen() const { return recordsFile.isOpen() && stringsFile.isOpen(); }

bool KanjiIndex::readRecord(const uint32_t index, Record& record) {
  uint8_t data[RECORD_BYTES] = {};
  if (!recordsFile.isOpen() || !recordsFile.seek(index * RECORD_BYTES) ||
      recordsFile.read(data, sizeof(data)) != static_cast<int>(sizeof(data))) {
    return false;
  }

  record.keyLen = readLe16(data + 64);
  if (record.keyLen > KEY_BYTES) return false;
  memcpy(record.key, data, record.keyLen);
  record.key[record.keyLen] = '\0';
  record.candidatesOffset = readLe32(data + 66);
  record.candidatesLen = readLe32(data + 70);
  record.candidateCount = readLe16(data + 74);
  return true;
}

std::string KanjiIndex::readString(const uint32_t offset, const uint32_t length) {
  if (!stringsFile.isOpen() || length == 0 || !stringsFile.seek(offset)) return {};

  std::string out;
  out.reserve(length);
  constexpr size_t CHUNK_BYTES = 96;
  uint8_t buffer[CHUNK_BYTES];
  uint32_t remaining = length;
  while (remaining > 0) {
    const size_t wanted = std::min<size_t>(remaining, CHUNK_BYTES);
    const int read = stringsFile.read(buffer, wanted);
    if (read <= 0) break;
    out.append(reinterpret_cast<const char*>(buffer), static_cast<size_t>(read));
    remaining -= static_cast<uint32_t>(read);
  }
  return out;
}

bool KanjiIndex::findRecord(const char* typePrefix, const std::string& key, Record& record) {
  if (!isOpen() || typePrefix == nullptr || key.empty() || recordCount == 0) return false;

  const std::string typedKey = std::string(typePrefix) + key;
  uint32_t lo = 0;
  uint32_t hi = recordCount;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (!readRecord(mid, record)) return false;
    const int cmp = strcmp(record.key, typedKey.c_str());
    if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }

  return lo < recordCount && readRecord(lo, record) && strcmp(record.key, typedKey.c_str()) == 0;
}

size_t KanjiIndex::lookupTyped(const char* typePrefix, const std::string& key, std::vector<std::string>& outCandidates,
                               const size_t maxCandidates) {
  outCandidates.clear();
  if (maxCandidates == 0) return 0;

  Record record;
  if (!findRecord(typePrefix, key, record)) return 0;

  const std::string packed = readString(record.candidatesOffset, record.candidatesLen);
  outCandidates.reserve(std::min<size_t>(record.candidateCount, maxCandidates));
  for (size_t pos = 0; pos < packed.size() && outCandidates.size() < maxCandidates;) {
    const std::string ch = utf8CharAt(packed, pos);
    if (ch.empty()) break;
    outCandidates.push_back(ch);
    pos += ch.size();
  }
  return outCandidates.size();
}

bool KanjiIndex::containsTypedCandidate(const char* typePrefix, const std::string& key, const std::string& candidate) {
  if (candidate.empty()) return false;

  Record record;
  if (!findRecord(typePrefix, key, record)) return false;

  const std::string packed = readString(record.candidatesOffset, record.candidatesLen);
  return packedContainsUtf8Candidate(packed, candidate);
}

size_t KanjiIndex::lookupReading(const std::string& reading, std::vector<std::string>& outCandidates,
                                 const size_t maxCandidates) {
  return lookupTyped("r:", reading, outCandidates, maxCandidates);
}

size_t KanjiIndex::lookupRadicalAliases(const std::string& alias, std::vector<std::string>& outComponents,
                                        const size_t maxComponents) {
  return lookupTyped("a:", alias, outComponents, maxComponents);
}

size_t KanjiIndex::lookupRadicalsByStroke(const std::string& strokes, std::vector<std::string>& outComponents,
                                          const size_t maxComponents) {
  return lookupTyped("s:", strokes, outComponents, maxComponents);
}

size_t KanjiIndex::lookupKanjiByStroke(const std::string& strokes, std::vector<std::string>& outCandidates,
                                       const size_t maxCandidates) {
  return lookupTyped("k:", strokes, outCandidates, maxCandidates);
}

size_t KanjiIndex::lookupComponentKanji(const std::string& component, std::vector<std::string>& outCandidates,
                                        const size_t maxCandidates) {
  return lookupTyped("c:", component, outCandidates, maxCandidates);
}

bool KanjiIndex::containsComponentKanji(const std::string& component, const std::string& kanji) {
  return containsTypedCandidate("c:", component, kanji);
}

size_t KanjiIndex::lookupComponentKanjiByStroke(const std::string& component, const std::string& strokes,
                                                std::vector<std::string>& outCandidates, const size_t maxCandidates) {
  outCandidates.clear();
  if (!isOpen() || component.empty() || strokes.empty() || maxCandidates == 0) return 0;

  Record componentRecord;
  Record strokeRecord;
  if (!findRecord("c:", component, componentRecord) || !findRecord("k:", strokes, strokeRecord)) return 0;

  const std::string packed = readString(componentRecord.candidatesOffset, componentRecord.candidatesLen);
  const std::string strokePacked = readString(strokeRecord.candidatesOffset, strokeRecord.candidatesLen);
  for (size_t pos = 0; pos < packed.size() && outCandidates.size() < maxCandidates;) {
    const std::string ch = utf8CharAt(packed, pos);
    if (ch.empty()) break;
    if (packedContainsUtf8Candidate(strokePacked, ch)) {
      outCandidates.push_back(ch);
    }
    pos += ch.size();
  }
  return outCandidates.size();
}
