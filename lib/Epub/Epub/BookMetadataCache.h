#pragma once

#include <HalStorage.h>

#include <algorithm>
#include <deque>
#include <string>

class BookMetadataCache {
 public:
  struct BookMetadata {
    std::string title;
    std::string author;
    std::string language;
    std::string coverItemHref;
    std::string textReferenceHref;
    bool pageProgressionRtl = false;
  };

  struct SpineEntry {
    std::string href;
    uint32_t cumulativeSize;
    int16_t tocIndex;
    uint16_t physicalSpineIndex;
    uint32_t sourceStartOffset;
    uint32_t sourceEndOffset;
    uint16_t splitIndex;
    uint16_t splitCount;

    SpineEntry()
        : cumulativeSize(0),
          tocIndex(-1),
          physicalSpineIndex(0),
          sourceStartOffset(0),
          sourceEndOffset(0),
          splitIndex(0),
          splitCount(1) {}
    SpineEntry(std::string href, const uint32_t cumulativeSize, const int16_t tocIndex)
        : href(std::move(href)),
          cumulativeSize(cumulativeSize),
          tocIndex(tocIndex),
          physicalSpineIndex(0),
          sourceStartOffset(0),
          sourceEndOffset(0),
          splitIndex(0),
          splitCount(1) {}
  };

  struct TocEntry {
    std::string title;
    std::string href;
    std::string anchor;
    uint8_t level;
    int16_t spineIndex;

    TocEntry() : level(0), spineIndex(-1) {}
    TocEntry(std::string title, std::string href, std::string anchor, const uint8_t level, const int16_t spineIndex)
        : title(std::move(title)),
          href(std::move(href)),
          anchor(std::move(anchor)),
          level(level),
          spineIndex(spineIndex) {}
  };

  struct AnchorEntry {
    std::string href;
    std::string anchor;
    int16_t spineIndex;

    AnchorEntry() : spineIndex(-1) {}
    AnchorEntry(std::string href, std::string anchor, const int16_t spineIndex)
        : href(std::move(href)), anchor(std::move(anchor)), spineIndex(spineIndex) {}
  };

 private:
  std::string cachePath;
  uint32_t lutOffset;
  uint16_t spineCount;
  uint16_t tocCount;
  uint16_t anchorCount;
  bool loaded;
  bool buildMode;

  HalFile bookFile;
  // Temp file handles during build
  HalFile spineFile;
  HalFile tocFile;

  // Index for fast href→spineIndex lookup (used only for large EPUBs)
  struct SpineHrefIndexEntry {
    uint64_t hrefHash;  // FNV-1a 64-bit hash
    uint16_t hrefLen;   // length for collision reduction
    int16_t spineIndex;
  };
  std::deque<SpineHrefIndexEntry> spineHrefIndex;
  bool useSpineHrefIndex = false;

  static constexpr uint16_t LARGE_SPINE_THRESHOLD = 400;

  // FNV-1a 64-bit hash function
  static uint64_t fnvHash64(const std::string& s) {
    uint64_t hash = 14695981039346656037ull;
    for (char c : s) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 1099511628211ull;
    }
    return hash;
  }

  uint32_t writeSpineEntry(HalFile& file, const SpineEntry& entry) const;
  uint32_t writeTocEntry(HalFile& file, const TocEntry& entry) const;
  uint32_t writeAnchorEntry(HalFile& file, const AnchorEntry& entry) const;
  SpineEntry readSpineEntry(HalFile& file) const;
  TocEntry readTocEntry(HalFile& file) const;
  AnchorEntry readAnchorEntry(HalFile& file) const;

 public:
  BookMetadata coreMetadata;

  explicit BookMetadataCache(std::string cachePath)
      : cachePath(std::move(cachePath)),
        lutOffset(0),
        spineCount(0),
        tocCount(0),
        anchorCount(0),
        loaded(false),
        buildMode(false) {}
  ~BookMetadataCache() = default;

  // Building phase (stream to disk immediately)
  bool beginWrite();
  bool beginContentOpfPass();
  void createSpineEntry(const std::string& href);
  bool endContentOpfPass();
  bool beginTocPass();
  void createTocEntry(const std::string& title, const std::string& href, const std::string& anchor, uint8_t level);
  bool endTocPass();
  bool endWrite();
  bool cleanupTmpFiles() const;

  // Post-processing to update mappings and sizes
  bool buildBookBin(const std::string& epubPath, const BookMetadata& metadata);

  // Reading phase (read mode)
  bool load();
  SpineEntry getSpineEntry(int index);
  TocEntry getTocEntry(int index);
  AnchorEntry getAnchorEntry(int index);
  int resolveAnchorToSpineIndex(const std::string& href, const std::string& anchor);
  int getSpineCount() const { return spineCount; }
  int getTocCount() const { return tocCount; }
  bool isLoaded() const { return loaded; }
};
