#include "BookMetadataCache.h"

#include <Logging.h>
#include <Serialization.h>
#include <ZipFile.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include "FsHelpers.h"

namespace {
constexpr uint8_t BOOK_CACHE_VERSION = 11;
constexpr char bookBinFile[] = "/book.bin";
constexpr char tmpSpineBinFile[] = "/spine.bin.tmp";
constexpr char tmpTocBinFile[] = "/toc.bin.tmp";

constexpr uint32_t SPLIT_SOURCE_SIZE_THRESHOLD = 140 * 1024;
constexpr uint32_t SPLIT_TEXT_UNITS_THRESHOLD = 30000;
constexpr uint32_t SPLIT_TARGET_TEXT_UNITS = 22000;
constexpr uint32_t SPLIT_MIN_TEXT_UNITS = 10000;
constexpr uint32_t SPLIT_HARD_MAX_TEXT_UNITS = 35000;

struct SourceRange {
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t units = 0;
};

struct AnchorOffset {
  std::string id;
  uint32_t offset = 0;
};

struct HrefTarget {
  std::string href;
  std::string anchor;
};

std::string lowercase(std::string value) {
  for (auto& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

std::string normaliseRelativeHref(const std::string& baseHref, const std::string& href) {
  if (href.empty()) {
    return baseHref;
  }
  const auto slashPos = baseHref.find_last_of('/');
  const std::string baseDir = slashPos == std::string::npos ? std::string() : baseHref.substr(0, slashPos + 1);
  return FsHelpers::normalisePath(FsHelpers::decodePercentEscapes(baseDir + href));
}

template <typename Container>
bool anchorListContains(const Container& anchors, const std::string& anchor) {
  return std::find(anchors.begin(), anchors.end(), anchor) != anchors.end();
}

class SpineSplitScanner final : public Print {
 public:
  explicit SpineSplitScanner(const std::string& currentHref, std::vector<std::string>& targetAnchors)
      : currentHref(currentHref), targetAnchors(targetAnchors) {}

  size_t write(uint8_t b) override {
    const char c = static_cast<char>(b);
    absoluteOffset++;

    if (inTag) {
      appendTagChar(c);
      if (c == '>') {
        finishTag();
      }
      return 1;
    }

    if (c == '<') {
      inTag = true;
      tagLength = 0;
      appendTagChar(c);
      return 1;
    }

    if (bodyStarted && skipDepth == 0) {
      countVisibleByte(static_cast<uint8_t>(b));
    }

    return 1;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    for (size_t i = 0; i < size; i++) {
      write(buffer[i]);
    }
    return size;
  }

  std::deque<SourceRange> finish() {
    if (invalidRootWrapper) {
      return {};
    }
    if (!bodyStarted) {
      bodyContentStart = 0;
    }
    if (ranges.empty() && currentUnits < SPLIT_TEXT_UNITS_THRESHOLD) {
      return {};
    }

    const uint32_t finalEnd = bodyEndOffset > bodyContentStart ? bodyEndOffset : absoluteOffset;
    if (finalEnd > rangeStart) {
      ranges.push_back({rangeStart, finalEnd, currentUnits});
    }
    return ranges.size() > 1 ? ranges : std::deque<SourceRange>{};
  }

  const std::vector<AnchorOffset>& getAnchors() const { return anchors; }
  const std::vector<HrefTarget>& getHrefTargets() const { return hrefTargets; }

 private:
  bool inTag = false;
  char tagBuffer[256] = {};
  size_t tagLength = 0;
  uint32_t absoluteOffset = 0;
  uint32_t bodyContentStart = 0;
  uint32_t bodyEndOffset = 0;
  uint32_t rangeStart = 0;
  uint32_t currentUnits = 0;
  uint32_t bestCandidateOffset = 0;
  uint32_t bestCandidateUnits = 0;
  uint8_t bestCandidateScore = 0;
  uint8_t utf8ContinuationBytes = 0;
  int skipDepth = 0;
  int bodyDepth = 0;
  int splitBoundaryDepth = 1;
  bool bodyStarted = false;
  bool splitInsideRootWrapper = false;
  bool rootWrapperClosed = false;
  bool invalidRootWrapper = false;
  bool inAsciiWord = false;
  std::string currentHref;
  std::vector<std::string>& targetAnchors;
  std::deque<SourceRange> ranges;
  std::vector<AnchorOffset> anchors;
  std::vector<HrefTarget> hrefTargets;

  static bool isAsciiWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }
  static bool isAsciiWordByte(const char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
  void countVisibleByte(const uint8_t b) {
    const char c = static_cast<char>(b);
    if (isAsciiWhitespace(c)) {
      inAsciiWord = false;
      return;
    }
    if (b < 0x80) {
      utf8ContinuationBytes = 0;
      if (isAsciiWordByte(c)) {
        if (!inAsciiWord) {
          currentUnits += 2;
          inAsciiWord = true;
        }
      } else {
        currentUnits += 1;
        inAsciiWord = false;
      }
      return;
    }
    inAsciiWord = false;
    if ((b & 0xC0) == 0x80) {
      if (utf8ContinuationBytes > 0) utf8ContinuationBytes--;
      return;
    }
    if ((b & 0xE0) == 0xC0) {
      utf8ContinuationBytes = 1;
    } else if ((b & 0xF0) == 0xE0) {
      utf8ContinuationBytes = 2;
    } else if ((b & 0xF8) == 0xF0) {
      utf8ContinuationBytes = 3;
    } else {
      utf8ContinuationBytes = 0;
    }
    currentUnits += 1;
  }

  void appendTagChar(const char c) {
    if (tagLength + 1 < sizeof(tagBuffer)) {
      tagBuffer[tagLength++] = c;
      tagBuffer[tagLength] = '\0';
    }
  }

  char lowerTagChar(const size_t index) const {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(tagBuffer[index])));
  }

  bool tagNameIs(const char* name) const {
    size_t i = 1;
    if (tagBuffer[i] == '/') i++;
    while (tagBuffer[i] == ' ' || tagBuffer[i] == '\t') i++;
    for (size_t j = 0; name[j] != '\0'; j++, i++) {
      if (lowerTagChar(i) != name[j]) return false;
    }
    return tagBuffer[i] == '>' || tagBuffer[i] == '/' || std::isspace(static_cast<unsigned char>(tagBuffer[i])) != 0;
  }

  bool isClosingTag() const { return tagLength > 2 && tagBuffer[1] == '/'; }

  bool isSelfClosingTag() const {
    for (size_t i = tagLength; i > 0; i--) {
      const char c = tagBuffer[i - 1];
      if (c == '/') return true;
      if (c != '>' && !std::isspace(static_cast<unsigned char>(c))) return false;
    }
    return false;
  }

  bool isHeadingTag() const {
    size_t i = 1;
    if (tagBuffer[i] == '/') i++;
    return lowerTagChar(i) == 'h' && tagBuffer[i + 1] >= '1' && tagBuffer[i + 1] <= '6' &&
           (tagBuffer[i + 2] == '>' || tagBuffer[i + 2] == '/' ||
            std::isspace(static_cast<unsigned char>(tagBuffer[i + 2])) != 0);
  }

  std::string extractAttributeValue(const char* attrName, const bool lower = true) const {
    const std::string needle = std::string(attrName) + "=";
    const auto lowerTag = lowercase(std::string(tagBuffer));
    const auto foundPos = lowerTag.find(needle);
    if (foundPos == std::string::npos) return "";
    size_t valuePos = foundPos + needle.size();
    const char quote = tagBuffer[valuePos];
    if (quote != '\'' && quote != '"') return "";
    valuePos++;

    std::string value;
    while (tagBuffer[valuePos] != '\0' && tagBuffer[valuePos] != quote) {
      value.push_back(tagBuffer[valuePos++]);
    }
    return lower ? lowercase(value) : value;
  }

  void recordAnchor() {
    if (!bodyStarted || isClosingTag()) return;
    auto id = extractAttributeValue("id");
    if (id.empty()) {
      id = extractAttributeValue("name");
    }
    if (!id.empty() && anchorListContains(targetAnchors, id)) {
      anchors.push_back({id, absoluteOffset});
    }
  }

  void recordHrefTarget() {
    if (!bodyStarted || isClosingTag()) return;
    const auto href = extractAttributeValue("href", false);
    const auto hashPos = href.find('#');
    if (hashPos == std::string::npos || hashPos + 1 >= href.size()) {
      return;
    }

    const auto anchor = lowercase(href.substr(hashPos + 1));
    const auto targetHref = hashPos == 0 ? std::string() : href.substr(0, hashPos);
    if (targetHref.empty() || normaliseRelativeHref(currentHref, targetHref) == currentHref) {
      if (!anchorListContains(targetAnchors, anchor)) {
        targetAnchors.push_back(anchor);
      }
    }
    for (const auto& target : hrefTargets) {
      if (target.href == targetHref && target.anchor == anchor) {
        return;
      }
    }
    hrefTargets.push_back({targetHref, anchor});
  }

  void considerCandidate(const uint8_t score) {
    if (!bodyStarted || currentUnits < SPLIT_MIN_TEXT_UNITS) return;
    const uint32_t offset = absoluteOffset;
    const bool pastTarget = currentUnits >= SPLIT_TARGET_TEXT_UNITS;
    const bool hardMax = currentUnits >= SPLIT_HARD_MAX_TEXT_UNITS;
    const uint32_t newDistance = currentUnits > SPLIT_TARGET_TEXT_UNITS ? currentUnits - SPLIT_TARGET_TEXT_UNITS
                                                                        : SPLIT_TARGET_TEXT_UNITS - currentUnits;
    const uint32_t bestDistance = bestCandidateUnits > SPLIT_TARGET_TEXT_UNITS
                                      ? bestCandidateUnits - SPLIT_TARGET_TEXT_UNITS
                                      : SPLIT_TARGET_TEXT_UNITS - bestCandidateUnits;
    if (hardMax || score > bestCandidateScore || bestCandidateOffset == 0 ||
        (score == bestCandidateScore && newDistance < bestDistance)) {
      bestCandidateOffset = offset;
      bestCandidateUnits = currentUnits;
      bestCandidateScore = score;
    }
    if ((pastTarget && bestCandidateOffset != 0) || hardMax) {
      commitRange(bestCandidateOffset, bestCandidateUnits);
    }
  }

  void commitRange(const uint32_t endOffset, const uint32_t units) {
    if (endOffset <= rangeStart) return;
    ranges.push_back({rangeStart, endOffset, units});
    rangeStart = endOffset;
    currentUnits = 0;
    bestCandidateOffset = 0;
    bestCandidateUnits = 0;
    bestCandidateScore = 0;
    utf8ContinuationBytes = 0;
    inAsciiWord = false;
  }

  void finishTag() {
    const bool closing = isClosingTag();
    const bool bodyTag = tagNameIs("body");

    if (bodyTag) {
      if (closing) {
        if (!splitInsideRootWrapper) {
          bodyEndOffset = absoluteOffset - tagLength;
        }
      } else if (!bodyStarted) {
        bodyStarted = true;
        bodyContentStart = absoluteOffset;
        rangeStart = bodyContentStart;
      }
    }

    if (bodyStarted && splitInsideRootWrapper && rootWrapperClosed && !closing && !bodyTag && bodyDepth == 0 &&
        !isSelfClosingTag()) {
      invalidRootWrapper = true;
    }

    if (bodyStarted && !splitInsideRootWrapper && !closing && !bodyTag && bodyDepth == 0 && tagNameIs("div") &&
        !isSelfClosingTag()) {
      splitInsideRootWrapper = true;
      splitBoundaryDepth = 2;
      bodyContentStart = absoluteOffset;
      rangeStart = bodyContentStart;
    }

    if (tagNameIs("script") || tagNameIs("style") || tagNameIs("rt")) {
      if (closing) {
        if (skipDepth > 0) skipDepth--;
      } else if (!isSelfClosingTag()) {
        skipDepth++;
      }
    }

    recordAnchor();
    recordHrefTarget();

    if (closing && bodyStarted && bodyDepth == splitBoundaryDepth) {
      if (tagNameIs("h1") || tagNameIs("h2")) {
        considerCandidate(90);
      } else if (isHeadingTag()) {
        considerCandidate(80);
      } else if (tagNameIs("section") || tagNameIs("article")) {
        considerCandidate(65);
      } else if (tagNameIs("p") || tagNameIs("li") || tagNameIs("blockquote")) {
        considerCandidate(25);
      }
    }

    if (closing && splitInsideRootWrapper && !rootWrapperClosed && bodyDepth == 1 && tagNameIs("div")) {
      bodyEndOffset = absoluteOffset - tagLength;
      rootWrapperClosed = true;
    }

    if (bodyStarted && !bodyTag && !isSelfClosingTag()) {
      bodyDepth += closing ? -1 : 1;
      if (bodyDepth < 0) bodyDepth = 0;
    }

    inTag = false;
    tagLength = 0;
    tagBuffer[0] = '\0';
  }
};
}  // namespace

