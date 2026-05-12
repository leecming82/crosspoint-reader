#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

struct JapaneseDictionaryMatch {
  std::string term;
  std::string reading;
  std::string definition;
  std::string sourceText;
  int32_t score = 0;
  int32_t sequence = 0;
  uint8_t tier = 0;
  uint8_t deinflectionDepth = 0;
};

class JapaneseDictionary {
 public:
  bool openDefault();
  void close();
  bool lookupContext(const std::string& context, std::vector<JapaneseDictionaryMatch>& outMatches,
                     size_t maxMatches = 5, size_t maxPrefixChars = 24);
  const std::string& getBasePath() const { return basePath; }

 private:
  struct Record;

  std::string basePath;
  FsFile bucketsFile;
  FsFile recordsFile;
  FsFile stringsFile;
  uint32_t cachedBucketCp = UINT32_MAX;
  uint32_t cachedBucketStart = 0;
  uint32_t cachedBucketCount = 0;
  bool cachedBucketValid = false;

  bool openAt(const char* path);
  bool readBucket(uint32_t cp, uint32_t& start, uint32_t& count);
  bool readBucketCached(uint32_t cp, uint32_t& start, uint32_t& count);
  bool readRecord(uint32_t index, Record& record);
  std::string readString(uint32_t offset, uint32_t length);
  bool findExact(const std::string& term, const std::string& sourceText, uint8_t deinflectionDepth,
                 std::vector<JapaneseDictionaryMatch>& outMatches, size_t maxMatches);
};
