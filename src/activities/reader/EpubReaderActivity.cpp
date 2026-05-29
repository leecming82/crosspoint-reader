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
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <esp_system.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <iterator>
#include <limits>

#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderNavigation.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ScreenshotUtil.h"
#include "util/StringUtils.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t KANJI_LOOKUP_CONTEXT_CHARS = 12;
constexpr unsigned long PENDING_PAGE_TURN_INTENT_TTL_MS = 3000;

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
      "1-dan",         "1-dan (spec.)",
      "2-dan",         "4-dan",
      "5-dan",         "5-dan (irreg.)",
      "5-dan (spec.)", "Brazil",
      "Buddhism",      "Chinese myth.",
      "Christian",     "Greek myth.",
      "Hokkaidō",      "Japanese myth.",
      "Kansai",        "Kantō",
      "Kyōto",         "Kyūshū",
      "Nagano",        "Roman myth.",
      "Ryūkyū",        "Shintō",
      "Tosa",          "Tsugaru",
      "Tōhoku",        "abbr.",
      "adjectival",    "adjective",
      "adverb",        "agriculture",
      "anatomy",       "archaic",
      "archeology",    "architecture",
      "art",           "astronomy",
      "audiovisual",   "aux-adj",
      "aux-verb",      "auxiliary",
      "aviation",      "baseball",
      "biochemistry",  "biology",
      "botany",        "boxing",
      "business",      "card games",
      "chemistry",     "childish",
      "civil eng.",    "clothing",
      "colloquial",    "computing",
      "conjunction",   "copula",
      "counter",       "crystal",
      "dated",         "dentistry",
      "derogatory",    "ecology",
      "economics",     "electricity",
      "electronics",   "embryology",
      "engineering",   "entomology",
      "euphemism",     "exp",
      "familiar",      "feminine",
      "film",          "finance",
      "fishing",       "food",
      "formal",        "gardening",
      "genetics",      "geography",
      "geology",       "geometry",
      "go (game)",     "golf",
      "grammar",       "hanafuda",
      "historical",    "honorific",
      "horse racing",  "humble",
      "idiom",         "interjection",
      "internet",      "intransitive",
      "jocular",       "kabuki",
      "ku-adj",        "kuru",
      "law",           "legend",
      "linguistics",   "logic",
      "mahjong",       "manga",
      "manga slang",   "martial arts",
      "masculine",     "math",
      "mech. eng.",    "medical",
      "meteorology",   "military",
      "mimetic",       "mineralogy",
      "mining",        "motorsport",
      "music",         "na-adj",
      "nari",          "net slang",
      "no-adj",        "noh",
      "noun",          "nu-verb",
      "numeric",       "obsolete",
      "ornithology",   "paleontology",
      "particle",      "pathology",
      "person",        "pharmacology",
      "philosophy",    "photography",
      "physics",       "physiology",
      "place",         "poetical",
      "polite",        "politics",
      "prefix",        "printing",
      "pronoun",       "proverb",
      "psychiatry",    "psychoanalysis",
      "psychology",    "quote",
      "railway",       "rare",
      "religion",      "ri-verb",
      "sensitive",     "shiku",
      "shōgi",         "skating",
      "skiing",        "slang",
      "sports",        "statistics",
      "stock market",  "su-verb",
      "suffix",        "sumō",
      "surgery",       "suru verb",
      "taru",          "telecom",
      "television",    "to-adverb",
      "trademark",     "transitive",
      "unclass",       "usually kana",
      "verb",          "veterinary medicine",
      "video games",   "vulgar",
      "wasei",         "work",
      "wrestling",     "yoji",
      "zoology",       "zuru",
      "Ōsaka",
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
         item == "〕" || item == "()" || item == "[]" || item == "（）" || item == "forms";
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

