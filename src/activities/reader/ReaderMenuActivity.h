#pragma once

#include <I18n.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class ReaderMenuActivity final : public Activity {
 public:
  enum class MenuAction {
    SELECT_CHAPTER,
    FOOTNOTES,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    DELETE_CACHE
  };

  enum MenuOption : uint16_t {
    OPTION_SELECT_CHAPTER = 1 << 0,
    OPTION_FOOTNOTES = 1 << 1,
    OPTION_GO_TO_PERCENT = 1 << 2,
    OPTION_AUTO_PAGE_TURN = 1 << 3,
    OPTION_ROTATE_SCREEN = 1 << 4,
    OPTION_SCREENSHOT = 1 << 5,
    OPTION_DISPLAY_QR = 1 << 6,
    OPTION_GO_HOME = 1 << 7,
    OPTION_SYNC = 1 << 8,
    OPTION_DELETE_CACHE = 1 << 9,
  };

  explicit ReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                              int currentPage, int totalPages, int bookProgressPercent, uint8_t currentOrientation,
                              uint16_t options, bool currentPageIsChapterPage = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  static constexpr size_t MAX_MENU_ITEMS = 10;
  static constexpr std::array<StrId, 4> ORIENTATION_LABELS = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                                              StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW};
  static constexpr std::array<int, 5> PAGE_TURN_RATES = {0, 1, 3, 6, 12};

  std::array<MenuItem, MAX_MENU_ITEMS> menuItems = {};
  size_t menuItemCount = 0;

  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;
  std::string title;
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
  bool currentPageIsChapterPage = false;

  void addMenuItem(MenuAction action, StrId labelId);
  void buildMenuItems(uint16_t options);
  const char* getPageTurnLabel(uint8_t option) const;
};
