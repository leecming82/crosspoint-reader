#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

struct JapaneseDictionaryMatch {
  std::string key;
  std::string term;
  std::string terms;
  std::string reading;
  std::string definition;
  std::string sourceText;
  int32_t score = 0;
  int32_t sequence = 0;
  uint8_t tier = 0;
  uint8_t flags = 0;
  uint8_t deinflectionDepth = 0;
  uint8_t termCount = 0;
};

struct JapaneseDictionaryExactCursor {
  std::string key;
  uint32_t pos = 0;
  uint32_t end = 0;
  bool active = false;
  bool exhausted = true;
};

struct JapaneseDictionaryBundleStatus {
  bool storageReady = false;
  bool complete = false;
  std::vector<std::string> missingFiles;
};

class JapaneseDictionary {
 public:
  static constexpr const char* DEFAULT_JPDICT_PATH = "/.crosspoint/dicts/jpdict";
  static constexpr const char* DEFAULT_KANJI_PATH = "/.crosspoint/dicts/kanji";

  static JapaneseDictionaryBundleStatus validateDefaultBundle();
  static JapaneseDictionaryBundleStatus validateBundleAt(const char* jpdictPath, const char* kanjiPath);

  bool openDefault();
  bool openAt(const char* path);
  void close();
  bool isOpen() const;
  const std::string& getBasePath() const { return basePath; }

  bool hasExact(const std::string& key);
  bool beginExactLookup(const std::string& key, JapaneseDictionaryExactCursor& cursor);
  size_t lookupExactNext(JapaneseDictionaryExactCursor& cursor, JapaneseDictionaryMatch* outMatches, size_t maxMatches);
  size_t lookupExact(const std::string& key, JapaneseDictionaryMatch* outMatches, size_t maxMatches);
  size_t lookupExactThenPrefix(const std::string& key, JapaneseDictionaryMatch* outMatches, size_t maxMatches,
                               size_t maxPrefixRecords = 48);
  size_t lookupDeinflected(const std::string& key, JapaneseDictionaryMatch* outMatches, size_t maxMatches);
  size_t lookupDeinflectedThenPrefix(const std::string& key, JapaneseDictionaryMatch* outMatches, size_t maxMatches,
                                     size_t maxPrefixRecords = 48);
  size_t lookupPrefix(const std::string& key, JapaneseDictionaryMatch* outMatches, size_t maxMatches,
                      size_t maxPrefixRecords = 48);

  bool lookupContext(const std::string& context, std::vector<JapaneseDictionaryMatch>& outMatches,
                     size_t maxMatches = 5, size_t maxPrefixChars = 24);

 private:
  static constexpr uint32_t UNICODE_BUCKETS = 0x110000;
  static constexpr size_t BUCKET_BYTES = 8;
  static constexpr size_t KEY_BYTES = 96;
  static constexpr size_t RECORD_BYTES = 128;
  static constexpr size_t KEY_FILTER_BYTES = 128 * 1024;
  static constexpr uint8_t KEY_FILTER_HASHES = 2;

  struct Record {
    char key[KEY_BYTES + 1] = {};
    uint16_t keyLen = 0;
    uint8_t tier = 0;
    uint8_t flags = 0;
    uint32_t termOffset = 0;
    uint16_t termLen = 0;
    uint32_t readingOffset = 0;
    uint16_t readingLen = 0;
    uint32_t definitionOffset = 0;
    uint32_t definitionLen = 0;
    int32_t score = 0;
    int32_t sequence = 0;
  };

  std::string basePath;
  HalFile bucketsFile;
  HalFile recordsFile;
  HalFile stringsFile;
  std::vector<uint8_t> keyFilter;

  bool hasKeyFilter() const;
  bool loadKeyFilter(const char* path);
  bool keyMightExist(const std::string& key) const;
  bool readBucket(uint32_t codepoint, uint32_t& start, uint32_t& count);
  bool readRecord(uint32_t index, Record& record);
  bool lowerBoundInBucket(const std::string& key, uint32_t& bucketStart, uint32_t& bucketCount, uint32_t& lowerBound);
  void populateMatch(const Record& record, const std::string& sourceText, uint8_t deinflectionDepth,
                     JapaneseDictionaryMatch& match);
  size_t appendExactMatches(const std::string& key, const std::string& sourceText, uint8_t deinflectionDepth,
                            JapaneseDictionaryMatch* outMatches, size_t found, size_t maxMatches);
  size_t appendDeinflectedMatches(const std::string& key, JapaneseDictionaryMatch* outMatches, size_t found,
                                  size_t maxMatches, uint8_t minDepth, uint8_t maxDepth);
  size_t appendPrefixMatches(const std::string& key, JapaneseDictionaryMatch* outMatches, size_t found,
                             size_t maxMatches, size_t maxPrefixRecords);
  std::string readString(uint32_t offset, uint32_t length);
};

uint32_t firstUtf8Codepoint(const std::string& text);