// SD card folder finished books are moved into. Single source of truth for the path.
// constexpr ⇒ lives in flash .rodata, no DRAM cost.
constexpr char READ_FOLDER[] = "/read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // length of "/Read" (excludes NUL)
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
// On rename failure: LOG_ERR and leave everything in place (no UI alert subsystem here).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();
  clearLatchedPageTurnIntent();

  if (!epub) {
    return;
  }

  resolveReadingProfile();
  sdFontSystem.ensureLoaded(renderer, effectiveReaderFontSize());
  // Configure screen orientation based on the explicit reader orientation setting.
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  HalFile f;
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
  clearLatchedPageTurnIntent();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearPersistentCache();
  }

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
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

  // Japanese dictionary cursor mode.
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
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      moveKanjiCursor(+1);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
      moveKanjiCursorToLine(-1);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
      moveKanjiCursorToLine(+1);
    }
    flushKanjiCursorRefresh();
    return;  // Swallow all other input while cursor is active.
  }

  // Long-press Confirm (600ms) enters cursor mode for Japanese books in horizontal or vertical writing mode.
  if (isJapaneseLanguageBook() && section && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= CURSOR_ENTER_MS) {
    if (RenderLock::peek()) {
      return;
    }
    enterKanjiCursorMode();
    return;
  }

  if (rubyAdjustActive) {
    if (RenderLock::peek()) {
      return;
    }
    if (rubyAdjustIgnoreOpeningRelease) {
      if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
          !mappedInput.isPressed(MappedInputManager::Button::Back)) {
        rubyAdjustIgnoreOpeningRelease = false;
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      exitRubyAdjustMode();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      adjustRubyOffset(RubyAdjustAxis::X, -1);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      adjustRubyOffset(RubyAdjustAxis::X, +1);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      adjustRubyOffset(RubyAdjustAxis::Y, -1);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      adjustRubyOffset(RubyAdjustAxis::Y, +1);
      return;
    }
    return;
  }

  // Rendering may be loading from SD and driving the display over SPI. Do not
  // mutate reader state until the current render finishes.
  if (RenderLock::peek()) {
    latchPageTurnIntentWhileBusy();
    return;
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  // Enter reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    clearLatchedPageTurnIntent();
    const int currentPage = section ? section->currentPage + 1 : 0;
    const int totalPages = section ? section->pageCount : 0;
    float bookProgress = 0.0f;
    if (epub->getBookSize() > 0 && section && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
    }
    const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
    const std::string menuTitle = StringUtils::uiSafeBookTitle(epub->getTitle(), epub->getPath());
    startActivityForResult(
        std::make_unique<EpubReaderMenuActivity>(
            renderer, mappedInput, menuTitle, currentPage, totalPages, bookProgressPercent, SETTINGS.orientation,
            SETTINGS.writingModePreference, !currentPageFootnotes.empty(), allowsManualVerticalWritingMode()),
        [this](const ActivityResult& result) {
          // Apply in-menu preference changes even if the menu was cancelled.
          const auto& menu = std::get<MenuResult>(result.data);
          applyOrientation(menu.orientation);
          applyWritingModePreference(menu.writingModePreference);
          toggleAutoPageTurn(menu.pageTurnOption);
          if (!result.isCancelled) {
            onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
          }
        });
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    clearLatchedPageTurnIntent();
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      clearLatchedPageTurnIntent();
      restoreSavedPosition();
      return;
    }
    clearLatchedPageTurnIntent();
    onGoHome();
    return;
  }

  bool latchedForwardTurn = false;
  const bool latchedPageTurn = consumeLatchedPageTurnIntent(latchedForwardTurn);
  const auto [livePrevTriggered, liveNextTriggered, fromTilt] =
      latchedPageTurn ? ReaderUtils::PageTurnResult{false, false, false} : ReaderUtils::detectPageTurn(mappedInput);
  const bool prevTriggered = latchedPageTurn ? !latchedForwardTurn : livePrevTriggered;
  const bool nextTriggered = latchedPageTurn ? latchedForwardTurn : liveNextTriggered;
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

  const bool longPress = !latchedPageTurn && !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      clearKanjiCursorState(/*saveResumePosition=*/false, /*requestRedraw=*/false);
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
    clearKanjiCursorState(/*saveResumePosition=*/false, /*requestRedraw=*/false);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      const auto& sync = std::get<ProgressChangeResult>(result.data);
      if (currentSpineIndex != sync.spineIndex || (section && section->currentPage != sync.page)) {
        RenderLock lock(*this);
        currentSpineIndex = sync.spineIndex;
        nextPageNumber = sync.page;
        section.reset();
      }
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& chapterResult = std::get<ChapterResult>(result.data);
              RenderLock lock(*this);
              clearKanjiCursorState(/*saveResumePosition=*/false, /*requestRedraw=*/false);

              currentSpineIndex = chapterResult.spineIndex;

              // If anchor is not empty, it will be used later to calculate the page number.
              pendingAnchor = chapterResult.anchor;

              // Otherwise page 0 will be used.
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
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult& result) {});
          break;
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
          clearKanjiCursorState(/*saveResumePosition=*/false, /*requestRedraw=*/false);
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
        CrossPointPosition localPos = getCurrentPosition();
        SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
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
          clearKanjiCursorState(/*saveResumePosition=*/false, /*requestRedraw=*/false);
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
    case EpubReaderMenuActivity::MenuAction::ADD_BOOKMARK:
      addBookmark();
      bookmarkMessageTime = millis();
      requestUpdate();
      break;
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::RUBY_OFFSET:
      enterRubyAdjustMode();
      break;
    case EpubReaderMenuActivity::MenuAction::ROTATE_SCREEN:
    case EpubReaderMenuActivity::MenuAction::WRITING_MODE:
    case EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN:
      break;
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  if (SETTINGS.orientation == orientation) {
    return;
  }

  {
    RenderLock lock(*this);
    clearKanjiCursorState(/*saveResumePosition=*/false, /*requestRedraw=*/false);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    section.reset();
  }
}

void EpubReaderActivity::applyWritingModePreference(const uint8_t writingModePreference) {
  if (writingModePreference >= CrossPointSettings::WRITING_MODE_PREFERENCE_COUNT ||
      SETTINGS.writingModePreference == writingModePreference) {
    return;
  }

  {
    RenderLock lock(*this);
    clearKanjiCursorState(/*saveResumePosition=*/false, /*requestRedraw=*/false);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    SETTINGS.writingModePreference = writingModePreference;
    resolveReadingProfile();
    sdFontSystem.ensureLoaded(renderer, effectiveReaderFontSize());
    SETTINGS.saveToFile();

    section.reset();
  }
}

uint8_t EpubReaderActivity::currentRubyOffsetX() const {
  return effectiveWritingMode == EpubWritingMode::VerticalRl ? SETTINGS.tategakiRubyOffsetX
                                                             : SETTINGS.yokogakiRubyOffsetX;
}

