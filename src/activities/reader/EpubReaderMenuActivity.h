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
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    DELETE_CACHE
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t configuredReadingLayout, const uint8_t resolvedReadingLayout,
                                  const bool hasFootnotes);

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
  uint8_t normalizeReadingLayout(uint8_t readingLayout) const;
  uint8_t menuLayoutForPending() const;
  void applyMenuOrientation();

  // Fixed menu layout
  const std::vector<MenuItem> menuItems;

  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;
  std::string title = "Reader Menu";
  uint8_t pendingReadingLayout = 0;
  uint8_t effectiveReadingLayout = CrossPointSettings::READING_LAYOUT_HORIZONTAL_PORTRAIT;
  uint8_t selectedPageTurnOption = 0;
  GfxRenderer::Orientation previousRendererOrientation = GfxRenderer::Orientation::Portrait;
  const std::vector<StrId> readingLayoutLabels = {
      StrId::STR_READING_LAYOUT_AUTO,
      StrId::STR_READING_LAYOUT_HORIZONTAL_PORTRAIT,
      StrId::STR_READING_LAYOUT_HORIZONTAL_LANDSCAPE_CW,
      StrId::STR_READING_LAYOUT_HORIZONTAL_INVERTED,
      StrId::STR_READING_LAYOUT_HORIZONTAL_LANDSCAPE_CCW,
      StrId::STR_READING_LAYOUT_VERTICAL_RL,
  };
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
};
