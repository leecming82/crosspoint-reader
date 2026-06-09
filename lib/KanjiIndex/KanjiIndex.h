#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class KanjiIndex {
 public:
  bool open(const char* basePath);
  bool openDefault();
  void close();
  bool isOpen() const;
  const std::string& path() const { return basePath; }

  size_t lookupReading(const std::string& reading, std::vector<std::string>& outCandidates, size_t maxCandidates);
  size_t lookupRadicalAliases(const std::string& alias, std::vector<std::string>& outComponents, size_t maxComponents);
  size_t lookupRadicalsByStroke(const std::string& strokes, std::vector<std::string>& outComponents,
                                size_t maxComponents);
  size_t lookupKanjiByStroke(const std::string& strokes, std::vector<std::string>& outCandidates,
                             size_t maxCandidates);
  size_t lookupComponentKanji(const std::string& component, std::vector<std::string>& outCandidates,
                              size_t maxCandidates);
  size_t lookupComponentKanjiByStroke(const std::string& component, const std::string& strokes,
                                      std::vector<std::string>& outCandidates, size_t maxCandidates);
  bool containsComponentKanji(const std::string& component, const std::string& kanji);

 private:
  static constexpr size_t KEY_BYTES = 64;
  static constexpr size_t RECORD_BYTES = 76;

  struct Record {
    char key[KEY_BYTES + 1] = {};
    uint16_t keyLen = 0;
    uint32_t candidatesOffset = 0;
    uint32_t candidatesLen = 0;
    uint16_t candidateCount = 0;
  };

  std::string basePath;
  HalFile recordsFile;
  HalFile stringsFile;
  uint32_t recordCount = 0;

  bool readRecord(uint32_t index, Record& record);
  bool findRecord(const char* typePrefix, const std::string& key, Record& record);
  std::string readString(uint32_t offset, uint32_t length);
  size_t lookupTyped(const char* typePrefix, const std::string& key, std::vector<std::string>& outCandidates,
                     size_t maxCandidates);
  bool containsTypedCandidate(const char* typePrefix, const std::string& key, const std::string& candidate);
};
