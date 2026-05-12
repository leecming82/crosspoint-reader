#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/ParsedText.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JapaneseDictionary.h>
#include <Logging.h>
#include <Utf8.h>
#include <esp_system.h>

#include <iterator>
#include <limits>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"
#include "util/StringUtils.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t KANJI_LOOKUP_CONTEXT_CHARS = 12;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

std::string popupPronunciationLine(const JapaneseDictionaryMatch& match) {
  if (match.reading.empty() || match.reading == match.term) return "";
  return match.reading;
}

bool hasVisibleDefinitionText(const std::string& text) {
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  while (*p != '\0') {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp != ' ' && cp != '-' && cp != '(' && cp != ')' && cp != '[' && cp != ']' && cp != '/' && cp != '\\' &&
        cp != ',' && cp != ';' && cp != ':' && cp != 0x2022 && cp != 0x3000 && cp != 0xFF08 && cp != 0xFF09 &&
        cp != 0x3014 && cp != 0x3015 && cp != 0x3010 && cp != 0x3011 && cp != 0x3008 && cp != 0x3009 && cp != 0x300C &&
        cp != 0x300D && cp != 0x300E && cp != 0x300F && cp != 0xFF3B && cp != 0xFF3D) {
      return true;
    }
  }
  return false;
}

std::string definitionAttributeLabel(const std::string& item) {
  static constexpr const char* attrs[] = {
      "1-dan",     "5-dan",     "adj-i",    "adj-na",    "adj-no",     "adjective",    "adv",         "adverb",
      "archaism",  "aux-adj",   "aux-v",    "auxiliary", "colloquial", "counter",      "exp",         "expression",
      "godan",     "honorific", "humble",   "i-adj",     "ichidan",    "intransitive", "irregular",   "kana",
      "na-adj",    "no-adj",    "noun",     "numeric",   "obsolete",   "particle",     "prefix",      "pronoun",
      "rare",      "sensitive", "suffix",   "suru verb", "transitive", "usually kana", "vulgar",      "Wikipedia",
      "astronomy", "Buddhism",  "business", "computing", "food",       "linguistics",  "mathematics", "medicine",
      "military",  "music",     "physics",  "sports",
  };
  for (const char* attr : attrs) {
    if (item == attr) return item;
  }
  return "";
}

bool isDefinitionAttribute(const std::string& item) { return !definitionAttributeLabel(item).empty(); }

void appendDefinitionAttribute(std::string& attributes, const std::string& item) {
  const std::string label = definitionAttributeLabel(item);
  if (label.empty()) return;
  size_t start = 0;
  while (start < attributes.size()) {
    const size_t end = attributes.find(", ", start);
    const std::string existing = attributes.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (existing == label) return;
    if (end == std::string::npos) break;
    start = end + 2;
  }
  if (!attributes.empty()) attributes += ", ";
  attributes += label;
}

bool isNoiseDefinitionItem(const std::string& item) {
  return item.empty() || !hasVisibleDefinitionText(item) || item == "[" || item == "]" || item == "〔" ||
         item == "〕" || item == "()" || item == "[]" || item == "（）";
}

void drawRotatedLine(const GfxRenderer& renderer, const int fontId, const int x, const int y, const std::string& text,
                     const int maxWidth, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!hasVisibleDefinitionText(text)) return;
  const std::string fitted = renderer.truncatedText(fontId, text.c_str(), maxWidth, style);
  if (!hasVisibleDefinitionText(fitted)) return;
  renderer.drawTextRotated90CW(fontId, x, y, fitted.c_str(), true, style);
}

int drawRotatedWrappedLines(const GfxRenderer& renderer, const int fontId, const int x, const int y,
                            const std::string& text, const int maxWidth, const int maxLines, const int lineStep) {
  const auto lines = renderer.wrappedText(fontId, text.c_str(), maxWidth, maxLines);
  int drawX = x;
  for (const auto& line : lines) {
    if (!hasVisibleDefinitionText(line)) continue;
    renderer.drawTextRotated90CW(fontId, drawX, y, line.c_str(), true);
    drawX += lineStep;
  }
  return drawX;
}

