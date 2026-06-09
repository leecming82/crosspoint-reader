#pragma once

#include <JapaneseDictionary.h>

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
  static constexpr size_t MAX_RESULTS = 8;

 private:
  enum class ViewMode { Editing, Results, Detail };

  JapaneseDictionaryBundleStatus bundleStatus;
  JapaneseDictionary dictionary;
  std::string committedKana;
  std::string pendingRomaji;
  std::vector<JapaneseDictionaryMatch> results;
  bool searched = false;
  ViewMode viewMode = ViewMode::Editing;
  bool dictionaryOpen = false;
  bool symbolsMode = false;
  size_t selectedResult = 0;

  std::string queryText() const;
  void insertChar(char ch);
  void insertSpace();
  void backspace();
  void clearQuery();
  void finalizePendingRomaji();
  void search();
  void handleBack();
  int resultsPerPage() const;
  bool handleTouch();
  Rect queryRect() const;
  Rect resultListRect() const;
  void drawMissingState();
  void drawQueryField();
  void drawResults();
  void drawDetail();
  void drawKeyboard();
};
