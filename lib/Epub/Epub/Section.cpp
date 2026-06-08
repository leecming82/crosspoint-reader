#include "Section.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <Serialization.h>
#include <Utf8.h>
#include <expat.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <string_view>

#include "Epub/DebugStyleConfig.h"
#include "Epub/WritingMode.h"
#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint8_t SECTION_FILE_VERSION = 43;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint8_t) + sizeof(bool) + sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
constexpr uint32_t SECTION_ADVANCE_CODEPOINT_LIMIT = 2048;

struct PageLutEntry {
  uint32_t fileOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
};

bool addUniqueCodepoint(std::vector<uint32_t>& codepoints, const uint32_t cp, const uint32_t limit = UINT32_MAX,
                        bool* hitCap = nullptr) {
  if (cp == 0 || cp == 0x00AD || cp < 0x20) {
    return false;
  }
  auto pos = std::lower_bound(codepoints.begin(), codepoints.end(), cp);
  if (pos != codepoints.end() && *pos == cp) {
    return false;
  }
  if (codepoints.size() >= limit) {
    if (hitCap) {
      *hitCap = true;
    }
    return false;
  }
  codepoints.insert(pos, cp);
  return true;
}

bool isCjkCodepoint(const uint32_t cp) {
  return (cp >= 0x3000 && cp <= 0x303F) ||  // CJK symbols and punctuation
         (cp >= 0x3040 && cp <= 0x30FF) ||  // Hiragana + Katakana
         (cp >= 0x31F0 && cp <= 0x31FF) ||  // Katakana phonetic extensions
         (cp >= 0x3400 && cp <= 0x4DBF) ||  // CJK extension A
         (cp >= 0x4E00 && cp <= 0x9FFF) ||  // CJK unified ideographs
         (cp >= 0xF900 && cp <= 0xFAFF) ||  // CJK compatibility ideographs
         (cp >= 0xFF00 && cp <= 0xFFEF) ||  // Fullwidth forms
         (cp >= 0x20000 && cp <= 0x2FA1F);  // CJK extensions B-F + compatibility supplement
}

struct SectionAdvanceScan {
  std::vector<uint32_t> codepoints;
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  bool hitCodepointCap = false;
  bool hasCjk = false;

  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<SectionAdvanceScan*>(userData);
    if (self->skipUntilDepth < self->depth) {
      self->depth++;
      return;
    }
    if (strcmp(name, "head") == 0 || strcmp(name, "script") == 0 || strcmp(name, "style") == 0) {
      self->skipUntilDepth = self->depth;
    }
    self->depth++;
  }

  static void XMLCALL endElement(void* userData, const XML_Char*) {
    auto* self = static_cast<SectionAdvanceScan*>(userData);
    self->depth--;
    if (self->skipUntilDepth == self->depth) {
      self->skipUntilDepth = INT_MAX;
    }
  }

  static void XMLCALL characterData(void* userData, const XML_Char* s, const int len) {
    auto* self = static_cast<SectionAdvanceScan*>(userData);
    if (self->skipUntilDepth < INT_MAX || self->hitCodepointCap || len <= 0) {
      return;
    }
    const std::string_view text(s, static_cast<size_t>(len));
    const auto* ptr = reinterpret_cast<const unsigned char*>(text.data());
    const auto* end = ptr + text.size();
    while (ptr < end && !self->hitCodepointCap) {
      const uint32_t cp = utf8NextCodepoint(&ptr);
      if (cp == 0) break;
      if (isCjkCodepoint(cp)) {
        self->hasCjk = true;
      }
      addUniqueCodepoint(self->codepoints, cp, SECTION_ADVANCE_CODEPOINT_LIMIT, &self->hitCodepointCap);
    }
  }
};

void destroyXmlParser(XML_Parser parser) {
  if (parser) {
    XML_ParserFree(parser);
  }
}