/* ============= WRITING / BUILDING FUNCTIONS ================ */

bool BookMetadataCache::beginWrite() {
  buildMode = true;
  spineCount = 0;
  tocCount = 0;
  anchorCount = 0;
  LOG_DBG("BMC", "Entering write mode");
  return true;
}

bool BookMetadataCache::beginContentOpfPass() {
  LOG_DBG("BMC", "Beginning content opf pass");

  // Open spine file for writing
  return Storage.openFileForWrite("BMC", cachePath + tmpSpineBinFile, spineFile);
}

bool BookMetadataCache::endContentOpfPass() {
  // Explicit close() required: member variable persists beyond function scope
  spineFile.close();
  return true;
}

bool BookMetadataCache::beginTocPass() {
  LOG_DBG("BMC", "Beginning toc pass");

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  if (!Storage.openFileForWrite("BMC", cachePath + tmpTocBinFile, tocFile)) {
    // Explicit close() required: member variable persists beyond function scope
    spineFile.close();
    return false;
  }

  if (spineCount >= LARGE_SPINE_THRESHOLD) {
    spineHrefIndex.clear();
    spineHrefIndex.resize(spineCount);
    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto entry = readSpineEntry(spineFile);
      SpineHrefIndexEntry idx;
      idx.hrefHash = fnvHash64(entry.href);
      idx.hrefLen = static_cast<uint16_t>(entry.href.size());
      idx.spineIndex = static_cast<int16_t>(i);
      spineHrefIndex[i] = idx;
    }
    std::sort(spineHrefIndex.begin(), spineHrefIndex.end(),
              [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
              });
    spineFile.seek(0);
    useSpineHrefIndex = true;
    LOG_DBG("BMC", "Using fast index for %d spine items", spineCount);
  } else {
    useSpineHrefIndex = false;
  }

  return true;
}

