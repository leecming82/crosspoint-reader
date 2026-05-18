#pragma once
#include <CrossPointSettings.h>
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <JapaneseDictionary.h>

#include <optional>
#include <string>
#include <vector>

#include "EpubReaderMenuActivity.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool automaticPageTurnActive = false;
  uint8_t effectiveReadingLayout = CrossPointSettings::READING_LAYOUT_HORIZONTAL_PORTRAIT;
  uint8_t effectiveReaderOrientation = CrossPointSettings::PORTRAIT;

  // Kanji cursor overlay (tategaki dictionary lookup, Phase 1)
  struct KanjiEntry {
    int16_t elementIdx;
    int16_t wordIdx;
    uint16_t byteOffset;
  };
  bool kanjiCursorActive = false;
  bool kanjiPopupActive = false;
  JapaneseDictionary kanjiDictionary;
  std::vector<JapaneseDictionaryMatch> kanjiPopupMatches;
  size_t kanjiPopupMatchIndex = 0;
  std::unique_ptr<Page> kanjiCursorPage;
  std::vector<KanjiEntry> kanjiIndex;
  int kanjiIndexPos = 0;
  int kanjiMarginLeft = 0;
  int kanjiMarginTop = 0;
  bool kanjiCursorRectValid = false;
  int kanjiCursorRectX = 0;
  int kanjiCursorRectY = 0;
  int kanjiCursorRectSize = 0;
  bool kanjiCursorRefreshPending = false;
  bool kanjiResumeValid = false;
  int kanjiResumeSpineIndex = -1;
  int kanjiResumePageNumber = -1;
  int kanjiResumeIndexPos = 0;

  static constexpr unsigned long CURSOR_ENTER_MS = 600;

  void enterKanjiCursorMode();
  void exitKanjiCursorMode();
  void moveKanjiCursor(int direction);
  void moveKanjiCursorToLine(int direction);
  bool drawKanjiCursor();
  void queueKanjiCursorRedraw();
  void flushKanjiCursorRefresh();
  std::string extractKanjiLookupText(size_t maxChars) const;
  void showKanjiPopup();
  void drawKanjiPopup();
  void moveKanjiPopupMatch(int direction);
  void hideKanjiPopup();

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  void resolveReadingProfile();
  uint8_t effectiveReaderFontSize() const;
  int effectiveReaderFontId() const;
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyReadingLayout(uint8_t readingLayout);
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
};