void drawJapanesePopupLine(const GfxRenderer& renderer, const int fontId, const int x, const int y,
                           const std::string& text, const int maxWidth) {
  if (!hasVisibleDefinitionText(text)) return;
  constexpr int physicalLeftPadding = 36;
  constexpr int glyphGap = 2;
  const int effectiveMaxWidth = maxWidth - physicalLeftPadding;
  if (effectiveMaxWidth <= 0) return;
  const std::string fitted = renderer.truncatedText(fontId, text.c_str(), effectiveMaxWidth);
  if (!hasVisibleDefinitionText(fitted)) return;

  int drawY = y - physicalLeftPadding;
  const int minY = y - maxWidth;
  const auto* p = reinterpret_cast<const unsigned char*>(fitted.c_str());
  while (*p != '\0' && drawY > minY) {
    const auto* cpStart = p;
    utf8NextCodepoint(&p);
    const std::string glyph(reinterpret_cast<const char*>(cpStart), p - cpStart);
    renderer.drawText(fontId, x, drawY, glyph.c_str(), true);
    drawY -= renderer.getTextAdvanceX(fontId, glyph.c_str(), EpdFontFamily::REGULAR) + glyphGap;
  }
}

struct PopupDefinition {
  std::string attributes;
  std::vector<std::string> glosses;
};

PopupDefinition popupDefinitionItems(const std::string& definition, const size_t maxGlosses) {
  PopupDefinition parsed;
  std::vector<std::string> items;
  size_t start = 0;
  while (start < definition.size()) {
    const size_t end = definition.find("; ", start);
    std::string item = definition.substr(start, end == std::string::npos ? std::string::npos : end - start);
    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
    while (!item.empty() && item.back() == ' ') item.pop_back();
    if (!isNoiseDefinitionItem(item)) items.push_back(item);
    if (end == std::string::npos) break;
    start = end + 2;
  }

  size_t pos = 0;
  while (pos < items.size() && isDefinitionAttribute(items[pos])) {
    appendDefinitionAttribute(parsed.attributes, items[pos++]);
  }
  for (; pos < items.size() && parsed.glosses.size() < maxGlosses; ++pos) {
    if (isDefinitionAttribute(items[pos])) {
      appendDefinitionAttribute(parsed.attributes, items[pos]);
      continue;
    }
    parsed.glosses.push_back("• " + items[pos]);
  }
  return parsed;
}

bool isOpenRubyParen(const uint32_t cp) {
  return cp == '(' || cp == 0xFF08 || cp == 0x3014 || cp == 0x3010 || cp == 0x3008;
}