bool BookMetadataCache::endTocPass() {
  // Explicit close() required: member variables persist beyond function scope
  tocFile.close();
  spineFile.close();

  spineHrefIndex.clear();
  spineHrefIndex.shrink_to_fit();
  useSpineHrefIndex = false;

  return true;
}

bool BookMetadataCache::endWrite() {
  if (!buildMode) {
    LOG_DBG("BMC", "endWrite called but not in build mode");
    return false;
  }

  buildMode = false;
  LOG_DBG("BMC", "Wrote %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

bool BookMetadataCache::buildBookBin(const std::string& epubPath, const BookMetadata& metadata) {
  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpTocBinFile, tocFile)) {
    spineFile.close();
    return false;
  }

  std::deque<SpineEntry> physicalSpines;
  physicalSpines.resize(spineCount);
  spineFile.seek(0);
  for (int i = 0; i < spineCount; i++) {
    physicalSpines[i] = readSpineEntry(spineFile);
    physicalSpines[i].physicalSpineIndex = static_cast<uint16_t>(i);
  }

  std::deque<TocEntry> tocEntries;
  tocEntries.resize(tocCount);
  tocFile.seek(0);
  for (int i = 0; i < tocCount; i++) {
    tocEntries[i] = readTocEntry(tocFile);
  }

  const uint16_t physicalSpineCount = spineCount;

  // Build physical spineIndex->tocIndex mapping in one pass.
  std::deque<int16_t> physicalSpineToTocIndex(physicalSpineCount, -1);
  for (int j = 0; j < tocCount; j++) {
    const auto spineIndex = tocEntries[j].spineIndex;
    if (spineIndex >= 0 && spineIndex < physicalSpineCount && physicalSpineToTocIndex[spineIndex] == -1) {
      physicalSpineToTocIndex[spineIndex] = static_cast<int16_t>(j);
    }
  }

  ZipFile zip(epubPath);
  if (!zip.open()) {
    LOG_ERR("BMC", "Could not open EPUB zip for size calculations");
    spineFile.close();
    tocFile.close();
    return false;
  }

  std::deque<uint32_t> physicalSpineSizes(physicalSpineCount, 0);
  if (physicalSpineCount >= LARGE_SPINE_THRESHOLD) {
    LOG_DBG("BMC", "Using batch size lookup for %d spine items", physicalSpineCount);

    std::deque<ZipFile::SizeTarget> targets;
    targets.resize(physicalSpineCount);
    for (int i = 0; i < physicalSpineCount; i++) {
      std::string path = FsHelpers::normalisePath(physicalSpines[i].href);

      ZipFile::SizeTarget t;
      t.hash = ZipFile::fnvHash64(path.c_str(), path.size());
      t.len = static_cast<uint16_t>(path.size());
      t.index = static_cast<uint16_t>(i);
      targets[i] = t;
    }

    std::sort(targets.begin(), targets.end(), [](const ZipFile::SizeTarget& a, const ZipFile::SizeTarget& b) {
      return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
    });

    const int matched = zip.fillUncompressedSizes(targets, physicalSpineSizes);
    LOG_DBG("BMC", "Batch lookup matched %d/%d spine items", matched, physicalSpineCount);

    targets.clear();
    targets.shrink_to_fit();
  }

  for (int i = 0; i < physicalSpineCount; i++) {
    if (physicalSpineSizes[i] != 0) continue;

    const std::string path = FsHelpers::normalisePath(physicalSpines[i].href);
    size_t itemSize = 0;
    if (zip.getInflatedFileSize(path.c_str(), &itemSize)) {
      physicalSpineSizes[i] = static_cast<uint32_t>(itemSize);
    } else {
      LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
    }
  }

  bool hasSplittableSpines = false;
  for (int i = 0; i < physicalSpineCount; i++) {
    if (physicalSpineSizes[i] >= SPLIT_SOURCE_SIZE_THRESHOLD) {
      hasSplittableSpines = true;
      break;
    }
  }

  std::vector<std::vector<std::string>> physicalTocAnchors;
  std::vector<std::vector<AnchorOffset>> physicalAnchorOffsets;
  if (hasSplittableSpines) {
    physicalTocAnchors.resize(physicalSpineCount);
    physicalAnchorOffsets.resize(physicalSpineCount);
    for (const auto& tocEntry : tocEntries) {
      if (tocEntry.spineIndex >= 0 && tocEntry.spineIndex < physicalSpineCount && !tocEntry.anchor.empty()) {
        const auto anchor = lowercase(tocEntry.anchor);
        if (!anchorListContains(physicalTocAnchors[tocEntry.spineIndex], anchor)) {
          physicalTocAnchors[tocEntry.spineIndex].push_back(anchor);
        }
      }
    }
  } else {
    LOG_DBG("BMC", "No large spine items; skipping virtual split anchor scan");
  }

  std::deque<SpineEntry> readerSpines;
  std::deque<int16_t> physicalSpineToFirstReaderIndex(physicalSpineCount, -1);
  int16_t lastSpineTocIndex = -1;
  uint32_t cumulativeSize = 0;

  for (int i = 0; i < physicalSpineCount; i++) {
    auto baseEntry = physicalSpines[i];
    baseEntry.tocIndex = physicalSpineToTocIndex[i];
    if (baseEntry.tocIndex == -1) {
      LOG_DBG("BMC", "Warning: Could not find TOC entry for spine item %d: %s, using title from last section", i,
              baseEntry.href.c_str());
      baseEntry.tocIndex = lastSpineTocIndex;
    }
    lastSpineTocIndex = baseEntry.tocIndex;

    std::deque<SourceRange> ranges;
    const auto itemSize = physicalSpineSizes[i];
    if (hasSplittableSpines && itemSize >= SPLIT_SOURCE_SIZE_THRESHOLD) {
      const std::string path = FsHelpers::normalisePath(baseEntry.href);
      SpineSplitScanner scanner(baseEntry.href, physicalTocAnchors[i]);
      if (zip.readFileToStream(path.c_str(), scanner, 1024)) {
        ranges = scanner.finish();
        physicalAnchorOffsets[i] = scanner.getAnchors();
        for (const auto& target : scanner.getHrefTargets()) {
          const auto targetHref = normaliseRelativeHref(baseEntry.href, target.href);
          for (int j = 0; j < physicalSpineCount; j++) {
            if (physicalSpines[j].href == targetHref) {
              if (!anchorListContains(physicalTocAnchors[j], target.anchor)) {
                physicalTocAnchors[j].push_back(target.anchor);
              }
              break;
            }
          }
        }
      } else {
        LOG_ERR("BMC", "Could not scan spine item for virtual sections: %s", path.c_str());
      }
    }

    if (ranges.empty()) {
      ranges.push_back({0, itemSize, 0});
    }

    const uint16_t splitCount = static_cast<uint16_t>(std::min<size_t>(ranges.size(), UINT16_MAX));
    for (uint16_t splitIndex = 0; splitIndex < splitCount; splitIndex++) {
      auto entry = baseEntry;
      entry.physicalSpineIndex = static_cast<uint16_t>(i);
      entry.sourceStartOffset = ranges[splitIndex].start;
      entry.sourceEndOffset = ranges[splitIndex].end;
      entry.splitIndex = splitIndex;
      entry.splitCount = splitCount;

      const uint32_t segmentSize =
          entry.sourceEndOffset > entry.sourceStartOffset ? entry.sourceEndOffset - entry.sourceStartOffset : itemSize;
      cumulativeSize += segmentSize;
      entry.cumulativeSize = cumulativeSize;

      if (physicalSpineToFirstReaderIndex[i] == -1) {
        physicalSpineToFirstReaderIndex[i] = static_cast<int16_t>(readerSpines.size());
      }
      readerSpines.push_back(entry);
    }
  }
  zip.close();

  for (auto& tocEntry : tocEntries) {
    if (tocEntry.spineIndex >= 0 && tocEntry.spineIndex < physicalSpineCount) {
      const auto physicalIndex = tocEntry.spineIndex;
      tocEntry.spineIndex = physicalSpineToFirstReaderIndex[physicalIndex];
      if (hasSplittableSpines && !tocEntry.anchor.empty() && !physicalAnchorOffsets[physicalIndex].empty()) {
        auto targetAnchor = lowercase(tocEntry.anchor);
        for (const auto& anchor : physicalAnchorOffsets[physicalIndex]) {
          if (anchor.id != targetAnchor) continue;
          for (int i = tocEntry.spineIndex; i < static_cast<int>(readerSpines.size()); i++) {
            const auto& readerSpine = readerSpines[i];
            if (readerSpine.physicalSpineIndex != physicalIndex) break;
            if (anchor.offset >= readerSpine.sourceStartOffset && anchor.offset < readerSpine.sourceEndOffset) {
              tocEntry.spineIndex = static_cast<int16_t>(i);
              break;
            }
          }
          break;
        }
      }
    }
  }

  std::deque<AnchorEntry> anchorEntries;
  if (hasSplittableSpines) {
    for (int physicalIndex = 0; physicalIndex < physicalSpineCount; physicalIndex++) {
      if (physicalTocAnchors[physicalIndex].empty() || physicalAnchorOffsets[physicalIndex].empty()) {
        continue;
      }
      for (const auto& anchor : physicalAnchorOffsets[physicalIndex]) {
        if (!anchorListContains(physicalTocAnchors[physicalIndex], anchor.id)) {
          continue;
        }
        int16_t targetReaderSpineIndex = physicalSpineToFirstReaderIndex[physicalIndex];
        for (int i = targetReaderSpineIndex; i < static_cast<int>(readerSpines.size()); i++) {
          const auto& readerSpine = readerSpines[i];
          if (readerSpine.physicalSpineIndex != physicalIndex) break;
          if (anchor.offset >= readerSpine.sourceStartOffset && anchor.offset < readerSpine.sourceEndOffset) {
            targetReaderSpineIndex = static_cast<int16_t>(i);
            break;
          }
        }
        anchorEntries.push_back({physicalSpines[physicalIndex].href, anchor.id, targetReaderSpineIndex});
      }
    }
  }

  spineCount = static_cast<uint16_t>(std::min<size_t>(readerSpines.size(), UINT16_MAX));
  anchorCount = static_cast<uint16_t>(std::min<size_t>(anchorEntries.size(), UINT16_MAX));

  // Open book.bin after virtual sections are known, because the LUT size depends on the final reader spine count.
  if (!Storage.openFileForWrite("BMC", cachePath + bookBinFile, bookFile)) {
    spineFile.close();
    tocFile.close();
    return false;
  }

  constexpr uint32_t headerASize = sizeof(BOOK_CACHE_VERSION) + /* LUT Offset */ sizeof(uint32_t) + sizeof(spineCount) +
                                   sizeof(tocCount) + sizeof(anchorCount);
  const uint32_t metadataSize = metadata.title.size() + metadata.author.size() + metadata.language.size() +
                                metadata.coverItemHref.size() + metadata.textReferenceHref.size() +
                                sizeof(uint32_t) * 5 + sizeof(metadata.pageProgressionRtl);
  const uint32_t lutSize = sizeof(uint32_t) * spineCount + sizeof(uint32_t) * tocCount + sizeof(uint32_t) * anchorCount;
  const uint32_t lutOffset = headerASize + metadataSize;

  // Header A
  serialization::writePod(bookFile, BOOK_CACHE_VERSION);
  serialization::writePod(bookFile, lutOffset);
  serialization::writePod(bookFile, spineCount);
  serialization::writePod(bookFile, tocCount);
  serialization::writePod(bookFile, anchorCount);
  // Metadata
  serialization::writeString(bookFile, metadata.title);
  serialization::writeString(bookFile, metadata.author);
  serialization::writeString(bookFile, metadata.language);
  serialization::writeString(bookFile, metadata.coverItemHref);
  serialization::writeString(bookFile, metadata.textReferenceHref);
  serialization::writePod(bookFile, metadata.pageProgressionRtl);

  std::deque<uint32_t> spineEntryPositions;
  spineEntryPositions.resize(spineCount);
  for (int i = 0; i < spineCount; i++) {
    serialization::writePod(bookFile, static_cast<uint32_t>(0));
  }

  std::deque<uint32_t> tocEntryPositions;
  tocEntryPositions.resize(tocCount);
  for (int i = 0; i < tocCount; i++) {
    serialization::writePod(bookFile, static_cast<uint32_t>(0));
  }

  std::deque<uint32_t> anchorEntryPositions;
  anchorEntryPositions.resize(anchorCount);
  for (int i = 0; i < anchorCount; i++) {
    serialization::writePod(bookFile, static_cast<uint32_t>(0));
  }

  for (int i = 0; i < spineCount; i++) {
    spineEntryPositions[i] = writeSpineEntry(bookFile, readerSpines[i]);
  }

  for (int i = 0; i < tocCount; i++) {
    tocEntryPositions[i] = writeTocEntry(bookFile, tocEntries[i]);
  }

  for (int i = 0; i < anchorCount; i++) {
    anchorEntryPositions[i] = writeAnchorEntry(bookFile, anchorEntries[i]);
  }

  bookFile.seek(lutOffset);
  for (int i = 0; i < spineCount; i++) {
    serialization::writePod(bookFile, spineEntryPositions[i]);
  }
  for (int i = 0; i < tocCount; i++) {
    serialization::writePod(bookFile, tocEntryPositions[i]);
  }
  for (int i = 0; i < anchorCount; i++) {
    serialization::writePod(bookFile, anchorEntryPositions[i]);
  }

  // Explicit close() required: member variables persist beyond function scope
  bookFile.close();
  spineFile.close();
  tocFile.close();

  LOG_DBG("BMC", "Successfully built book.bin");
  return true;
}