bool scanSectionAdvanceCodepoints(const std::string& tmpHtmlPath, std::vector<uint32_t>& codepoints, bool& hitCap,
                                  bool& hasCjk, const uint32_t sourceStartOffset = 0,
                                  const uint32_t sourceEndOffset = 0, const std::string& fragmentPrefix = {},
                                  const std::string& fragmentSuffix = {}) {
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_ERR("SCT", "Could not allocate advance scan parser");
    return false;
  }

  SectionAdvanceScan scan;
  XML_SetUserData(parser, &scan);
  XML_SetElementHandler(parser, SectionAdvanceScan::startElement, SectionAdvanceScan::endElement);
  XML_SetCharacterDataHandler(parser, SectionAdvanceScan::characterData);

  HalFile file;
  if (!Storage.openFileForRead("SCT", tmpHtmlPath, file)) {
    destroyXmlParser(parser);
    return false;
  }

  const size_t fileSize = file.size();
  const bool scanFragment = sourceStartOffset > 0 || (sourceEndOffset > 0 && sourceEndOffset < fileSize);
  const size_t scanStart = scanFragment ? std::min<size_t>(sourceStartOffset, fileSize) : 0;
  const size_t scanEnd =
      scanFragment && sourceEndOffset > scanStart ? std::min<size_t>(sourceEndOffset, fileSize) : fileSize;
  const size_t scanSize = scanEnd > scanStart ? scanEnd - scanStart : 0;

  if (scanFragment) {
    const std::string& prefix = fragmentPrefix.empty() ? std::string("<html><body>") : fragmentPrefix;
    if (XML_Parse(parser, prefix.c_str(), static_cast<int>(prefix.size()), XML_FALSE) == XML_STATUS_ERROR) {
      LOG_ERR("SCT", "Advance scan parse error in fragment prefix: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      file.close();
      destroyXmlParser(parser);
      return false;
    }
    file.seek(scanStart);
  }

  constexpr size_t SCAN_BUFFER_SIZE = 4096;
  size_t bytesRead = 0;
  int done = 0;
  do {
    void* const buf = XML_GetBuffer(parser, SCAN_BUFFER_SIZE);
    if (!buf) {
      LOG_ERR("SCT", "Could not allocate advance scan XML buffer");
      file.close();
      destroyXmlParser(parser);
      return false;
    }

    const size_t remaining = scanFragment ? scanSize - bytesRead : SCAN_BUFFER_SIZE;
    const size_t wanted = scanFragment ? std::min<size_t>(SCAN_BUFFER_SIZE, remaining) : SCAN_BUFFER_SIZE;
    const size_t len = wanted > 0 ? file.read(buf, wanted) : 0;
    bytesRead += len;

    if (len == 0 && (!scanFragment && file.available() > 0)) {
      LOG_ERR("SCT", "Advance scan file read error");
      file.close();
      destroyXmlParser(parser);
      return false;
    }

    done = scanFragment ? bytesRead >= scanSize : file.available() == 0;
    if (XML_ParseBuffer(parser, static_cast<int>(len), done && !scanFragment) == XML_STATUS_ERROR) {
      LOG_ERR("SCT", "Advance scan parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      file.close();
      destroyXmlParser(parser);
      return false;
    }
  } while (!done);

  if (scanFragment) {
    const std::string& suffix = fragmentSuffix.empty() ? std::string("</body></html>") : fragmentSuffix;
    if (XML_Parse(parser, suffix.c_str(), static_cast<int>(suffix.size()), XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("SCT", "Advance scan parse error in fragment suffix at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      file.close();
      destroyXmlParser(parser);
      return false;
    }
  }

  file.close();
  destroyXmlParser(parser);
  hitCap = scan.hitCodepointCap;
  hasCjk = scan.hasCjk;
  codepoints = std::move(scan.codepoints);
  return true;
}

}  // namespace

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", pageCount);

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                     const uint16_t viewportHeight, const bool hyphenationEnabled,
                                     const bool embeddedStyle, const uint8_t imageRendering,
                                     const bool focusReadingEnabled, const uint8_t readingLayout,
                                     const uint8_t writingMode) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(embeddedStyle) + sizeof(imageRendering) + sizeof(focusReadingEnabled) +
                                   sizeof(readingLayout) + sizeof(writingMode) + sizeof(uint32_t) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, embeddedStyle);
  serialization::writePod(file, imageRendering);
  serialization::writePod(file, focusReadingEnabled);
  serialization::writePod(file, readingLayout);
  serialization::writePod(file, writingMode);
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for paragraph LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for li LUT offset (patched later)
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                              const uint8_t imageRendering, const bool focusReadingEnabled, const uint8_t readingLayout,
                              const uint8_t writingMode) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  {
    uint8_t version;
    serialization::readPod(file, version);
    if (version != SECTION_FILE_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      return false;
    }

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    bool fileFocusReadingEnabled;
    uint8_t fileReadingLayout;
    uint8_t fileWritingMode;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileEmbeddedStyle);
    serialization::readPod(file, fileImageRendering);
    serialization::readPod(file, fileFocusReadingEnabled);
    serialization::readPod(file, fileReadingLayout);
    serialization::readPod(file, fileWritingMode);

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
        viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
        hyphenationEnabled != fileHyphenationEnabled || embeddedStyle != fileEmbeddedStyle ||
        imageRendering != fileImageRendering || focusReadingEnabled != fileFocusReadingEnabled ||
        readingLayout != fileReadingLayout || writingMode != fileWritingMode) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  serialization::readPod(file, pageCount);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages", pageCount);
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  const auto glyphPackPath = getGlyphPackPath();
  if (Storage.exists(glyphPackPath.c_str()) && !Storage.remove(glyphPackPath.c_str())) {
    LOG_ERR("SCT", "Failed to clear section glyph pack");
  }

  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                                const uint8_t imageRendering, const bool focusReadingEnabled,
                                const uint8_t readingLayout, const uint8_t writingMode,
                                const std::function<void(size_t, size_t)>& progressFn) {
  const auto spineItem = epub->getSpineItem(spineIndex);
  const auto localPath = spineItem.href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Retry logic for SD card timing issues
  bool success = false;
  uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      delay(50);  // Brief delay before retry
    }

    // Remove any incomplete file from previous attempt before retrying
    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }

    HalFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    // Explicitly close() file before calling Storage.remove()
    tmpHtml.close();

    // If streaming failed, remove the incomplete file immediately
    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }

  if (!success) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }

  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes)", tmpHtmlPath.c_str(), fileSize);

  bool sdAdvancePrewarmed = false;
  bool shouldBuildGlyphPack = false;
  bool includeVerticalSubstitutions = false;
  std::vector<uint32_t> sectionCodepoints;
  // Preload SD-font advance metrics for this section so CJK layout avoids per-block glyph metadata reads.
  if (renderer.isSdCardFont(fontId)) {
    bool hitCodepointCap = false;
    bool hasCjk = false;
    if (scanSectionAdvanceCodepoints(tmpHtmlPath, sectionCodepoints, hitCodepointCap, hasCjk,
                                     spineItem.sourceStartOffset, spineItem.sourceEndOffset, spineItem.fragmentPrefix,
                                     spineItem.fragmentSuffix)) {
      if (!hasCjk) {
        LOG_DBG("SCT", "Section advance prewarm skipped: no CJK text");
      } else if (hitCodepointCap) {
        LOG_DBG("SCT", "Section advance prewarm skipped: codepoint cap hit");
      } else {
        addUniqueCodepoint(sectionCodepoints, ' ');
        if (hyphenationEnabled) {
          addUniqueCodepoint(sectionCodepoints, '-');
        }
        addUniqueCodepoint(sectionCodepoints, 0x65E5);  // 日
        addUniqueCodepoint(sectionCodepoints, 0x3042);  // あ

        includeVerticalSubstitutions = static_cast<EpubWritingMode>(writingMode) != EpubWritingMode::HorizontalTb;
        const auto& sdFonts = renderer.getSdCardFonts();
        auto fontIt = sdFonts.find(fontId);
        if (fontIt != sdFonts.end() && fontIt->second) {
          const int missed = fontIt->second->buildAdvanceTableFromCodepoints(
              sectionCodepoints.data(), static_cast<uint32_t>(sectionCodepoints.size()), 0x0F,
              includeVerticalSubstitutions);
          sdAdvancePrewarmed = missed >= 0;
          if (missed > 0) {
            LOG_DBG("SCT", "Section advance prewarm: %d codepoint(s) not found", missed);
          }
          shouldBuildGlyphPack = sdAdvancePrewarmed;
        }
      }
    } else {
      LOG_ERR("SCT", "Section advance prewarm scan failed; falling back to block prewarm");
    }
  }

  if (shouldBuildGlyphPack && !sectionCodepoints.empty()) {
    const auto& sdFonts = renderer.getSdCardFonts();
    auto fontIt = sdFonts.find(fontId);
    if (fontIt != sdFonts.end() && fontIt->second) {
      fontIt->second->buildSectionGlyphPackFromCodepoints(
          sectionCodepoints.data(), static_cast<uint32_t>(sectionCodepoints.size()), 0x0F, getGlyphPackPath().c_str(),
          includeVerticalSubstitutions);
    }
  }

  if (!Storage.openFileForWrite("SCT", filePath, file)) {
    Storage.remove(getGlyphPackPath().c_str());
    return false;
  }
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled,
                         readingLayout, writingMode);
  std::vector<PageLutEntry> lut = {};

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  CssParser* cssParser = nullptr;
  const bool applyEmbeddedStyle = embeddedStyle && !DEBUG_IGNORE_EMBEDDED_EPUB_CSS;
  if (applyEmbeddedStyle) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      if (!cssParser->loadFromCache()) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries
  std::vector<std::string> tocAnchors;
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
      viewportHeight, hyphenationEnabled, focusReadingEnabled,
      [this, &lut](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex) {
        lut.push_back({this->onPageComplete(std::move(page)), paragraphIndex, listItemIndex});
      },
      applyEmbeddedStyle, contentBase, imageBasePath, imageRendering, progressFn, cssParser,
      spineItem.sourceStartOffset, spineItem.sourceEndOffset, static_cast<EpubWritingMode>(writingMode),
      sdAdvancePrewarmed, spineItem.fragmentPrefix, spineItem.fragmentSuffix, std::move(tocAnchors));
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  success = visitor.parseAndBuildPages();

  Storage.remove(tmpHtmlPath.c_str());
  if (!success) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filePath.c_str());
    Storage.remove(getGlyphPackPath().c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT
  for (const auto& entry : lut) {
    if (entry.fileOffset == 0) {
      hasFailedLutRecords = true;
      break;
    }
    serialization::writePod(file, entry.fileOffset);
  }

  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filePath.c_str());
    Storage.remove(getGlyphPackPath().c_str());
    return false;
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets)
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = visitor.getAnchors();
  serialization::writePod(file, static_cast<uint16_t>(anchors.size()));
  for (const auto& [anchor, page] : anchors) {
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
  }

  const uint32_t paragraphLutOffset = file.position();
  serialization::writePod(file, static_cast<uint16_t>(lut.size()));
  for (const auto& entry : lut) {
    serialization::writePod(file, entry.paragraphIndex);
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : lut) {
    serialization::writePod(file, entry.listItemIndex);
  }

  // Patch header with final pageCount, lutOffset, anchorMapOffset, paragraphLutOffset, and liLutOffset
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, paragraphLutOffset);
  serialization::writePod(file, liLutFileOffset);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  if (cssParser) {
    cssParser->clear();
  }
  return true;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (currentPage < 0 || currentPage >= pageCount) {
    return nullptr;
  }
  return loadPageFromSectionFile(static_cast<uint16_t>(currentPage));
}