uint8_t EpubReaderActivity::currentRubyOffsetY() const {
  return effectiveWritingMode == EpubWritingMode::VerticalRl ? SETTINGS.tategakiRubyOffsetY
                                                             : SETTINGS.yokogakiRubyOffsetY;
}

void EpubReaderActivity::setCurrentRubyOffsetX(const uint8_t value) {
  if (effectiveWritingMode == EpubWritingMode::VerticalRl) {
    SETTINGS.tategakiRubyOffsetX = value;
  } else {
    SETTINGS.yokogakiRubyOffsetX = value;
  }
}

void EpubReaderActivity::setCurrentRubyOffsetY(const uint8_t value) {
  if (effectiveWritingMode == EpubWritingMode::VerticalRl) {
    SETTINGS.tategakiRubyOffsetY = value;
  } else {
    SETTINGS.yokogakiRubyOffsetY = value;
  }
}

void EpubReaderActivity::enterRubyAdjustMode() {
  rubyAdjustActive = true;
  rubyAdjustIgnoreOpeningRelease = true;
  automaticPageTurnActive = false;
  clearKanjiCursorState(/*saveResumePosition=*/false, /*requestRedraw=*/false);
  requestUpdate();
}

void EpubReaderActivity::exitRubyAdjustMode() {
  rubyAdjustActive = false;
  rubyAdjustIgnoreOpeningRelease = false;
  SETTINGS.saveToFile();
  requestUpdate();
}

void EpubReaderActivity::adjustRubyOffset(const RubyAdjustAxis axis, const int delta) {
  const int current = std::min<int>(axis == RubyAdjustAxis::X ? currentRubyOffsetX() : currentRubyOffsetY(), 32);
  const int next = std::clamp(current + delta, 0, 32);
  if (next == current) {
    return;
  }
  if (axis == RubyAdjustAxis::X) {
    setCurrentRubyOffsetX(static_cast<uint8_t>(next));
  } else {
    setCurrentRubyOffsetY(static_cast<uint8_t>(next));
  }
  requestUpdate();
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
    clearKanjiCursorState(/*saveResumePosition=*/false, /*requestRedraw=*/false);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::clearLatchedPageTurnIntent() {
  pendingPageTurnIntent = PendingPageTurnIntent::None;
  pendingPageTurnIntentAt = 0UL;
}

void EpubReaderActivity::latchPageTurnIntentWhileBusy() {
  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (fromTilt || (!prevTriggered && !nextTriggered)) {
    return;
  }

  // Single-slot latch: preserve only the latest explicit page-turn intent while
  // render/display owns the mutex. This avoids multi-page buffering.
  pendingPageTurnIntent = nextTriggered ? PendingPageTurnIntent::Next : PendingPageTurnIntent::Prev;
  pendingPageTurnIntentAt = millis();
}

bool EpubReaderActivity::consumeLatchedPageTurnIntent(bool& isForwardTurn) {
  if (pendingPageTurnIntent == PendingPageTurnIntent::None) {
    return false;
  }

  const unsigned long age = millis() - pendingPageTurnIntentAt;
  if (age > PENDING_PAGE_TURN_INTENT_TTL_MS) {
    clearLatchedPageTurnIntent();
    return false;
  }

  isForwardTurn = pendingPageTurnIntent == PendingPageTurnIntent::Next;
  clearLatchedPageTurnIntent();
  return true;
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  clearLatchedPageTurnIntent();

  // Clear cursor state without triggering a redundant requestUpdate — pageTurn does its own.
  clearKanjiCursorState(/*saveResumePosition=*/false, /*requestRedraw=*/false);
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
  const int statusBarReserve =
      automaticPageTurnActive &&
              (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())
          ? std::max(
                SETTINGS.screenMargin,
                static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin))
          : std::max(SETTINGS.screenMargin, statusBarHeight);

  const int statusBarContentGap = statusBarHeight > 0 ? SETTINGS.screenMargin : 0;
  orientedMarginBottom += statusBarReserve + statusBarContentGap;

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    const bool sectionCacheHit = section->loadSectionFile(
        effectiveReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
        SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
        SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, SETTINGS.orientation,
        static_cast<uint8_t>(effectiveWritingMode));
    if (!sectionCacheHit) {
      LOG_DBG("ERS", "Cache not found, building...");

      GUI.drawPopup(renderer, tr(STR_INDEXING));

      if (!section->createSectionFile(effectiveReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                      SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, SETTINGS.orientation,
                                      static_cast<uint8_t>(effectiveWritingMode))) {
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

    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  }
  if (kanjiCursorActive && !kanjiPopupActive) {
    if (kanjiCursorRebuildPending && !rebuildKanjiCursorPage()) {
      clearKanjiCursorState(/*saveResumePosition=*/false, /*requestRedraw=*/false);
    }
    kanjiCursorRectValid = false;
    queueKanjiCursorRedraw();
    flushKanjiCursorRefresh();
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, tr(STR_BOOKMARK_ADDED));
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
  if (nextSection.loadSectionFile(effectiveReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, SETTINGS.orientation,
                                  static_cast<uint8_t>(effectiveWritingMode))) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(effectiveReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                     viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                     SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, SETTINGS.orientation,
                                     static_cast<uint8_t>(effectiveWritingMode))) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearPersistentCache();
  }
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}