bool BookMetadataCache::cleanupTmpFiles() const {
  const auto spineBinFile = cachePath + tmpSpineBinFile;
  if (Storage.exists(spineBinFile.c_str())) {
    Storage.remove(spineBinFile.c_str());
  }
  const auto tocBinFile = cachePath + tmpTocBinFile;
  if (Storage.exists(tocBinFile.c_str())) {
    Storage.remove(tocBinFile.c_str());
  }
  return true;
}

uint32_t BookMetadataCache::writeSpineEntry(HalFile& file, const SpineEntry& entry) const {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.href);
  serialization::writePod(file, entry.cumulativeSize);
  serialization::writePod(file, entry.tocIndex);
  serialization::writePod(file, entry.physicalSpineIndex);
  serialization::writePod(file, entry.sourceStartOffset);
  serialization::writePod(file, entry.sourceEndOffset);
  serialization::writePod(file, entry.splitIndex);
  serialization::writePod(file, entry.splitCount);
  return pos;
}

uint32_t BookMetadataCache::writeTocEntry(HalFile& file, const TocEntry& entry) const {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.title);
  serialization::writeString(file, entry.href);
  serialization::writeString(file, entry.anchor);
  serialization::writePod(file, entry.level);
  serialization::writePod(file, entry.spineIndex);
  return pos;
}

