#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

struct JapaneseDictionaryMatch {
  std::string term;
  std::string reading;
  std::string definition;
  int32_t score = 0;
  int32_t sequence = 0;
  uint8_t tier = 0;
};

struct JapaneseDictionaryTelemetry {
  uint32_t openUs = 0;
  uint32_t bucketUs = 0;
  uint32_t searchUs = 0;
  uint32_t stringUs = 0;
  uint32_t totalUs = 0;
  uint16_t matches = 0;
  bool opened = false;
};

class JapaneseDictionary {
 public:
  bool openDefault();
  bool lookupContext(const std::string& context, std::vector<JapaneseDictionaryMatch>& outMatches,
                     JapaneseDictionaryTelemetry* telemetry = nullptr, size_t maxMatches = 5,
                     size_t maxPrefixChars = 24);
  const std::string& getBasePath() const { return basePath; }

  static void appendTelemetryCsv(const std::string& query, const std::string& dictPath,
                                 const JapaneseDictionaryTelemetry& telemetry);

 private:
  struct Record;

  std::string basePath;
  FsFile bucketsFile;
  FsFile recordsFile;
  FsFile stringsFile;

  bool openAt(const char* path);
  bool readBucket(uint32_t cp, uint32_t& start, uint32_t& count);
  bool readRecord(uint32_t index, Record& record);
  std::string readString(uint32_t offset, uint32_t length);
  bool findExact(const std::string& term, std::vector<JapaneseDictionaryMatch>& outMatches, size_t maxMatches,
                 uint32_t* bucketUs, uint32_t* searchUs, uint32_t* stringUs);
};