std::unique_ptr<Page> Section::loadPageFromSectionFile(const uint16_t pageNumber) {
  if (pageNumber >= pageCount) {
    return nullptr;
  }

  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return nullptr;
  }

  file.seek(HEADER_SIZE - sizeof(uint32_t) * 4);
  uint32_t lutOffset;
  serialization::readPod(file, lutOffset);
  file.seek(lutOffset + sizeof(uint32_t) * pageNumber);
  uint32_t pagePos;
  serialization::readPod(file, pagePos);
  file.seek(pagePos);

  auto page = Page::deserialize(file);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  return page;
}

std::string Section::getTextFromSectionFile() {
  std::string fullText;
  auto p = this->loadPageFromSectionFile();
  if (p) {
    for (const auto& el : p->elements) {
      if (el->getTag() == TAG_PageLine) {
        const auto& line = static_cast<const PageLine&>(*el);
        if (line.getBlock()) {
          const auto& words = line.getBlock()->getWords();
          for (const auto& w : words) {
            if (!fullText.empty()) fullText += " ";
            fullText += w;
          }
        }
      }
    }
  }
  return fullText;
}

std::optional<uint16_t> Section::getCachedPageCount() const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE) {
    return std::nullopt;
  }

  f.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(uint16_t));
  uint16_t count;
  serialization::readPod(f, count);
  return count;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 3);
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(anchorMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    serialization::readString(f, key);
    serialization::readPod(f, page);
    if (key == anchor) {
      return page;
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = paragraphLutOffset + sizeof(uint16_t) + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx;
    serialization::readPod(f, pagePIdx);
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset + sizeof(uint16_t) + page * sizeof(uint16_t));
  uint16_t pIdx;
  serialization::readPod(f, pIdx);
  return pIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t liLutOffset;
  serialization::readPod(f, liLutOffset);
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = liLutOffset + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(liLutOffset);
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx;
    serialization::readPod(f, pageLiIdx);
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}