uint32_t BookMetadataCache::writeAnchorEntry(HalFile& file, const AnchorEntry& entry) const {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.href);
  serialization::writeString(file, entry.anchor);
  serialization::writePod(file, entry.spineIndex);
  return pos;
}

// Note: for the LUT to be accurate, this **MUST** be called for all spine items before `addTocEntry` is ever called
// this is because in this function we're marking positions of the items
void BookMetadataCache::createSpineEntry(const std::string& href) {
  if (!buildMode || !spineFile) {
    LOG_DBG("BMC", "createSpineEntry called but not in build mode");
    return;
  }

  const SpineEntry entry(href, 0, -1);
  writeSpineEntry(spineFile, entry);
  spineCount++;
}

void BookMetadataCache::createTocEntry(const std::string& title, const std::string& href, const std::string& anchor,
                                       const uint8_t level) {
  if (!buildMode || !tocFile || !spineFile) {
    LOG_DBG("BMC", "createTocEntry called but not in build mode");
    return;
  }

  int16_t spineIndex = -1;

  if (useSpineHrefIndex) {
    uint64_t targetHash = fnvHash64(href);
    uint16_t targetLen = static_cast<uint16_t>(href.size());

    auto it =
        std::lower_bound(spineHrefIndex.begin(), spineHrefIndex.end(), SpineHrefIndexEntry{targetHash, targetLen, 0},
                         [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                           return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
                         });

    while (it != spineHrefIndex.end() && it->hrefHash == targetHash && it->hrefLen == targetLen) {
      spineIndex = it->spineIndex;
      break;
    }

    if (spineIndex == -1) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  } else {
    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto spineEntry = readSpineEntry(spineFile);
      if (spineEntry.href == href) {
        spineIndex = static_cast<int16_t>(i);
        break;
      }
    }
    if (spineIndex == -1) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  }

  const TocEntry entry(title, href, anchor, level, spineIndex);
  writeTocEntry(tocFile, entry);
  tocCount++;
}

