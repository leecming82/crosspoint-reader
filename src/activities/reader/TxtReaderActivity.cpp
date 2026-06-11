#include "TxtReaderActivity.h"

#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReaderFontConfig.h"
#include "ReaderFontProvider.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#ifdef CROSSPOINT_BOARD_MURPHY_M4
#include "TtfReaderMetrics.h"
#endif
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
constexpr int TXT_PAGE_CACHE_BATCH = 50;
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 6;          // Increment when cache format changes

void writeReaderFontIdentity(HalFile& file, const ReaderFontIdentity& identity) {
  serialization::writePod(file, identity.version);
  serialization::writePod(file, identity.mode);
  serialization::writePod(file, identity.provider);
  serialization::writePod(file, identity.pixelSize);
  serialization::writePod(file, identity.reserved);
  serialization::writePod(file, identity.weight);
  serialization::writePod(file, identity.fileSize);
  serialization::writePod(file, identity.fileHash);
  serialization::writePod(file, identity.providerVersion);
  serialization::writePod(file, identity.legacyFontId);
}

ReaderFontIdentity readReaderFontIdentity(HalFile& file) {
  ReaderFontIdentity identity;
  serialization::readPod(file, identity.version);
  serialization::readPod(file, identity.mode);
  serialization::readPod(file, identity.provider);
  serialization::readPod(file, identity.pixelSize);
  serialization::readPod(file, identity.reserved);
  if (identity.version >= 2) {
    serialization::readPod(file, identity.weight);
  } else {
    identity.weight = 400;
  }
  serialization::readPod(file, identity.fileSize);
  serialization::readPod(file, identity.fileHash);
  serialization::readPod(file, identity.providerVersion);
  serialization::readPod(file, identity.legacyFontId);
  return identity;
}

int clampPercent(const int percent) {
  if (percent < 0) return 0;
  if (percent > 100) return 100;
  return percent;
}

int measureTxtLineForIndexing(const GfxRenderer& renderer, const int fontId, const std::string& line) {
  return renderer.getTextAdvanceX(fontId, line.c_str(), EpdFontFamily::REGULAR);
}