bool isCloseRubyParen(const uint32_t cp) {
  return cp == ')' || cp == 0xFF09 || cp == 0x3015 || cp == 0x3011 || cp == 0x3009;
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  FsFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        // UINT16_MAX is an in-memory navigation sentinel for "open previous
        // chapter on its last page". It should never be treated as persisted
        // resume state after sleep or reopen.
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearPersistentCache();
  }

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  epub.reset();
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  // Kanji cursor mode: active only in tategaki (LandscapeCounterClockwise) orientation.
  if (kanjiCursorActive) {
    if (RenderLock::peek()) {
      return;
    }
    // Popup mode: Back dismisses it, Left/Right cycle through ranked dictionary matches.
    if (kanjiPopupActive) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        hideKanjiPopup();
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
        moveKanjiPopupMatch(-1);
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        moveKanjiPopupMatch(+1);
        return;
      }
      return;  // Swallow all other input while popup is active.
    }
    // Back exits cursor mode without leaving the reader. Long Back is disabled here
    // so slow popup/cursor redraws cannot accidentally navigate away.
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      exitKanjiCursorMode();
      return;
    }
    // Confirm opens the lookup popup for the selected kanji.
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      showKanjiPopup();
      return;
    }
    // Left/Right buttons move within the current tategaki column; side buttons jump columns.
    // Use wasPressed only (leading edge) to avoid double-fires across render windows.
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      moveKanjiCursor(-1);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      moveKanjiCursor(+1);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
      moveKanjiCursorToLine(-1);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
      moveKanjiCursorToLine(+1);
      return;
    }
    return;  // Swallow all other input while cursor is active.
  }

  // Long-press Confirm (600ms) in tategaki orientation enters cursor mode.
  if (SETTINGS.orientation == CrossPointSettings::LANDSCAPE_CCW && section &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= CURSOR_ENTER_MS) {
    if (RenderLock::peek()) {
      return;
    }
    enterKanjiCursorMode();
    return;
  }

  // Enter reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int currentPage = section ? section->currentPage + 1 : 0;
    const int totalPages = section ? section->pageCount : 0;
    float bookProgress = 0.0f;
    if (epub->getBookSize() > 0 && section && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
    }
    const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
    const std::string menuTitle = StringUtils::uiSafeBookTitle(epub->getTitle(), epub->getPath());
    startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                               renderer, mappedInput, menuTitle, currentPage, totalPages, bookProgressPercent,
                               SETTINGS.orientation, !currentPageFootnotes.empty()),
                           [this](const ActivityResult& result) {
                             // Always apply orientation change even if the menu was cancelled
                             const auto& menu = std::get<MenuResult>(result.data);
                             applyOrientation(menu.orientation);
                             toggleAutoPageTurn(menu.pageTurnOption);
                             if (!result.isCancelled) {
                               onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                             }
                           });
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
      section.reset();
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && currentSpineIndex != std::get<ChapterResult>(result.data).spineIndex) {
              RenderLock lock(*this);
              currentSpineIndex = std::get<ChapterResult>(result.data).spineIndex;
              nextPageNumber = 0;
              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        auto p = section->loadPageFromSectionFile();
        if (p) {
          std::string fullText;
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
          if (!fullText.empty()) {
            startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                   [this](const ActivityResult& result) {});
            break;
          }
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        const int currentPage = section ? section->currentPage : nextPageNumber;
        const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
        std::optional<uint16_t> paragraphIndex;
        if (section && currentPage >= 0 && currentPage < section->pageCount) {
          const uint16_t paragraphPage =
              currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
          if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
            paragraphIndex = *pIdx;
          }
        }

        // Pre-compute local KO position and chapter name while Epub is still in RAM.
        CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
        if (paragraphIndex.has_value()) {
          localPos.paragraphIndex = *paragraphIndex;
          localPos.hasParagraphIndex = true;
        }
        KOReaderPosition localKoPos = ProgressMapper::toKOReader(epub, localPos);
        const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
        std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
        const std::string savedEpubPath = epub->getPath();

        // Persist current position so the reader resumes at the right page on return.
        // goToReader() depends on this file, so abort the sync if the write fails.
        if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
          LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
          pendingSyncSaveError = true;
          requestUpdate();
          return;
        }

        // Release Epub and Section to free ~65KB RAM for the TLS handshake.
        LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
        {
          RenderLock lock(*this);
          if (section) {
            nextPageNumber = section->currentPage;
          }
          section.reset();
          epub.reset();
        }
        LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

        activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
            renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
            std::move(localChapterName), paragraphIndex));
      }
      break;
    }
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  // Clear cursor state without triggering a redundant requestUpdate — pageTurn does its own.
  if (kanjiCursorActive) {
    kanjiCursorActive = false;
    kanjiPopupActive = false;
    kanjiPopupMatches.clear();
    kanjiPopupMatches.shrink_to_fit();
    kanjiPopupMatchIndex = 0;
    kanjiIndex.clear();
    kanjiIndex.shrink_to_fit();
    kanjiCursorPage.reset();
  }
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
      LOG_DBG("ERS", "Cache not found, building...");

      Rect popupRect = GUI.drawPopup(renderer, tr(STR_INDEXING));
      GUI.fillPopupProgress(renderer, popupRect, 0);

      const auto progressFn = [this, popupRect](const size_t bytesRead, const size_t fileSize) {
        if (fileSize == 0) {
          return;
        }
        const int progress =
            clampPercent(static_cast<int>((static_cast<float>(bytesRead) * 100.0f) / static_cast<float>(fileSize)));
        GUI.fillPopupProgress(renderer, popupRect, progress);
      };

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                      SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, progressFn)) {
        LOG_ERR("ERS", "Failed to persist page data to SD");
        section.reset();
        showPendingSyncSaveError();
        return;
      }
      if (auto* fcm = renderer.getFontCacheManager()) {
        fcm->clearPersistentCache();
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      if (*pendingPageJump >= section->pageCount && section->pageCount > 0) {
        section->currentPage = section->pageCount - 1;
      } else {
        section->currentPage = *pendingPageJump;
      }
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      } else if (section->currentPage >= section->pageCount && section->pageCount > 0) {
        LOG_DBG("ERS", "Clamping cached page %d to %d", section->currentPage, section->pageCount - 1);
        section->currentPage = section->pageCount - 1;
      }
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      section->clearCache();
      section.reset();
      requestUpdate();  // Try again after clearing cache
                        // TODO: prevent infinite loop if the page keeps failing to load for some reason
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  if (kanjiCursorActive && !kanjiPopupActive) {
    kanjiCursorRectValid = false;
    drawKanjiCursor();
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                     viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                     SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  // Force special handling for pages with images when anti-aliasing is on
  bool imagePageWithAA = page->hasImages() && SETTINGS.textAntiAliasing;

  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  const auto tBwRender = millis();

  if (imagePageWithAA) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // Double FAST_REFRESH handles ghosting for image pages; don't count toward full refresh cadence
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Save BW buffer to reset buffer/display state after grayscale data sync.
  // If indexing fragmented heap enough that this fails, skip AA for this page;
  // otherwise X3 grayscale can leave RED RAM unsynced and force a slow full sync
  // on the next page turn.
  const bool bwBufferStored = SETTINGS.textAntiAliasing && renderer.storeBwBuffer();
  const auto tBwStore = millis();

  // grayscale rendering
  // TODO: Only do this if font supports it
  if (SETTINGS.textAntiAliasing && bwBufferStored) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleLsbBuffers();
    const auto tGrayLsb = millis();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleMsbBuffers();
    const auto tGrayMsb = millis();

    // display grayscale part
    renderer.displayGrayBuffer();
    const auto tGrayDisplay = millis();
    renderer.setRenderMode(GfxRenderer::BW);
    // restore the bw data
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
            "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
            tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
  } else {
    if (SETTINGS.textAntiAliasing) {
      LOG_ERR("ERS", "Skipping text AA: failed to store BW buffer (free=%u max=%u)",
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    }
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tBwRestore - tBwStore,
            tEnd - t0);
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = std::string("Pages 1-") + std::to_string(static_cast<int>(pageCount));
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = StringUtils::uiSafeLabelOrFallback(tocItem.title, title);
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = StringUtils::uiSafeBookTitle(epub->getTitle(), epub->getPath());
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

// --- Kanji cursor overlay (Phase 1: tategaki dictionary lookup) ---

void EpubReaderActivity::enterKanjiCursorMode() {
  if (!section) return;
  LOG_DBG("DICT", "Enter cursor requested spine=%d page=%d", currentSpineIndex, section->currentPage);

  // Compute the same margins used by renderContents so cursor coords align.
  int dummy1, dummy2;
  renderer.getOrientedViewableTRBL(&kanjiMarginTop, &dummy1, &dummy2, &kanjiMarginLeft);
  kanjiMarginTop += SETTINGS.screenMargin;
  kanjiMarginLeft += SETTINGS.screenMargin;

  kanjiCursorPage = section->loadPageFromSectionFile();
  if (!kanjiCursorPage) {
    LOG_ERR("CURSOR", "Failed to load page for cursor mode");
    return;
  }

  // Build a flat list of text positions where dictionary terms can start.
  kanjiIndex.clear();
  kanjiIndex.reserve(128);
  const auto& elements = kanjiCursorPage->elements;
  for (int16_t ei = 0; ei < static_cast<int16_t>(elements.size()); ++ei) {
    if (elements[ei]->getTag() != TAG_PageLine) continue;
    const auto& block = *static_cast<const PageLine&>(*elements[ei]).getBlock();
    const auto& words = block.getWords();
    for (int16_t wi = 0; wi < static_cast<int16_t>(words.size()); ++wi) {
      if (words[wi].empty()) continue;
      const auto* wordStart = reinterpret_cast<const unsigned char*>(words[wi].c_str());
      const auto* p = wordStart;
      while (*p != '\0') {
        const auto* cpStart = p;
        if (utf8IsJapaneseDictionaryStart(utf8NextCodepoint(&p))) {
          kanjiIndex.push_back({ei, wi, static_cast<uint16_t>(cpStart - wordStart)});
        }
      }
    }
  }

  if (kanjiIndex.empty()) {
    LOG_DBG("DICT", "No Japanese dictionary start chars spine=%d page=%d", currentSpineIndex, section->currentPage);
    kanjiCursorPage.reset();
    return;
  }

  const bool resumed = kanjiResumeValid && kanjiResumeSpineIndex == currentSpineIndex &&
                       kanjiResumePageNumber == section->currentPage && kanjiResumeIndexPos >= 0 &&
                       kanjiResumeIndexPos < static_cast<int>(kanjiIndex.size());
  if (kanjiResumeValid && kanjiResumeSpineIndex == currentSpineIndex && kanjiResumePageNumber == section->currentPage &&
      kanjiResumeIndexPos >= 0 && kanjiResumeIndexPos < static_cast<int>(kanjiIndex.size())) {
    kanjiIndexPos = kanjiResumeIndexPos;
  } else {
    kanjiIndexPos = 0;
  }
  kanjiCursorActive = true;
  LOG_DBG("DICT", "Enter cursor ok entries=%d pos=%d resumed=%d", static_cast<int>(kanjiIndex.size()), kanjiIndexPos,
          resumed ? 1 : 0);

  drawKanjiCursor();
}

void EpubReaderActivity::exitKanjiCursorMode() {
  if (section && kanjiCursorActive && !kanjiIndex.empty() && kanjiIndexPos >= 0 &&
      kanjiIndexPos < static_cast<int>(kanjiIndex.size())) {
    kanjiResumeValid = true;
    kanjiResumeSpineIndex = currentSpineIndex;
    kanjiResumePageNumber = section->currentPage;
    kanjiResumeIndexPos = kanjiIndexPos;
    LOG_DBG("DICT", "Exit cursor saved spine=%d page=%d pos=%d entries=%d", kanjiResumeSpineIndex,
            kanjiResumePageNumber, kanjiResumeIndexPos, static_cast<int>(kanjiIndex.size()));
  } else {
    LOG_DBG("DICT", "Exit cursor no-save active=%d entries=%d", kanjiCursorActive ? 1 : 0,
            static_cast<int>(kanjiIndex.size()));
  }
  kanjiCursorActive = false;
  kanjiPopupActive = false;
  kanjiPopupMatches.clear();
  kanjiPopupMatches.shrink_to_fit();
  kanjiPopupMatchIndex = 0;
  kanjiCursorRectValid = false;
  kanjiIndex.clear();
  kanjiIndex.shrink_to_fit();
  kanjiCursorPage.reset();
  kanjiDictionary.close();
  requestUpdate();
}

void EpubReaderActivity::moveKanjiCursor(const int direction) {
  if (!kanjiCursorActive || kanjiIndex.empty()) return;
  const int next = kanjiIndexPos + direction;
  if (next < 0 || next >= static_cast<int>(kanjiIndex.size())) return;
  kanjiIndexPos = next;
  LOG_DBG("DICT", "Move cursor dir=%d pos=%d/%d", direction, kanjiIndexPos + 1, static_cast<int>(kanjiIndex.size()));
  drawKanjiCursor();
}

void EpubReaderActivity::moveKanjiCursorToLine(const int direction) {
  if (!kanjiCursorActive || kanjiIndex.empty() || !kanjiCursorPage) return;
  const auto& elements = kanjiCursorPage->elements;

  const auto& cur = kanjiIndex[kanjiIndexPos];
  if (cur.elementIdx >= static_cast<int16_t>(elements.size())) return;
  if (elements[cur.elementIdx]->getTag() != TAG_PageLine) return;
  const int curYPos = static_cast<const PageLine&>(*elements[cur.elementIdx]).yPos;

  // Scan in direction for the first kanji whose PageLine has a different yPos (= different column).
  int pos = kanjiIndexPos + direction;
  while (pos >= 0 && pos < static_cast<int>(kanjiIndex.size())) {
    const auto& e = kanjiIndex[pos];
    if (e.elementIdx < static_cast<int16_t>(elements.size()) && elements[e.elementIdx]->getTag() == TAG_PageLine) {
      const int yPos = static_cast<const PageLine&>(*elements[e.elementIdx]).yPos;
      if (yPos != curYPos) {
        kanjiIndexPos = pos;
        LOG_DBG("DICT", "Jump cursor dir=%d pos=%d/%d", direction, kanjiIndexPos + 1,
                static_cast<int>(kanjiIndex.size()));
        drawKanjiCursor();
        return;
      }
    }
    pos += direction;
  }

  kanjiIndexPos = direction > 0 ? 0 : static_cast<int>(kanjiIndex.size()) - 1;
  LOG_DBG("DICT", "Wrap cursor dir=%d pos=%d/%d", direction, kanjiIndexPos + 1, static_cast<int>(kanjiIndex.size()));
  drawKanjiCursor();
}

void EpubReaderActivity::drawKanjiCursor() {
  if (!kanjiCursorActive || !kanjiCursorPage || kanjiIndex.empty()) return;
  if (kanjiIndexPos < 0 || kanjiIndexPos >= static_cast<int>(kanjiIndex.size())) return;

  const auto& entry = kanjiIndex[kanjiIndexPos];
  const auto& elements = kanjiCursorPage->elements;
  if (entry.elementIdx >= static_cast<int16_t>(elements.size())) return;
  if (elements[entry.elementIdx]->getTag() != TAG_PageLine) return;

  const auto& pl = static_cast<const PageLine&>(*elements[entry.elementIdx]);
  const auto& tb = *pl.getBlock();
  const auto& xposVec = tb.getWordXpos();
  const auto& words = tb.getWords();
  const auto& styles = tb.getWordStyles();
  if (entry.wordIdx >= static_cast<int16_t>(xposVec.size())) return;
  if (entry.wordIdx >= static_cast<int16_t>(words.size()) || entry.wordIdx >= static_cast<int16_t>(styles.size()))
    return;

  int intraWordX = 0;
  if (entry.byteOffset > 0 && entry.byteOffset <= words[entry.wordIdx].size()) {
    const std::string prefix = words[entry.wordIdx].substr(0, entry.byteOffset);
    intraWordX = renderer.getTextAdvanceX(SETTINGS.getReaderFontId(), prefix.c_str(), styles[entry.wordIdx]);
  }

  const int cx = kanjiMarginLeft + pl.xPos + xposVec[entry.wordIdx] + intraWordX;
  const int cy = kanjiMarginTop + pl.yPos;
  const int cellSize = renderer.getLineHeight(SETTINGS.getReaderFontId());

  if (kanjiCursorRectValid) {
    renderer.drawRect(kanjiCursorRectX, kanjiCursorRectY, kanjiCursorRectSize, kanjiCursorRectSize, 2, false);
  }

  renderer.drawRect(cx, cy, cellSize, cellSize, 2, true);
  kanjiCursorRectValid = true;
  kanjiCursorRectX = cx;
  kanjiCursorRectY = cy;
  kanjiCursorRectSize = cellSize;
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

std::string EpubReaderActivity::extractKanjiLookupText(const size_t maxChars) const {
  if (!kanjiCursorPage || kanjiIndex.empty() || kanjiIndexPos < 0 ||
      kanjiIndexPos >= static_cast<int>(kanjiIndex.size()) || maxChars == 0) {
    return "";
  }

  const auto& selected = kanjiIndex[kanjiIndexPos];
  std::string result;
  size_t charCount = 0;
  bool started = false;
  int parenDepth = 0;

  const auto& elements = kanjiCursorPage->elements;
  for (int16_t ei = 0; ei < static_cast<int16_t>(elements.size()) && charCount < maxChars; ++ei) {
    if (elements[ei]->getTag() != TAG_PageLine) continue;
    if (!started && ei < selected.elementIdx) continue;

    const auto& block = *static_cast<const PageLine&>(*elements[ei]).getBlock();
    const auto& words = block.getWords();
    for (int16_t wi = 0; wi < static_cast<int16_t>(words.size()) && charCount < maxChars; ++wi) {
      if (!started) {
        if (ei != selected.elementIdx || wi != selected.wordIdx) continue;
        started = true;
      }

      const size_t startOffset = (ei == selected.elementIdx && wi == selected.wordIdx) ? selected.byteOffset : 0;
      if (startOffset >= words[wi].size()) continue;

      const auto* wordStart = reinterpret_cast<const unsigned char*>(words[wi].c_str());
      const auto* p = wordStart + startOffset;
      while (*p != '\0' && charCount < maxChars) {
        const auto* cpStart = p;
        const uint32_t cp = utf8NextCodepoint(&p);
        if (isOpenRubyParen(cp)) {
          ++parenDepth;
          continue;
        }
        if (isCloseRubyParen(cp)) {
          if (parenDepth > 0) --parenDepth;
          continue;
        }
        if (parenDepth > 0) continue;
        result.append(reinterpret_cast<const char*>(cpStart), p - cpStart);
        ++charCount;
      }
    }
  }

  return result;
}

void EpubReaderActivity::showKanjiPopup() {
  if (!kanjiCursorActive || kanjiIndex.empty()) return;
  kanjiPopupActive = true;

  // Extract the selected kanji and the forward context that dictionary lookup will receive.
  const std::string lookupText = extractKanjiLookupText(KANJI_LOOKUP_CONTEXT_CHARS);
  LOG_DBG("DICT", "Lookup pos=%d/%d text=%s", kanjiIndexPos + 1, static_cast<int>(kanjiIndex.size()),
          lookupText.c_str());
  kanjiPopupMatches.clear();
  kanjiPopupMatchIndex = 0;
  kanjiDictionary.lookupContext(lookupText, kanjiPopupMatches, 8, KANJI_LOOKUP_CONTEXT_CHARS);
  LOG_DBG("DICT", "Lookup result matches=%d dict=%s", static_cast<int>(kanjiPopupMatches.size()),
          kanjiDictionary.getBasePath().c_str());

  drawKanjiPopup();
}

void EpubReaderActivity::drawKanjiPopup() {
  const bool hasMatch = !kanjiPopupMatches.empty();

  // In landscape CCW orientation the device is held in portrait.
  // Renderer X-axis = user's top-to-bottom; renderer Y-axis = user's right-to-left.
  // A box that is wide in renderer-X and narrow in renderer-Y becomes a portrait
  // box after the display's CCW rotation.
  // Text drawn with drawTextRotated90CW rotates 90° CW in renderer space; the
  // display's CCW transform cancels it, so text appears upright to the user.
  const int sw = renderer.getScreenWidth();   // 800 in landscape CCW
  const int sh = renderer.getScreenHeight();  // 480 in landscape CCW
  const int pad = 18;

  // Box: rdx = user portrait height (renderer X extent), rdy = user portrait width (renderer Y extent).
  const int rdx = sw * 17 / 20;   // 85% of screen height after CCW display rotation
  const int rdy = sh * 17 / 20;   // 85% of screen width after CCW display rotation
  const int rx = (sw - rdx) / 2;  // center in renderer X
  const int ry = (sh - rdy) / 2;  // center in renderer Y

  renderer.fillRect(rx, ry, rdx, rdy, false);    // white background
  renderer.drawRect(rx, ry, rdx, rdy, 2, true);  // black border

  // All text lines start from the user's left edge (= high renderer Y) and run right.
  const int textY = ry + rdy - pad;
  const int maxTextWidth = rdy - pad * 2;
  const int termLineStep = renderer.getLineHeight(SETTINGS.getReaderFontId()) + 12;
  const int uiLineStep = renderer.getLineHeight(UI_10_FONT_ID) + 8;

  int lineX = rx + pad;
  if (hasMatch) {
    if (kanjiPopupMatchIndex >= kanjiPopupMatches.size()) kanjiPopupMatchIndex = 0;
    const auto& match = kanjiPopupMatches[kanjiPopupMatchIndex];
    drawJapanesePopupLine(renderer, SETTINGS.getReaderFontId(), lineX, textY, match.term, maxTextWidth);
    lineX += termLineStep;
    const std::string pronunciation = popupPronunciationLine(match);
    if (!pronunciation.empty()) {
      drawJapanesePopupLine(renderer, SETTINGS.getReaderFontId(), lineX, textY, pronunciation, maxTextWidth);
      lineX += termLineStep;
    }
    if (match.sourceText != match.term) {
      drawRotatedLine(renderer, UI_10_FONT_ID, lineX, textY, "from " + match.sourceText, maxTextWidth);
      lineX += uiLineStep;
    }
    const PopupDefinition definition = popupDefinitionItems(match.definition, 6);
    if (hasVisibleDefinitionText(definition.attributes)) {
      drawRotatedLine(renderer, UI_10_FONT_ID, lineX, textY, "(" + definition.attributes + ")", maxTextWidth,
                      EpdFontFamily::BOLD);
      lineX += uiLineStep;
    }
    for (const auto& item : definition.glosses) {
      if (lineX >= rx + rdx - pad - uiLineStep * 3) break;
      lineX = drawRotatedWrappedLines(renderer, UI_10_FONT_ID, lineX, textY, item, maxTextWidth, 2, uiLineStep);
    }
    if (kanjiPopupMatches.size() > 1) {
      const std::string position =
          std::to_string(kanjiPopupMatchIndex + 1) + "/" + std::to_string(kanjiPopupMatches.size());
      renderer.drawTextRotated90CW(UI_10_FONT_ID, rx + rdx - pad - 18, ry + pad + maxTextWidth, position.c_str(), true);
    }
  } else {
    drawRotatedLine(renderer, UI_10_FONT_ID, lineX, textY, "No dictionary match", maxTextWidth);
  }

  const bool hasMultipleMatches = kanjiPopupMatches.size() > 1;
  const auto labels =
      mappedInput.mapLabels(tr(STR_EXIT), "", hasMultipleMatches ? "Prev" : "", hasMultipleMatches ? "Next" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void EpubReaderActivity::moveKanjiPopupMatch(const int direction) {
  if (!kanjiPopupActive || kanjiPopupMatches.size() <= 1 || direction == 0) return;
  const int count = static_cast<int>(kanjiPopupMatches.size());
  int next = static_cast<int>(kanjiPopupMatchIndex) + direction;
  if (next < 0) {
    next = count - 1;
  } else if (next >= count) {
    next = 0;
  }
  kanjiPopupMatchIndex = static_cast<size_t>(next);
  LOG_DBG("DICT", "Popup match pos=%d/%d", next + 1, count);
  drawKanjiPopup();
}

void EpubReaderActivity::hideKanjiPopup() {
  LOG_DBG("DICT", "Hide popup matches=%d", static_cast<int>(kanjiPopupMatches.size()));
  kanjiPopupActive = false;
  kanjiPopupMatches.clear();
  kanjiPopupMatchIndex = 0;
  kanjiCursorRectValid = false;
  requestUpdate();  // repaint the page under the popup, then restore the cursor overlay
}

// --- End kanji cursor overlay ---

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->pageCount;
    if (epub && epub->getBookSize() > 0 && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}