void EpubReaderActivity::resolveReadingProfile() {
  switch (SETTINGS.writingModePreference) {
    case CrossPointSettings::WRITING_MODE_HORIZONTAL:
      effectiveWritingMode = EpubWritingMode::HorizontalTb;
      break;
    case CrossPointSettings::WRITING_MODE_VERTICAL_RL:
      effectiveWritingMode =
          allowsManualVerticalWritingMode() ? EpubWritingMode::VerticalRl : EpubWritingMode::HorizontalTb;
      break;
    case CrossPointSettings::WRITING_MODE_BOOK_DEFAULT:
    default:
      effectiveWritingMode = epub ? epub->getResolvedWritingMode() : EpubWritingMode::HorizontalTb;
      break;
  }
}

bool EpubReaderActivity::isJapaneseLanguageBook() const {
  if (!epub) return false;
  const std::string& language = epub->getLanguage();
  if (language.empty()) return false;

  std::string primary;
  for (char ch : language) {
    if (ch == '-' || ch == '_' || ch == '.') break;
    primary.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  return primary == "ja" || primary == "jpn";
}

bool EpubReaderActivity::allowsManualVerticalWritingMode() const {
  if (!epub) return false;
  return isJapaneseLanguageBook() || epub->getResolvedWritingMode() != EpubWritingMode::HorizontalTb;
}

bool EpubReaderActivity::shouldUseJapaneseFontSize() const { return isJapaneseLanguageBook(); }

uint8_t EpubReaderActivity::effectiveReaderFontSize() const {
  const uint8_t size = shouldUseJapaneseFontSize() ? SETTINGS.japaneseFontSize : SETTINGS.fontSize;
  return size < CrossPointSettings::FONT_SIZE_COUNT ? size : CrossPointSettings::MEDIUM;
}

int EpubReaderActivity::effectiveReaderFontId() const { return SETTINGS.getReaderFontId(effectiveReaderFontSize()); }

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  const int rubyOffsetX = static_cast<int>(std::min<uint8_t>(currentRubyOffsetX(), 32)) - 16;
  const int rubyOffsetY = static_cast<int>(std::min<uint8_t>(currentRubyOffsetY(), 32)) - 16;
  const int contentBottom = renderer.getScreenHeight() - orientedMarginBottom;

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, effectiveReaderFontId(), orientedMarginLeft, orientedMarginTop, rubyOffsetX, rubyOffsetY,
               contentBottom);  // scan pass
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  // Force special handling for pages with images when anti-aliasing is on
  const bool pageHasImages = page->hasImages();
  bool imagePageWithAA = pageHasImages && SETTINGS.textAntiAliasing;

  page->render(renderer, effectiveReaderFontId(), orientedMarginLeft, orientedMarginTop, rubyOffsetX, rubyOffsetY,
               contentBottom);
  renderStatusBar();
  renderRubyAdjustOverlay();
  const auto tBwRender = millis();

  if (imagePageWithAA) {
    kanjiOverlayFastRefreshPending = false;
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
      page->render(renderer, effectiveReaderFontId(), orientedMarginLeft, orientedMarginTop, rubyOffsetX, rubyOffsetY,
                   contentBottom);
      renderRubyAdjustOverlay();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // Double FAST_REFRESH handles ghosting for image pages; don't count toward full refresh cadence
  } else if (kanjiOverlayFastRefreshPending) {
    kanjiOverlayFastRefreshPending = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Tiled grayscale: render each plane band-by-band into a small scratch and
  // stream straight to the controller, leaving the BW framebuffer intact so no
  // full-frame storeBwBuffer is needed; controller RAM is re-synced from the
  // live framebuffer afterward. The page is re-rendered ceil(H/STRIP_ROWS) times
  // per plane, but renderCharImpl culls out-of-band glyphs before decode so the
  // cost stays close to one render. Both text (drawPixel) and images
  // (DirectPixelWriter) honor the active strip target.
  if (SETTINGS.textAntiAliasing && renderer.supportsStripGrayscale()) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
    if (!scratch) {
      LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
    } else {
      // Bands may be streamed in any order on displays that support windowed
      // grayscale plane writes.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        page->render(renderer, effectiveReaderFontId(), orientedMarginLeft, orientedMarginTop, rubyOffsetX, rubyOffsetY,
                     contentBottom);
        renderRubyAdjustOverlay();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }
      const auto tGrayLsb = millis();

      // MSB plane.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        page->render(renderer, effectiveReaderFontId(), orientedMarginLeft, orientedMarginTop, rubyOffsetX, rubyOffsetY,
                     contentBottom);
        renderRubyAdjustOverlay();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
      }
      const auto tGrayMsb = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tCleanup = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
              "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
              tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
      return;
    }
  }

  // Fallback path for a controller without strip support. grayscale rendering
  // TODO: Only do this if font supports it
  if (SETTINGS.textAntiAliasing) {
    // Save the BW frame before the grayscale passes overwrite it, restore
    // after. Only needed when grayscale actually renders.
    const bool bwBufferStored = renderer.storeBwBuffer();
    const auto tBwStore = millis();

    if (!bwBufferStored) {
      LOG_ERR("ERS", "Rendering text AA without BW backup (free=%u max=%u)", static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()));

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      page->render(renderer, effectiveReaderFontId(), orientedMarginLeft, orientedMarginTop, rubyOffsetX, rubyOffsetY,
                   contentBottom);
      renderRubyAdjustOverlay();
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      page->render(renderer, effectiveReaderFontId(), orientedMarginLeft, orientedMarginTop, rubyOffsetX, rubyOffsetY,
                   contentBottom);
      renderRubyAdjustOverlay();
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.clearScreen();
      page->render(renderer, effectiveReaderFontId(), orientedMarginLeft, orientedMarginTop, rubyOffsetX, rubyOffsetY,
                   contentBottom);
      renderStatusBar();
      renderRubyAdjustOverlay();
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_rerender=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
      return;
    }

    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, effectiveReaderFontId(), orientedMarginLeft, orientedMarginTop, rubyOffsetX, rubyOffsetY,
                 contentBottom);
    renderRubyAdjustOverlay();
    renderer.copyGrayscaleLsbBuffers();
    const auto tGrayLsb = millis();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, effectiveReaderFontId(), orientedMarginLeft, orientedMarginTop, rubyOffsetX, rubyOffsetY,
                 contentBottom);
    renderRubyAdjustOverlay();
    renderer.copyGrayscaleMsbBuffers();
    const auto tGrayMsb = millis();

    // display grayscale part
    renderer.displayGrayBuffer();
    const auto tGrayDisplay = millis();
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
            "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
            tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    return;
  }

  // No anti-aliasing: BW frame already displayed above, no grayscale to
  // render, so no save/restore.
  const auto tEnd = millis();
  LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
          tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
}

