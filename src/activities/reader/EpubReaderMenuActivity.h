#pragma once
#include <CrossPointSettings.h>
#include <Epub.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderMenuActivity final : public Activity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    SELECT_CHAPTER,
    FOOTNOTES,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    WRITING_MODE,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    DELETE_CACHE
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  uint8_t orientation, uint8_t writingModePreference, bool hasFootnotes);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  static std::vector<MenuItem> buildMenuItems(bool hasFootnotes);
  uint8_t normalizeOrientation(uint8_t orientation) const;
  uint8_t normalizeWritingModePreference(uint8_t writingModePreference) const;

  // Fixed menu layout
  const std::vector<MenuItem> menuItems;

  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = CrossPointSettings::PORTRAIT;
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