/* ============= READING / LOADING FUNCTIONS ================ */

bool BookMetadataCache::load() {
  if (!Storage.openFileForRead("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(bookFile, version);
  if (version != BOOK_CACHE_VERSION) {
    LOG_DBG("BMC", "Cache version mismatch: expected %d, got %d", BOOK_CACHE_VERSION, version);
    // Explicit close() required: member variable persists beyond function scope
    bookFile.close();
    return false;
  }

  serialization::readPod(bookFile, lutOffset);
  serialization::readPod(bookFile, spineCount);
  serialization::readPod(bookFile, tocCount);
  serialization::readPod(bookFile, anchorCount);

  serialization::readString(bookFile, coreMetadata.title);
  serialization::readString(bookFile, coreMetadata.author);
  serialization::readString(bookFile, coreMetadata.language);
  serialization::readString(bookFile, coreMetadata.coverItemHref);
  serialization::readString(bookFile, coreMetadata.textReferenceHref);
  serialization::readPod(bookFile, coreMetadata.pageProgressionRtl);

  loaded = true;
  LOG_DBG("BMC", "Loaded cache data: %d spine, %d TOC, %d anchor entries", spineCount, tocCount, anchorCount);
  return true;
}

BookMetadataCache::SpineEntry BookMetadataCache::getSpineEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getSpineEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(spineCount)) {
    LOG_ERR("BMC", "getSpineEntry index %d out of range", index);
    return {};
  }

  // Seek to spine LUT item, read from LUT and get out data
  bookFile.seek(lutOffset + sizeof(uint32_t) * index);
  uint32_t spineEntryPos;
  serialization::readPod(bookFile, spineEntryPos);
  bookFile.seek(spineEntryPos);
  return readSpineEntry(bookFile);
}