std::string EpubReaderActivity::statusBarTitleForCurrentSpine() const {
  const uint8_t mode = SETTINGS.statusBarTitle;
  if (mode == CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    return {};
  }
  if (cachedStatusTitleSpineIndex == currentSpineIndex && cachedStatusTitleMode == mode) {
    return cachedStatusTitle;
  }

  std::string title;
  if (mode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = EpubReaderNavigation::titleForSpine(epub, currentSpineIndex,
                                                std::string("Section ") + std::to_string(currentSpineIndex + 1));
  } else if (mode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = StringUtils::uiSafeBookTitle(epub->getTitle(), epub->getPath());
  }

  cachedStatusTitleSpineIndex = currentSpineIndex;
  cachedStatusTitleMode = mode;
  cachedStatusTitle = title;
  return cachedStatusTitle;
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

  } else {
    title = statusBarTitleForCurrentSpine();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset);
}

void EpubReaderActivity::renderRubyAdjustOverlay() const {
  if (!rubyAdjustActive) {
    return;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "X-", "X+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, "Y-", "Y+");
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
  targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr, currentSpineIndex);
  if (sameFile && targetSpineIndex < 0) {
    targetSpineIndex = currentSpineIndex;
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    clearKanjiCursorState(/*saveResumePosition=*/false, /*requestRedraw=*/false);
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
    clearKanjiCursorState(/*saveResumePosition=*/false, /*requestRedraw=*/false);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

// --- Japanese dictionary cursor overlay ---

void EpubReaderActivity::enterKanjiCursorMode() {
  if (!section) return;
  LOG_DBG("DICT", "Enter cursor requested spine=%d page=%d", currentSpineIndex, section->currentPage);

  if (!rebuildKanjiCursorPage()) {
    return;
  }

  queueKanjiCursorRedraw();
  flushKanjiCursorRefresh();
}

bool EpubReaderActivity::rebuildKanjiCursorPage() {
  if (!section) return false;

  // Compute the same margins used by renderContents so cursor coords align.
  int dummy1, dummy2;
  renderer.getOrientedViewableTRBL(&kanjiMarginTop, &dummy1, &dummy2, &kanjiMarginLeft);
  kanjiMarginTop += SETTINGS.screenMargin;
  kanjiMarginLeft += SETTINGS.screenMargin;

  kanjiCursorPage = section->loadPageFromSectionFile();
  if (!kanjiCursorPage) {
    LOG_ERR("CURSOR", "Failed to load page for cursor mode");
    return false;
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
    return false;
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
  kanjiCursorRebuildPending = false;
  LOG_DBG("DICT", "Enter cursor ok entries=%d pos=%d resumed=%d", static_cast<int>(kanjiIndex.size()), kanjiIndexPos,
          resumed ? 1 : 0);
  return true;
}

void EpubReaderActivity::releaseKanjiCursorPageCache(const bool releaseIndexCapacity) {
  kanjiCursorPage.reset();
  kanjiIndex.clear();
  if (releaseIndexCapacity) {
    std::vector<KanjiEntry>().swap(kanjiIndex);
  }
}

void EpubReaderActivity::releaseKanjiPopupMatches() {
  kanjiPopupMatches.clear();
  std::vector<JapaneseDictionaryMatch>().swap(kanjiPopupMatches);
  kanjiPopupMatchIndex = 0;
}

void EpubReaderActivity::clearKanjiCursorState(const bool saveResumePosition, const bool requestRedraw) {
  if (saveResumePosition && section && kanjiCursorActive && !kanjiIndex.empty() && kanjiIndexPos >= 0 &&
      kanjiIndexPos < static_cast<int>(kanjiIndex.size())) {
    kanjiResumeValid = true;
    kanjiResumeSpineIndex = currentSpineIndex;
    kanjiResumePageNumber = section->currentPage;
    kanjiResumeIndexPos = kanjiIndexPos;
    LOG_DBG("DICT", "Exit cursor saved spine=%d page=%d pos=%d entries=%d", kanjiResumeSpineIndex,
            kanjiResumePageNumber, kanjiResumeIndexPos, static_cast<int>(kanjiIndex.size()));
  } else if (kanjiCursorActive || !kanjiIndex.empty() || kanjiPopupActive) {
    LOG_DBG("DICT", "Exit cursor no-save active=%d entries=%d", kanjiCursorActive ? 1 : 0,
            static_cast<int>(kanjiIndex.size()));
  }
  kanjiCursorActive = false;
  kanjiPopupActive = false;
  releaseKanjiPopupMatches();
  kanjiCursorRefreshPending = false;
  kanjiOverlayFastRefreshPending = false;
  kanjiCursorRebuildPending = false;
  kanjiCursorRectValid = false;
  releaseKanjiCursorPageCache(/*releaseIndexCapacity=*/true);
  kanjiDictionary.close();
  if (requestRedraw) {
    requestUpdate();
  }
}

void EpubReaderActivity::exitKanjiCursorMode() {
  clearKanjiCursorState(/*saveResumePosition=*/true, /*requestRedraw=*/false);
  kanjiOverlayFastRefreshPending = true;
  requestUpdate();
}

void EpubReaderActivity::moveKanjiCursor(const int direction) {
  if (!kanjiCursorActive || kanjiIndex.empty()) return;
  const int next = kanjiIndexPos + direction;
  if (next < 0 || next >= static_cast<int>(kanjiIndex.size())) return;
  kanjiIndexPos = next;
  LOG_DBG("DICT", "Move cursor dir=%d pos=%d/%d", direction, kanjiIndexPos + 1, static_cast<int>(kanjiIndex.size()));
  queueKanjiCursorRedraw();
}

void EpubReaderActivity::moveKanjiCursorToLine(const int direction) {
  if (!kanjiCursorActive || kanjiIndex.empty() || !kanjiCursorPage) return;
  const auto& elements = kanjiCursorPage->elements;

  const auto& cur = kanjiIndex[kanjiIndexPos];
  if (cur.elementIdx >= static_cast<int16_t>(elements.size())) return;
  if (elements[cur.elementIdx]->getTag() != TAG_PageLine) return;
  const auto& curLine = static_cast<const PageLine&>(*elements[cur.elementIdx]);
  const bool vertical = curLine.getBlock() && curLine.getBlock()->isVertical();
  const int curAxisPos = vertical ? curLine.xPos : curLine.yPos;

  // Scan in direction for the first kanji whose PageLine is on a different line/column.
  int pos = kanjiIndexPos + direction;
  while (pos >= 0 && pos < static_cast<int>(kanjiIndex.size())) {
    const auto& e = kanjiIndex[pos];
    if (e.elementIdx < static_cast<int16_t>(elements.size()) && elements[e.elementIdx]->getTag() == TAG_PageLine) {
      const auto& line = static_cast<const PageLine&>(*elements[e.elementIdx]);
      const int axisPos = vertical ? line.xPos : line.yPos;
      if (axisPos != curAxisPos) {
        kanjiIndexPos = pos;
        LOG_DBG("DICT", "Jump cursor dir=%d pos=%d/%d", direction, kanjiIndexPos + 1,
                static_cast<int>(kanjiIndex.size()));
        queueKanjiCursorRedraw();
        return;
      }
    }
    pos += direction;
  }

  kanjiIndexPos = direction > 0 ? 0 : static_cast<int>(kanjiIndex.size()) - 1;
  LOG_DBG("DICT", "Wrap cursor dir=%d pos=%d/%d", direction, kanjiIndexPos + 1, static_cast<int>(kanjiIndex.size()));
  queueKanjiCursorRedraw();
}

bool EpubReaderActivity::getSelectedKanjiBlock(const PageLine*& pageLine, const TextBlock*& textBlock,
                                               KanjiEntry& entry) const {
  if (!kanjiCursorPage || kanjiIndex.empty() || kanjiIndexPos < 0 ||
      kanjiIndexPos >= static_cast<int>(kanjiIndex.size())) {
    return false;
  }

  entry = kanjiIndex[kanjiIndexPos];
  const auto& elements = kanjiCursorPage->elements;
  if (entry.elementIdx >= static_cast<int16_t>(elements.size())) return false;
  if (elements[entry.elementIdx]->getTag() != TAG_PageLine) return false;

  pageLine = &static_cast<const PageLine&>(*elements[entry.elementIdx]);
  if (!pageLine->getBlock()) return false;
  textBlock = pageLine->getBlock().get();
  return true;
}

bool EpubReaderActivity::getKanjiCursorRect(const PageLine& pageLine, const TextBlock& textBlock,
                                            const KanjiEntry& entry, KanjiCursorRect& rect) const {
  const auto& words = textBlock.getWords();
  const auto& xposVec = textBlock.getWordXpos();
  const auto& yposVec = textBlock.getWordYpos();
  const auto& styles = textBlock.getWordStyles();
  if (entry.wordIdx >= static_cast<int16_t>(words.size()) || entry.wordIdx >= static_cast<int16_t>(styles.size())) {
    return false;
  }
  if (textBlock.isVertical()) {
    if (!xposVec.empty() && entry.wordIdx >= static_cast<int16_t>(xposVec.size())) return false;
  } else if (entry.wordIdx >= static_cast<int16_t>(xposVec.size())) {
    return false;
  }
  if (textBlock.isVertical() && entry.wordIdx >= static_cast<int16_t>(yposVec.size())) return false;

  const int cellSize = renderer.getLineHeight(effectiveReaderFontId());
  rect.width = cellSize;
  rect.height = cellSize;

  if (textBlock.isVertical()) {
    const int wordX = xposVec.empty() ? 0 : xposVec[entry.wordIdx];
    rect.x = kanjiMarginLeft + pageLine.xPos + wordX;
    rect.y = kanjiMarginTop + pageLine.yPos + yposVec[entry.wordIdx];
    return true;
  }

  int intraWordX = 0;
  if (entry.byteOffset > 0 && entry.byteOffset <= words[entry.wordIdx].size()) {
    const std::string prefix = words[entry.wordIdx].substr(0, entry.byteOffset);
    intraWordX = renderer.getTextAdvanceX(effectiveReaderFontId(), prefix.c_str(), styles[entry.wordIdx]);
  }

  rect.x = kanjiMarginLeft + pageLine.xPos + xposVec[entry.wordIdx] + intraWordX;
  rect.y = kanjiMarginTop + pageLine.yPos;
  return true;
}

bool EpubReaderActivity::drawKanjiCursor() {
  if (!kanjiCursorActive || !kanjiCursorPage || kanjiIndex.empty()) return false;
  const PageLine* pageLine = nullptr;
  const TextBlock* textBlock = nullptr;
  KanjiEntry entry;
  if (!getSelectedKanjiBlock(pageLine, textBlock, entry)) return false;

  KanjiCursorRect rect;
  if (!getKanjiCursorRect(*pageLine, *textBlock, entry, rect)) return false;

  if (kanjiCursorRectValid) {
    renderer.drawRect(kanjiCursorRectX, kanjiCursorRectY, kanjiCursorRectSize, kanjiCursorRectSize, 2, false);
  }

  renderer.drawRect(rect.x, rect.y, rect.width, rect.height, 2, true);
  kanjiCursorRectValid = true;
  kanjiCursorRectX = rect.x;
  kanjiCursorRectY = rect.y;
  kanjiCursorRectSize = rect.width;
  return true;
}

void EpubReaderActivity::queueKanjiCursorRedraw() {
  if (drawKanjiCursor()) {
    kanjiCursorRefreshPending = true;
  }
}

void EpubReaderActivity::flushKanjiCursorRefresh() {
  if (!kanjiCursorRefreshPending) return;
  kanjiCursorRefreshPending = false;
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

std::string EpubReaderActivity::extractKanjiLookupText(const size_t maxChars) const {
  if (!section || !kanjiCursorPage || kanjiIndex.empty() || kanjiIndexPos < 0 ||
      kanjiIndexPos >= static_cast<int>(kanjiIndex.size()) || maxChars == 0) {
    return "";
  }

  const auto& selected = kanjiIndex[kanjiIndexPos];
  std::string result;
  size_t charCount = 0;
  int parenDepth = 0;

  auto appendFromPage = [&](const Page& page, const bool startAtSelection) {
    bool started = !startAtSelection;
    const auto& elements = page.elements;
    for (int16_t ei = 0; ei < static_cast<int16_t>(elements.size()) && charCount < maxChars; ++ei) {
      if (elements[ei]->getTag() != TAG_PageLine) continue;
      if (!started && ei < selected.elementIdx) continue;

      const auto& pageLine = static_cast<const PageLine&>(*elements[ei]);
      if (!pageLine.getBlock()) continue;
      const auto& block = *pageLine.getBlock();
      const auto& words = block.getWords();
      for (int16_t wi = 0; wi < static_cast<int16_t>(words.size()) && charCount < maxChars; ++wi) {
        if (!started) {
          if (ei != selected.elementIdx || wi != selected.wordIdx) continue;
          started = true;
        }

        const size_t startOffset =
            startAtSelection && ei == selected.elementIdx && wi == selected.wordIdx ? selected.byteOffset : 0;
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
  };

  appendFromPage(*kanjiCursorPage, /*startAtSelection=*/true);
  if (charCount < maxChars && section->currentPage + 1 < section->pageCount) {
    auto nextPage = section->loadPageFromSectionFile(static_cast<uint16_t>(section->currentPage + 1));
    if (nextPage) {
      appendFromPage(*nextPage, /*startAtSelection=*/false);
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
  kanjiDictionary.close();

  drawKanjiPopup();
}

void EpubReaderActivity::drawKanjiPopup() {
  const bool hasMatch = !kanjiPopupMatches.empty();

  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const int pad = 18;
  const int popupW = sw * 17 / 20;
  const int popupH = sh * 7 / 10;
  const int popupX = (sw - popupW) / 2;
  const int popupY = (sh - popupH) / 2;
  const int maxTextWidth = popupW - pad * 2;
  int textY = popupY + pad;

  renderer.fillRect(popupX, popupY, popupW, popupH, false);
  renderer.drawRect(popupX, popupY, popupW, popupH, 2, true);

  if (hasMatch) {
    if (kanjiPopupMatchIndex >= kanjiPopupMatches.size()) kanjiPopupMatchIndex = 0;
    const auto& match = kanjiPopupMatches[kanjiPopupMatchIndex];
    const std::string term =
        renderer.truncatedText(effectiveReaderFontId(), match.term.c_str(), maxTextWidth, EpdFontFamily::BOLD);
    renderer.drawText(effectiveReaderFontId(), popupX + pad, textY, term.c_str(), true, EpdFontFamily::BOLD);
    textY += renderer.getLineHeight(effectiveReaderFontId()) + 8;

    const std::string pronunciation = popupPronunciationLine(match);
    if (!pronunciation.empty()) {
      const std::string reading = renderer.truncatedText(effectiveReaderFontId(), pronunciation.c_str(), maxTextWidth);
      renderer.drawText(effectiveReaderFontId(), popupX + pad, textY, reading.c_str(), true);
      textY += renderer.getLineHeight(effectiveReaderFontId()) + 8;
    }
    if (match.sourceText != match.term) {
      const std::string source =
          renderer.truncatedText(UI_10_FONT_ID, ("from " + match.sourceText).c_str(), maxTextWidth);
      renderer.drawText(UI_10_FONT_ID, popupX + pad, textY, source.c_str(), true);
      textY += renderer.getLineHeight(UI_10_FONT_ID) + 6;
    }

    const PopupDefinition definition = popupDefinitionItems(match.definition, 6);
    if (hasVisibleDefinitionText(definition.attributes)) {
      const std::string attrs = renderer.truncatedText(UI_10_FONT_ID, ("(" + definition.attributes + ")").c_str(),
                                                       maxTextWidth, EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, popupX + pad, textY, attrs.c_str(), true, EpdFontFamily::BOLD);
      textY += renderer.getLineHeight(UI_10_FONT_ID) + 6;
    }
    const int lineStep = renderer.getLineHeight(UI_10_FONT_ID) + 4;
    for (const auto& item : definition.glosses) {
      const int remainingLines = (popupY + popupH - pad - textY) / lineStep;
      if (remainingLines <= 1) break;
      const auto lines = renderer.wrappedText(UI_10_FONT_ID, item.c_str(), maxTextWidth, remainingLines);
      for (const auto& line : lines) {
        renderer.drawText(UI_10_FONT_ID, popupX + pad, textY, line.c_str(), true);
        textY += lineStep;
        if (textY >= popupY + popupH - pad) break;
      }
    }
    if (kanjiPopupMatches.size() > 1) {
      const std::string position =
          std::to_string(kanjiPopupMatchIndex + 1) + "/" + std::to_string(kanjiPopupMatches.size());
      const int posWidth = renderer.getTextWidth(UI_10_FONT_ID, position.c_str());
      renderer.drawText(UI_10_FONT_ID, popupX + popupW - pad - posWidth, popupY + pad, position.c_str(), true);
    }
  } else {
    renderer.drawText(UI_10_FONT_ID, popupX + pad, textY, "No dictionary match", true);
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
  if (section && kanjiCursorActive && !kanjiIndex.empty() && kanjiIndexPos >= 0 &&
      kanjiIndexPos < static_cast<int>(kanjiIndex.size())) {
    kanjiResumeValid = true;
    kanjiResumeSpineIndex = currentSpineIndex;
    kanjiResumePageNumber = section->currentPage;
    kanjiResumeIndexPos = kanjiIndexPos;
  }
  kanjiPopupActive = false;
  releaseKanjiPopupMatches();
  kanjiDictionary.close();
  releaseKanjiCursorPageCache(/*releaseIndexCapacity=*/true);
  kanjiCursorRectValid = false;
  kanjiCursorRebuildPending = true;
  kanjiOverlayFastRefreshPending = true;
  requestUpdate();  // repaint the page under the popup, then restore the cursor overlay
}

// --- End kanji cursor overlay ---

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) {
    return;
  }
  LOG_DBG("ERS", "Adding bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock(*this);
    pageCount = section->pageCount;
    currentPage = section->currentPage;
  }

  std::string pageText;
  if (currentPage >= 0 && currentPage < pageCount) {
    pageText = section->getTextFromSectionFile();
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());

  BookmarkEntry entry;
  entry.percentage = progress.percentage;
  entry.xpath = progress.xpath;
  entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
  entry.computedSpineIndex = static_cast<uint16_t>(currentSpineIndex);
  entry.computedChapterPageCount = static_cast<uint16_t>(pageCount);
  entry.computedChapterProgress = static_cast<uint16_t>(std::clamp(currentPage, 0, std::max(0, pageCount - 1)));

  // Add bookmark
  const std::string path = BookmarkUtil::getBookmarkPath(epub->getPath());
  LOG_DBG("ERS", "Bookmark path: %s", path.c_str());
  const std::string bookmarksDir = BookmarkUtil::getBookmarksDir();
  Storage.mkdir(bookmarksDir.c_str());
  std::vector<BookmarkEntry> bookmarks;
  if (Storage.exists(path.c_str())) {
    LOG_DBG("ERS", "Existing bookmark file found, loading bookmarks");
    String json = Storage.readFile(path.c_str());
    if (!json.isEmpty()) {
      JsonSettingsIO::loadBookmarks(bookmarks, json.c_str());
    }
  } else {
    LOG_DBG("ERS", "No existing bookmark file, starting with empty bookmark list");
  }
  bookmarks.insert(bookmarks.begin(), entry);
  LOG_DBG("ERS", "Saving bookmark to file: %s", path.c_str());
  const bool ok = JsonSettingsIO::saveBookmarks(bookmarks, path.c_str());
  if (ok) {
    showBookmarkMessage = true;
  } else {
    LOG_ERR("ERS", "Failed to save bookmark to: %s", path.c_str());
  }

  requestUpdate();
}

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

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
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

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