size_t firstUtf8CodepointLength(const std::string& line) {
  if (line.empty()) return 0;
  const auto* start = reinterpret_cast<const unsigned char*>(line.c_str());
  const auto* p = start;
  utf8NextCodepoint(&p);
  const size_t len = static_cast<size_t>(p - start);
  return len > 0 ? len : 1;
}
}  // namespace

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  currentPageLines.clear();
  endOfFileKnown = false;
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void TxtReaderActivity::loop() {
  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(txt ? txt->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  if (handleTouchZones()) {
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  (void)fromTilt;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered) {
    turnPage(false);
  } else if (nextTriggered) {
    turnPage(true);
  }
}

void TxtReaderActivity::turnPage(const bool forward) {
  if (!forward) {
    if (currentPage <= 0) {
      return;
    }
    currentPage--;
    requestUpdate();
    return;
  }

  if (currentPage + 1 >= static_cast<int>(pageOffsets.size()) && !endOfFileKnown) {
    ensurePageCacheAhead(TXT_PAGE_CACHE_BATCH, true);
    savePageIndexCache();
  }
  if (currentPage + 1 < static_cast<int>(pageOffsets.size())) {
    currentPage++;
    requestUpdate();
  } else if (endOfFileKnown) {
    onGoHome();
  }
}

bool TxtReaderActivity::handleTouchZones() {
  if (!mappedInput.wasTapped()) {
    return false;
  }

  const auto tap = mappedInput.lastTap();
  const int screenWidth = renderer.getScreenWidth();
  const int centerLeft = screenWidth / 3;
  const int centerRight = (screenWidth * 2) / 3;

  if (tap.x < centerLeft) {
    turnPage(false);
    return true;
  }
  if (tap.x >= centerRight) {
    turnPage(true);
    return true;
  }

  return true;
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  readerFontConfig = ReaderFontResolver::resolveGlobal();
  cachedFontId = effectiveLayoutFontId();
  cachedRenderFontId = effectiveRenderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  // Try to load cached page index first. If no valid cache exists, start with
  // only the first page and grow the index in bounded batches.
  if (!loadPageIndexCache()) {
    pageOffsets.clear();
    if (txt->getFileSize() > 0) {
      pageOffsets.push_back(0);
    }
    endOfFileKnown = txt->getFileSize() == 0;
    updateEstimatedTotalPages();
  }

  // Load saved progress
  loadProgress();
  if (currentPage >= static_cast<int>(pageOffsets.size())) {
    currentPage = std::max(0, static_cast<int>(pageOffsets.size()) - 1);
  }
  ensurePageCacheAhead(TXT_PAGE_CACHE_BATCH, true);
  savePageIndexCache();

  initialized = true;
}

int TxtReaderActivity::effectiveRenderFontId() const {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  if (readerFontConfig.isTtf()) {
    ReaderFontProvider* provider = ReaderFontProviders::providerForConfig(readerFontConfig);
    if (provider && provider->ensureLoaded(readerFontConfig)) {
      renderer.setReaderFontMetricsProvider(provider);
      return provider->fontId();
    }
    LOG_ERR("TRS", "TTF reader render unavailable; using built-in font for this pass");
  }
#endif
  return UI_12_FONT_ID;
}

int TxtReaderActivity::effectiveLayoutFontId() const {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  if (readerFontConfig.isTtf()) {
    ReaderFontProvider* provider = ReaderFontProviders::providerForConfig(readerFontConfig);
    if (provider && provider->ensureLoaded(readerFontConfig)) {
      renderer.setReaderFontMetricsProvider(provider);
      return provider->fontId();
    }
    LOG_ERR("TRS", "TTF reader metrics unavailable; using built-in font layout for this pass");
  }
#endif
  return effectiveRenderFontId();
}

bool TxtReaderActivity::ensurePageCacheAhead(const int pagesAhead, const bool showProgress) {
  if (endOfFileKnown) {
    updateEstimatedTotalPages();
    return true;
  }

  const size_t fileSize = txt->getFileSize();
  if (pageOffsets.empty()) {
    if (fileSize == 0) {
      endOfFileKnown = true;
      updateEstimatedTotalPages();
      return true;
    }
    pageOffsets.push_back(0);
  }

  const size_t targetKnownPages = static_cast<size_t>(std::max(0, currentPage) + pagesAhead + 1);
  const size_t startKnownPages = pageOffsets.size();
  const size_t pagesToGenerate = targetKnownPages > startKnownPages ? targetKnownPages - startKnownPages : 0;
  const unsigned long indexStartMs = millis();

  Rect popupRect{};
  int lastProgress = -1;
  if (showProgress && pagesToGenerate > 0) {
    popupRect = GUI.drawPopup(renderer, tr(STR_INDEXING));
    GUI.fillPopupProgress(renderer, popupRect, 0);
  }

  while (!endOfFileKnown && pageOffsets.size() < targetKnownPages) {
    const size_t offset = pageOffsets.back();
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(offset, tempLines, nextOffset) || nextOffset <= offset) {
      endOfFileKnown = true;
      break;
    }

    if (nextOffset < fileSize) {
      pageOffsets.push_back(nextOffset);
    } else {
      endOfFileKnown = true;
    }

    if (showProgress && pagesToGenerate > 0) {
      const size_t generated = pageOffsets.size() > startKnownPages ? pageOffsets.size() - startKnownPages : 0;
      const int progress = clampPercent(static_cast<int>((static_cast<float>(generated) * 100.0f) / pagesToGenerate));
      if (progress != lastProgress) {
        GUI.fillPopupProgress(renderer, popupRect, progress);
        lastProgress = progress;
      }
    }

    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  updateEstimatedTotalPages();
  const size_t generatedPages = pageOffsets.size() > startKnownPages ? pageOffsets.size() - startKnownPages : 0;
  LOG_INF("TRS",
          "Indexing session complete mode=%s font_id=%d generated=%u known=%u total=%d eof=%d duration_ms=%lu",
          readerFontConfig.isTtf() ? "ttf" : "builtin", cachedFontId, static_cast<unsigned>(generatedPages),
          static_cast<unsigned>(pageOffsets.size()), totalPages, endOfFileKnown ? 1 : 0, millis() - indexStartMs);
  if (showProgress && pagesToGenerate > 0) {
    GUI.fillPopupProgress(renderer, popupRect, 100);
  }
  return pageOffsets.size() > static_cast<size_t>(currentPage + 1) || endOfFileKnown;
}

void TxtReaderActivity::updateEstimatedTotalPages() {
  if (endOfFileKnown) {
    totalPages = std::max<int>(1, static_cast<int>(pageOffsets.size()));
    return;
  }

  if (pageOffsets.size() < 2 || pageOffsets.back() == 0) {
    totalPages = std::max<int>(1, static_cast<int>(pageOffsets.size()));
    return;
  }

  const float bytesPerPage = static_cast<float>(pageOffsets.back()) / static_cast<float>(pageOffsets.size() - 1);
  const int estimated = bytesPerPage > 0
                            ? static_cast<int>(static_cast<float>(txt->getFileSize()) / bytesPerPage + 0.5f)
                            : static_cast<int>(pageOffsets.size());
  totalPages = std::max<int>(static_cast<int>(pageOffsets.size()), estimated);
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  // Prime reader font advance data with this chunk's codepoints.
  // Without this, every getTextAdvanceX() call in the wrap loop below triggers
  // on-demand glyph loads through the 8-slot overflow ring buffer, which
  // thrashes for any text with more than 8 unique chars (i.e. all English),
  // floods the heap with short-lived bitmap allocations, and eventually
  // corrupts FreeRTOS state. The advance table persists across calls per
  // font, so the cost amortizes to ~ASCII-size after the first chunk.
  if (renderer.isSdCardFont(cachedFontId)) {
    renderer.ensureReaderFontReady(cachedFontId, reinterpret_cast<const char*>(buffer), /*styleMask=*/0x01);
  }

  // Parse lines from buffer
  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Calculate the actual length of line content in the buffer (excluding newline)
    size_t lineContentLen = lineEnd - pos;

    // Check for carriage return
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    // Extract line content for display (without CR/LF)
    std::string line(reinterpret_cast<char*>(buffer + pos), displayLen);
    if (renderer.isSdCardFont(cachedFontId) && !line.empty()) {
      renderer.ensureReaderFontReady(cachedFontId, line.c_str(), 0x01);
    }

    // Track position within this source line (in bytes from pos)
    size_t lineBytePos = 0;

    // Emit at least one visual line for each source line (including blank lines),
    // then continue with wrapping when needed.
    do {
      if (line.empty()) {
        outLines.emplace_back();
        break;
      }

      int lineWidth = measureTxtLineForIndexing(renderer, cachedFontId, line);

      if (lineWidth <= viewportWidth) {
        outLines.push_back(line);
        lineBytePos = displayLen;  // Consumed entire display content
        line.clear();
        break;
      }

      // Find break point
      size_t breakPos = line.length();
      while (breakPos > 0 &&
             measureTxtLineForIndexing(renderer, cachedFontId, line.substr(0, breakPos)) > viewportWidth) {
        // Try to break at space
        size_t spacePos = line.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        } else {
          // Break at character boundary for UTF-8
          breakPos--;
          // Make sure we don't break in the middle of a UTF-8 sequence
          while (breakPos > 0 && (line[breakPos] & 0xC0) == 0x80) {
            breakPos--;
          }
        }
      }

      if (breakPos == 0) {
        breakPos = firstUtf8CodepointLength(line);
      }

      outLines.push_back(line.substr(0, breakPos));

      // Skip space at break point
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
      line = line.substr(skipChars);
    } while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage);

    // Determine how much of the source buffer we consumed
    if (line.empty()) {
      // Fully consumed this source line, move past the newline
      pos = lineEnd + 1;
    } else {
      // Partially consumed - page is full mid-line
      // Move pos to where we stopped in the line (NOT past the line)
      pos = pos + lineBytePos;
      break;
    }
  }

  // Ensure we make progress even if calculations go wrong
  if (pos == 0 && !outLines.empty()) {
    // Fallback: at minimum, consume something to avoid infinite loop
    pos = 1;
  }

  nextOffset = offset + pos;

  // Make sure we don't go past the file
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  free(buffer);

  return !outLines.empty();
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= static_cast<int>(pageOffsets.size())) {
    currentPage = static_cast<int>(pageOffsets.size()) - 1;
  }

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  loadPageAtOffset(offset, currentPageLines, nextOffset);

  renderer.clearScreen();
  renderPage();

  // Save progress
  saveProgress();
}

