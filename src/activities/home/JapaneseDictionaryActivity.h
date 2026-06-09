#pragma once

#include <JapaneseDictionary.h>
#include <KanjiIndex.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

class JapaneseDictionaryActivity final : public Activity {
 public:
  explicit JapaneseDictionaryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("JapaneseDictionary", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowsGlobalTouchBack() const override { return false; }

  static constexpr int COLS = 5;
  static constexpr int ROWS = 6;
  static constexpr int BOTTOM_KEY_COUNT = 5;
  static constexpr size_t MAX_RESULTS = 32;

 private:
  enum class ViewMode { Editing, Results, Detail, KanjiSearch };

  JapaneseDictionaryBundleStatus bundleStatus;
  JapaneseDictionary dictionary;
  KanjiIndex kanjiIndex;
  JapaneseDictionaryExactCursor exactCursor;
  std::string committedKana;
  std::string pendingRomaji;
  std::string kanjiSearchCommitted;
  std::string kanjiSearchPending;
  std::string kanjiSearchDigits;
  std::vector<JapaneseDictionaryMatch> results;
  std::vector<std::string> selectedRadicals;
  std::vector<std::string> radicalCandidates;
  std::vector<std::string> kanjiCandidates;
  bool searched = false;
  ViewMode viewMode = ViewMode::Editing;
  bool dictionaryOpen = false;
  bool kanjiIndexOpen = false;
  bool symbolsMode = false;
  bool kanjiKeyboardVisible = true;
  size_t selectedResult = 0;
  int detailLineOffset = 0;
  int radicalPageOffset = 0;
  int kanjiPageOffset = 0;

  std::string queryText() const;
  std::string kanjiSearchText() const;
  bool kanjiSearchIsStrokeCount() const;
  void insertChar(char ch);
  void insertKanjiSearchChar(char ch);
  void insertSpace();
  void backspace();
  void backspaceKanjiSearch();
  void clearQuery();
  void clearKanjiSearchInput();
  void finalizePendingRomaji();
  void finalizeKanjiSearchPending();
  void search();
  void openKanjiSearch();
  void closeKanjiSearch();
  bool openKanjiIndex();
  void refreshKanjiSearchCandidates();
  void addSelectedRadical(const std::string& radical);
  void removeSelectedRadical(size_t index);
  void insertKanjiCandidate(const std::string& kanji);
  bool hasMoreExactResults() const;
  int resultLayoutItemCount() const;
  bool appendNextExactResultPage();
  std::vector<std::string> detailLines() const;
  int detailLinesPerPage(int lineOffset) const;
  int detailMaxLineOffset() const;
  void pageDetail(int direction);
  void handleBack();
  int resultsPerPage() const;
  bool handleTouch();
  Rect queryRect() const;
  Rect resultListRect() const;
  Rect kanjiSearchRect() const;
  Rect selectedRadicalsRect() const;
  Rect kanjiRadicalsRect(bool hasKanji) const;
  Rect kanjiCandidatesRect(bool hasRadicals) const;
  void drawMissingState();
  void drawQueryField();
  void drawResults();
  void drawDetail();
  void drawKanjiSearch();
  void drawKeyboard();
};
