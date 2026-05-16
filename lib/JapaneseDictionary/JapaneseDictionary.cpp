#include "JapaneseDictionary.h"

#include <Utf8.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr size_t KEY_BYTES = 96;
constexpr size_t RECORD_BYTES = 124;
constexpr size_t RECORD_CACHE_RECORDS = 16;
constexpr size_t BUCKET_BYTES = 8;
constexpr uint32_t UNICODE_BUCKETS = 0x110000;

uint16_t readLe16(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8); }

uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

int32_t readLeI32(const uint8_t* p) { return static_cast<int32_t>(readLe32(p)); }

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

bool endsWith(const std::string& word, const char* suffix) {
  const size_t suffixLen = strlen(suffix);
  return word.size() >= suffixLen && word.compare(word.size() - suffixLen, suffixLen, suffix) == 0;
}

std::string replaceSuffix(const std::string& word, const char* suffix, const char* replacement) {
  return word.substr(0, word.size() - strlen(suffix)) + replacement;
}

std::string utf8LastChar(const std::string& text) {
  if (text.empty()) return "";
  size_t pos = text.size() - 1;
  while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) --pos;
  return text.substr(pos);
}

bool containsString(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

void addUnique(std::vector<std::string>& values, const std::string& value) {
  if (!value.empty() && !containsString(values, value)) values.push_back(value);
}

void addSuffixCandidate(std::vector<std::string>& out, const std::string& word, const char* suffix,
                        const char* replacement) {
  if (endsWith(word, suffix)) addUnique(out, replaceSuffix(word, suffix, replacement));
}

void addTrimmedAuxiliaryTailCandidates(std::vector<std::string>& out, const std::string& word) {
  static constexpr const char* suffixes[] = {
      "だけだった",   "だけでした",   "だけだ",   "だけです",   "だけ",
      "ばかりだった", "ばかりでした", "ばかりだ", "ばかりです", "ばかり",
  };
  for (const char* suffix : suffixes) {
    if (endsWith(word, suffix)) addUnique(out, word.substr(0, word.size() - strlen(suffix)));
  }
}

bool isGodanARow(const std::string& ch) {
  static constexpr const char* values[] = {"わ", "か", "が", "さ", "ざ", "た", "だ",
                                           "な", "は", "ば", "ぱ", "ま", "ら"};
  for (const char* value : values) {
    if (ch == value) return true;
  }
  return false;
}

const char* godanFromARow(const std::string& ch) {
  struct Pair {
    const char* from;
    const char* to;
  };
  static constexpr Pair pairs[] = {{"わ", "う"}, {"か", "く"}, {"が", "ぐ"}, {"さ", "す"}, {"ざ", "ず"},
                                   {"た", "つ"}, {"だ", "づ"}, {"な", "ぬ"}, {"は", "ふ"}, {"ば", "ぶ"},
                                   {"ぱ", "ぷ"}, {"ま", "む"}, {"ら", "る"}};
  for (const auto& pair : pairs) {
    if (ch == pair.from) return pair.to;
  }
  return nullptr;
}

const char* godanFromMasuStem(const std::string& ch) {
  struct Pair {
    const char* from;
    const char* to;
  };
  static constexpr Pair pairs[] = {{"い", "う"}, {"き", "く"}, {"ぎ", "ぐ"}, {"し", "す"}, {"じ", "ず"},
                                   {"ち", "つ"}, {"ぢ", "づ"}, {"に", "ぬ"}, {"ひ", "ふ"}, {"び", "ぶ"},
                                   {"ぴ", "ぷ"}, {"み", "む"}, {"り", "る"}};
  for (const auto& pair : pairs) {
    if (ch == pair.from) return pair.to;
  }
  return nullptr;
}

void addTeTaCandidates(std::vector<std::string>& out, const std::string& word) {
  static constexpr const char* uTsuru[] = {"う", "つ", "る"};
  for (const char* repl : uTsuru) {
    addSuffixCandidate(out, word, "って", repl);
    addSuffixCandidate(out, word, "った", repl);
  }
  static constexpr const char* bumnu[] = {"ぶ", "む", "ぬ"};
  for (const char* repl : bumnu) {
    addSuffixCandidate(out, word, "んで", repl);
    addSuffixCandidate(out, word, "んだ", repl);
  }
  addSuffixCandidate(out, word, "いて", "く");
  addSuffixCandidate(out, word, "いた", "く");
  addSuffixCandidate(out, word, "いで", "ぐ");
  addSuffixCandidate(out, word, "いだ", "ぐ");
  addSuffixCandidate(out, word, "して", "す");
  addSuffixCandidate(out, word, "した", "す");
  addSuffixCandidate(out, word, "して", "する");
  addSuffixCandidate(out, word, "した", "する");
  addSuffixCandidate(out, word, "行って", "行く");
  addSuffixCandidate(out, word, "行った", "行く");
  addSuffixCandidate(out, word, "来て", "来る");
  addSuffixCandidate(out, word, "来た", "来る");
  addSuffixCandidate(out, word, "來て", "來る");
  addSuffixCandidate(out, word, "來た", "來る");

  if (endsWith(word, "て") || endsWith(word, "た")) {
    const std::string stem = word.substr(0, word.size() - strlen(endsWith(word, "て") ? "て" : "た"));
    const std::string last = utf8LastChar(stem);
    if (last != "っ" && last != "ん" && last != "い") addUnique(out, stem + "る");
  }
}

void addPoliteStemCandidates(std::vector<std::string>& out, const std::string& stem) {
  if (stem.empty()) return;
  addUnique(out, stem);
  const std::string last = utf8LastChar(stem);
  if (const char* repl = godanFromMasuStem(last)) {
    addUnique(out, stem.substr(0, stem.size() - last.size()) + repl);
  }
  addSuffixCandidate(out, stem, "し", "する");
  addSuffixCandidate(out, stem, "こ", "くる");
  addSuffixCandidate(out, stem, "来", "来る");
  addSuffixCandidate(out, stem, "來", "來る");
  if (!isGodanARow(last)) addUnique(out, stem + "る");
}

void addBareMasuStemCandidates(std::vector<std::string>& out, const std::string& word) {
  const std::string last = utf8LastChar(word);
  if (const char* repl = godanFromMasuStem(last)) {
    addUnique(out, word.substr(0, word.size() - last.size()) + repl);
  }
  addSuffixCandidate(out, word, "し", "する");
  addSuffixCandidate(out, word, "こ", "くる");
}

void addNegativeCandidates(std::vector<std::string>& out, const std::string& word) {
  if (endsWith(word, "なかった")) addUnique(out, replaceSuffix(word, "なかった", "ない"));
  if (endsWith(word, "ませんでした")) addUnique(out, replaceSuffix(word, "ませんでした", "ません"));
  if (!endsWith(word, "ない")) return;
  const std::string stem = word.substr(0, word.size() - strlen("ない"));
  if (stem.empty()) return;
  const std::string last = utf8LastChar(stem);
  if (const char* repl = godanFromARow(last)) {
    addUnique(out, stem.substr(0, stem.size() - last.size()) + repl);
  }
  addSuffixCandidate(out, stem, "し", "する");
  addSuffixCandidate(out, stem, "こ", "くる");
  addSuffixCandidate(out, stem, "来", "来る");
  addSuffixCandidate(out, stem, "來", "來る");
  if (!isGodanARow(last)) addUnique(out, stem + "る");
}

void addIAdjectiveCandidates(std::vector<std::string>& out, const std::string& word) {
  addSuffixCandidate(out, word, "く", "い");
  addSuffixCandidate(out, word, "くて", "い");
  addSuffixCandidate(out, word, "かった", "い");
  addSuffixCandidate(out, word, "かったです", "い");
  addSuffixCandidate(out, word, "くない", "い");
  addSuffixCandidate(out, word, "くないです", "い");
  addSuffixCandidate(out, word, "くなかった", "い");
  addSuffixCandidate(out, word, "くなかったです", "い");
  addSuffixCandidate(out, word, "くありません", "い");
  addSuffixCandidate(out, word, "くありませんでした", "い");
}

void addInflectionStep(std::vector<std::string>& out, const std::string& word) {
  addTrimmedAuxiliaryTailCandidates(out, word);
  addTeTaCandidates(out, word);
  addBareMasuStemCandidates(out, word);
  addNegativeCandidates(out, word);
  addIAdjectiveCandidates(out, word);

  static constexpr const char* politeSuffixes[] = {"ませんでした", "ました", "ません", "ましょう", "ます"};
  for (const char* suffix : politeSuffixes) {
    if (endsWith(word, suffix)) addPoliteStemCandidates(out, word.substr(0, word.size() - strlen(suffix)));
  }

  addSuffixCandidate(out, word, "ちゃう", "て");
  addSuffixCandidate(out, word, "じゃう", "で");
  addSuffixCandidate(out, word, "ちゃった", "て");
  addSuffixCandidate(out, word, "じゃった", "で");
  addSuffixCandidate(out, word, "ちゃって", "て");
  addSuffixCandidate(out, word, "じゃって", "で");
  addSuffixCandidate(out, word, "ている", "て");
  addSuffixCandidate(out, word, "でいる", "で");
  addSuffixCandidate(out, word, "ていた", "て");
  addSuffixCandidate(out, word, "でいた", "で");

  struct Rule {
    const char* suffix;
    const char* replacement;
  };
  static constexpr Rule rules[] = {
      {"えば", "う"},       {"けば", "く"},     {"げば", "ぐ"},     {"せば", "す"},       {"てば", "つ"},
      {"ねば", "ぬ"},       {"べば", "ぶ"},     {"めば", "む"},     {"れば", "る"},       {"ければ", "い"},
      {"おう", "う"},       {"こう", "く"},     {"ごう", "ぐ"},     {"そう", "す"},       {"とう", "つ"},
      {"のう", "ぬ"},       {"ぼう", "ぶ"},     {"もう", "む"},     {"ろう", "る"},       {"よう", "る"},
      {"われ", "う"},       {"かれ", "く"},     {"がれ", "ぐ"},     {"され", "す"},       {"たれ", "つ"},
      {"なれ", "ぬ"},       {"ばれ", "ぶ"},     {"まれ", "む"},     {"られ", "る"},       {"わせ", "う"},
      {"かせ", "く"},       {"がせ", "ぐ"},     {"させ", "す"},     {"たせ", "つ"},       {"なせ", "ぬ"},
      {"ばせ", "ぶ"},       {"ませ", "む"},     {"らせ", "る"},     {"われる", "う"},     {"かれる", "く"},
      {"がれる", "ぐ"},     {"される", "す"},   {"たれる", "つ"},   {"なれる", "ぬ"},     {"ばれる", "ぶ"},
      {"まれる", "む"},     {"られる", "る"},   {"わせる", "う"},   {"かせる", "く"},     {"がせる", "ぐ"},
      {"させる", "す"},     {"たせる", "つ"},   {"なせる", "ぬ"},   {"ばせる", "ぶ"},     {"ませる", "む"},
      {"らせる", "る"},     {"ける", "く"},     {"げる", "ぐ"},     {"せる", "す"},       {"てる", "つ"},
      {"ねる", "ぬ"},       {"べる", "ぶ"},     {"める", "む"},     {"れる", "る"},       {"しよう", "する"},
      {"こよう", "くる"},   {"来よう", "来る"}, {"される", "する"}, {"され", "する"},     {"こられる", "くる"},
      {"来られる", "来る"}, {"こられ", "くる"}, {"来られ", "来る"}, {"こさせる", "くる"}, {"来させる", "来る"},
      {"こさせ", "くる"},   {"来させ", "来る"}};
  for (const auto& rule : rules) addSuffixCandidate(out, word, rule.suffix, rule.replacement);
}

struct DeinflectionCandidate {
  std::string term;
  uint8_t depth = 0;
};

bool containsCandidateTerm(const std::vector<DeinflectionCandidate>& values, const std::string& term) {
  return std::find_if(values.begin(), values.end(), [&term](const auto& value) { return value.term == term; }) !=
         values.end();
}

std::vector<DeinflectionCandidate> expandDeinflections(const std::string& input) {
  std::vector<DeinflectionCandidate> ordered;
  if (!input.empty()) ordered.push_back({input, 0});
  size_t levelStart = 0;
  size_t levelEnd = ordered.size();
  static constexpr uint8_t MAX_DEPTH = 3;
  for (uint8_t depth = 0; depth < MAX_DEPTH && levelStart < levelEnd; ++depth) {
    const size_t before = ordered.size();
    for (size_t i = levelStart; i < levelEnd; ++i) {
      std::vector<std::string> next;
      addInflectionStep(next, ordered[i].term);
      for (const auto& term : next) {
        if (!containsCandidateTerm(ordered, term)) ordered.push_back({term, static_cast<uint8_t>(depth + 1)});
      }
    }
    levelStart = levelEnd;
    levelEnd = ordered.size();
    if (before == levelEnd) break;
  }
  return ordered;
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
  if (!basePath.empty() && bucketsFile.isOpen() && recordsFile.isOpen() && stringsFile.isOpen()) return true;

  static constexpr const char* paths[] = {
      "/.crosspoint/dicts/jitendex-cpdict-ranked-japanese",
      "/.crosspoint/dicts/jitendex-cpdict-ranked",
      "/.crosspoint/dicts/jitendex-cpdict-modern",
      "/dict/jitendex-cpdict-ranked-japanese",
      "/dict/jitendex-cpdict-ranked",
      "/dict/jitendex-cpdict-modern",
      "/jitendex-cpdict-ranked-japanese",
      "/jitendex-cpdict-ranked",
      "/jitendex-cpdict-modern",
  };

  for (const char* path : paths) {
    if (openAt(path)) return true;
  }
  return false;
}

void JapaneseDictionary::close() {
  if (bucketsFile.isOpen()) bucketsFile.close();
  if (recordsFile.isOpen()) recordsFile.close();
  if (stringsFile.isOpen()) stringsFile.close();
  bucketsFile = FsFile();
  recordsFile = FsFile();
  stringsFile = FsFile();
  basePath.clear();
  resetRecordCache(true);
  cachedBucketValid = false;
  cachedBucketCp = UINT32_MAX;
  cachedBucketStart = 0;
  cachedBucketCount = 0;
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
  resetRecordCache(false);
  for (auto& slot : recordCacheSlots) slot.data.resize(RECORD_BYTES * RECORD_CACHE_RECORDS);
  cachedBucketValid = false;
  return true;
}

void JapaneseDictionary::resetRecordCache(const bool releaseBuffers) {
  recordCacheUseCounter = 0;
  for (auto& slot : recordCacheSlots) {
    if (releaseBuffers) {
      slot.data = std::vector<uint8_t>();
    }
    slot.blockStart = UINT32_MAX;
    slot.count = 0;
    slot.lastUsed = 0;
    slot.valid = false;
  }
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

bool JapaneseDictionary::readBucketCached(const uint32_t cp, uint32_t& start, uint32_t& count) {
  if (cachedBucketValid && cp == cachedBucketCp) {
    start = cachedBucketStart;
    count = cachedBucketCount;
    return true;
  }
  if (!readBucket(cp, start, count)) return false;
  cachedBucketCp = cp;
  cachedBucketStart = start;
  cachedBucketCount = count;
  cachedBucketValid = true;
  return true;
}

bool JapaneseDictionary::readRecord(const uint32_t index, Record& record) {
  const uint32_t blockStart = index - (index % RECORD_CACHE_RECORDS);
  RecordCacheSlot* slot = nullptr;

  for (auto& candidate : recordCacheSlots) {
    if (candidate.valid && index >= candidate.blockStart && index < candidate.blockStart + candidate.count) {
      slot = &candidate;
      break;
    }
  }

  const bool cacheHit = slot != nullptr;
  if (!cacheHit) {
    slot = &recordCacheSlots[0];
    for (auto& candidate : recordCacheSlots) {
      if (!candidate.valid) {
        slot = &candidate;
        break;
      }
      if (candidate.lastUsed < slot->lastUsed) slot = &candidate;
    }

    if (slot->data.empty()) slot->data.resize(RECORD_BYTES * RECORD_CACHE_RECORDS);
    if (!recordsFile.seek(blockStart * RECORD_BYTES)) return false;
    const int bytesRead = recordsFile.read(slot->data.data(), slot->data.size());
    if (bytesRead < static_cast<int>(RECORD_BYTES)) return false;
    slot->blockStart = blockStart;
    slot->count = static_cast<uint16_t>(bytesRead / RECORD_BYTES);
    slot->valid = slot->count > 0;
  }

  if (!slot || !slot->valid || index < slot->blockStart || index >= slot->blockStart + slot->count) {
    return false;
  }

  slot->lastUsed = ++recordCacheUseCounter;
  const uint8_t* buf = slot->data.data() + ((index - slot->blockStart) * RECORD_BYTES);
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

bool JapaneseDictionary::findExact(const std::string& term, const std::string& sourceText,
                                   const uint8_t deinflectionDepth, std::vector<JapaneseDictionaryMatch>& outMatches,
                                   const size_t maxMatches) {
  if (term.empty() || outMatches.size() >= maxMatches) return true;

  const auto* p = reinterpret_cast<const unsigned char*>(term.c_str());
  const uint32_t firstCp = utf8NextCodepoint(&p);
  uint32_t start = 0;
  uint32_t count = 0;
  if (!readBucketCached(firstCp, start, count) || count == 0) return true;

  uint32_t lo = start;
  uint32_t hi = start + count;
  Record rec;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (!readRecord(mid, rec)) return false;
    if (rec.key < term) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }

  for (uint32_t pos = lo; pos < start + count && outMatches.size() < maxMatches; ++pos) {
    if (!readRecord(pos, rec)) return false;
    if (rec.key != term) break;
    JapaneseDictionaryMatch match;
    match.term = rec.key;
    match.sourceText = sourceText;
    match.reading = readString(rec.readingOffset, rec.readingLength);
    match.definition =
        readString(rec.definitionOffset, static_cast<uint16_t>(std::min<uint32_t>(rec.definitionLength, 512)));
    match.score = rec.score;
    match.sequence = rec.sequence;
    match.tier = rec.tier;
    match.deinflectionDepth = deinflectionDepth;
    outMatches.push_back(std::move(match));
  }

  return true;
}

bool JapaneseDictionary::lookupContext(const std::string& context, std::vector<JapaneseDictionaryMatch>& outMatches,
                                       const size_t maxMatches, const size_t maxPrefixChars) {
  if (!openDefault()) return false;

  outMatches.clear();
  const auto prefixes = contextPrefixes(context, maxPrefixChars);
  std::vector<std::string> searchedTerms;
  searchedTerms.reserve(32);
  bool lookupSucceeded = true;

  const size_t collectionLimit = std::max<size_t>(maxMatches * 4, maxMatches + 8);
  for (auto it = prefixes.rbegin(); it != prefixes.rend() && outMatches.size() < collectionLimit; ++it) {
    const size_t matchesBeforePrefix = outMatches.size();
    if (!containsString(searchedTerms, *it)) {
      searchedTerms.push_back(*it);
      if (!findExact(*it, *it, 0, outMatches, collectionLimit)) {
        lookupSucceeded = false;
        break;
      }
      if (outMatches.size() > matchesBeforePrefix) break;
    }

    const auto candidates = expandDeinflections(*it);
    for (const auto& candidate : candidates) {
      if (outMatches.size() >= collectionLimit) break;
      if (containsString(searchedTerms, candidate.term)) continue;
      searchedTerms.push_back(candidate.term);
      if (!findExact(candidate.term, *it, candidate.depth, outMatches, collectionLimit)) {
        lookupSucceeded = false;
        break;
      }
    }
    if (!lookupSucceeded) break;
    if (outMatches.size() > matchesBeforePrefix) break;
  }

  std::stable_sort(outMatches.begin(), outMatches.end(), [](const auto& a, const auto& b) {
    if (a.sourceText.size() != b.sourceText.size()) return a.sourceText.size() > b.sourceText.size();
    if (a.deinflectionDepth != b.deinflectionDepth) return a.deinflectionDepth < b.deinflectionDepth;
    if (a.tier != b.tier) return a.tier < b.tier;
    if (a.score != b.score) return a.score > b.score;
    return a.term < b.term;
  });
  if (outMatches.size() > maxMatches) outMatches.resize(maxMatches);

  return lookupSucceeded;
}