void TxtReaderActivity::renderPage() {
  const int lineHeight = renderer.getLineHeight(cachedFontId);
  const int contentWidth = viewportWidth;

  if (renderer.isSdCardFont(cachedRenderFontId)) {
    std::string pageText;
    for (const auto& line : currentPageLines) {
      pageText += line;
      pageText += '\n';
    }
    renderer.ensureReaderFontReady(cachedRenderFontId, pageText.c_str(), 0x01);
  }

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (!line.empty()) {
        int x = cachedOrientedMarginLeft;
        const bool lineIsRtl = BidiUtils::startsWithRtl(line.c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH);
        uint8_t effectiveAlignment = cachedParagraphAlignment;
        if (lineIsRtl && (effectiveAlignment == CrossPointSettings::LEFT_ALIGN ||
                          effectiveAlignment == CrossPointSettings::JUSTIFIED)) {
          effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
        }
        const int textWidth = measureTxtLineForIndexing(renderer, cachedFontId, line);

        // Apply text alignment
        switch (effectiveAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            x = cachedOrientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // For plain text, justified is treated as left-aligned
            // (true justification would require word spacing adjustments)
            break;
        }

        renderer.drawText(cachedRenderFontId, x, y, line.c_str());
      }
      y += lineHeight;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();  // scan pass — text accumulated, no drawing
  scope.endScanAndPrewarm();

  // BW rendering
  renderLines();
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  bool renderTextAntiAliasing = SETTINGS.textAntiAliasing;
  if (renderTextAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  }
  // scope destructor clears font cache via FontCacheManager
}

void TxtReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
}

void TxtReaderActivity::saveProgress() const {
  HalFile f;
  if (Storage.openFileForWrite("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = 0;
    data[3] = 0;
    f.write(data, 4);
  }
}

void TxtReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("TRS", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - ReaderFontIdentity fields (explicit provider/font identity)
  // - int32_t: screen margin (to invalidate cache on margin change)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
  // - uint8_t: whether EOF has been reached by the growing cache
  // - uint32_t: known page offsets count
  // - N * uint32_t: page offsets

  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  const ReaderFontIdentity fontIdentity = readReaderFontIdentity(f);
  if (fontIdentity != readerFontConfig.identity) {
    LOG_DBG("TRS", "Cache reader font identity mismatch, rebuilding");
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint8_t eofKnownByte = 0;
  serialization::readPod(f, eofKnownByte);
  endOfFileKnown = eofKnownByte != 0;

  uint32_t numPages;
  serialization::readPod(f, numPages);

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  updateEstimatedTotalPages();
  LOG_DBG("TRS", "Loaded page index cache: known=%u total=%d eof=%d", static_cast<unsigned>(pageOffsets.size()),
          totalPages, endOfFileKnown ? 1 : 0);
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  // Write header using serialization module
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  writeReaderFontIdentity(f, readerFontConfig.identity);
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, static_cast<uint8_t>(endOfFileKnown ? 1 : 0));
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  LOG_DBG("TRS", "Saved page index cache: known=%u total=%d eof=%d", static_cast<unsigned>(pageOffsets.size()),
          totalPages, endOfFileKnown ? 1 : 0);
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = totalPages;
  info.progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