BookMetadataCache::TocEntry BookMetadataCache::getTocEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getTocEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(tocCount)) {
    LOG_ERR("BMC", "getTocEntry index %d out of range", index);
    return {};
  }

  // Seek to TOC LUT item, read from LUT and get out data
  bookFile.seek(lutOffset + sizeof(uint32_t) * spineCount + sizeof(uint32_t) * index);
  uint32_t tocEntryPos;
  serialization::readPod(bookFile, tocEntryPos);
  bookFile.seek(tocEntryPos);
  return readTocEntry(bookFile);
}

BookMetadataCache::AnchorEntry BookMetadataCache::getAnchorEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getAnchorEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(anchorCount)) {
    LOG_ERR("BMC", "getAnchorEntry index %d out of range", index);
    return {};
  }

  bookFile.seek(lutOffset + sizeof(uint32_t) * spineCount + sizeof(uint32_t) * tocCount + sizeof(uint32_t) * index);
  uint32_t anchorEntryPos;
  serialization::readPod(bookFile, anchorEntryPos);
  bookFile.seek(anchorEntryPos);
  return readAnchorEntry(bookFile);
}

int BookMetadataCache::resolveAnchorToSpineIndex(const std::string& href, const std::string& anchor) {
  if (!loaded || href.empty() || anchor.empty()) {
    return -1;
  }

  const auto normalizedAnchor = lowercase(anchor);
  const auto normalizedHref = FsHelpers::normalisePath(href);
  const auto slashPos = normalizedHref.find_last_of('/');
  const auto filename = slashPos == std::string::npos ? normalizedHref : normalizedHref.substr(slashPos + 1);

  for (int i = 0; i < anchorCount; i++) {
    const auto entry = getAnchorEntry(i);
    if (entry.anchor != normalizedAnchor) {
      continue;
    }
    if (entry.href == normalizedHref) {
      return entry.spineIndex;
    }
    const auto entrySlashPos = entry.href.find_last_of('/');
    const auto entryFilename = entrySlashPos == std::string::npos ? entry.href : entry.href.substr(entrySlashPos + 1);
    if (entryFilename == filename) {
      return entry.spineIndex;
    }
  }

  return -1;
}

BookMetadataCache::SpineEntry BookMetadataCache::readSpineEntry(HalFile& file) const {
  SpineEntry entry;
  serialization::readString(file, entry.href);
  serialization::readPod(file, entry.cumulativeSize);
  serialization::readPod(file, entry.tocIndex);
  serialization::readPod(file, entry.physicalSpineIndex);
  serialization::readPod(file, entry.sourceStartOffset);
  serialization::readPod(file, entry.sourceEndOffset);
  serialization::readPod(file, entry.splitIndex);
  serialization::readPod(file, entry.splitCount);
  return entry;
}

BookMetadataCache::TocEntry BookMetadataCache::readTocEntry(HalFile& file) const {
  TocEntry entry;
  serialization::readString(file, entry.title);
  serialization::readString(file, entry.href);
  serialization::readString(file, entry.anchor);
  serialization::readPod(file, entry.level);
  serialization::readPod(file, entry.spineIndex);
  return entry;
}

BookMetadataCache::AnchorEntry BookMetadataCache::readAnchorEntry(HalFile& file) const {
  AnchorEntry entry;
  serialization::readString(file, entry.href);
  serialization::readString(file, entry.anchor);
  serialization::readPod(file, entry.spineIndex);
  return entry;
}
