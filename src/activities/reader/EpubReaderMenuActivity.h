#pragma once
#include <CrossPointSettings.h>
#include <Epub.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/TouchNavigator.h"

class EpubReaderMenuActivity final : public Activity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    SELECT_CHAPTER,
    FOOTNOTES,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    ADD_BOOKMARK,
    BOOKMARKS,
    WRITING_MODE,
    RUBY_OFFSET,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    DELETE_CACHE
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  uint8_t orientation, uint8_t writingModePreference, bool hasFootnotes,
                                  bool allowVerticalWritingMode);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowsGlobalTouchBack() const override { return false; }

 private:
  enum class MenuTab { Navigate = 0, Reading, Tools, Count };

  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  static std::vector<MenuItem> buildMenuItems(bool hasFootnotes);
  static std::vector<std::vector<MenuItem>> buildTabbedMenuItems(bool hasFootnotes);
  uint8_t normalizeOrientation(uint8_t orientation) const;
  uint8_t normalizeWritingModePreference(uint8_t writingModePreference) const;
  uint8_t writingModeOptionCount() const;
  const std::vector<MenuItem>& activeMenuItems() const;
  void activateSelectedAction();
  void cancelMenu();
  bool handleTouch();
  Rect screenRect() const;
  Rect contentRect() const;
  Rect headerBackRect() const;
  Rect backButtonRect() const;
  Rect progressRect() const;
  Rect tabBarRect() const;
  Rect footerHintsRect() const;
  int listPageItems() const;
  int currentListPageStart() const;
  int visibleMenuItemCount() const;
  int visibleListRowCount() const;
  bool hasPreviousListPage() const;
  bool hasNextListPage() const;
  bool isPreviousPageRow(int visibleRow) const;
  bool isNextPageRow(int visibleRow) const;
  int visibleRowToMenuIndex(int visibleRow) const;
  void selectTab(int tabIndex);

  // Fixed menu layout
  const std::vector<MenuItem> menuItems;
  const std::vector<std::vector<MenuItem>> tabMenuItems;

  int selectedTabIndex = 0;
  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = CrossPointSettings::PORTRAIT;
  bool allowVerticalWritingMode = false;
  uint8_t pendingWritingModePreference = CrossPointSettings::WRITING_MODE_BOOK_DEFAULT;
  uint8_t selectedPageTurnOption = 0;
  GfxRenderer::Orientation previousRendererOrientation = GfxRenderer::Orientation::Portrait;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  const std::vector<StrId> writingModeLabels = {StrId::STR_BOOK_S_STYLE, StrId::STR_READING_LAYOUT_HORIZONTAL_PORTRAIT,
                                                StrId::STR_READING_LAYOUT_VERTICAL_RL};
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
};
