#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t orientation,
                                               const uint8_t writingModePreference, const bool hasFootnotes)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes)),
      title(title),
      pendingOrientation(normalizeOrientation(orientation)),
      pendingWritingModePreference(normalizeWritingModePreference(writingModePreference)),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes) {
  std::vector<MenuItem> items;
  items.reserve(11);
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::WRITING_MODE, StrId::STR_READING_LAYOUT});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  return items;
}

uint8_t EpubReaderMenuActivity::normalizeOrientation(const uint8_t orientation) const {
  return orientation < orientationLabels.size() ? orientation : CrossPointSettings::PORTRAIT;
}

uint8_t EpubReaderMenuActivity::normalizeWritingModePreference(const uint8_t writingModePreference) const {
  return writingModePreference < writingModeLabels.size() ? writingModePreference
                                                          : CrossPointSettings::WRITING_MODE_BOOK_DEFAULT;
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  previousRendererOrientation = renderer.getOrientation();
  ReaderUtils::applyOrientation(renderer, pendingOrientation);
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() {
  renderer.setOrientation(previousRendererOrientation);
  Activity::onExit();
}

void EpubReaderMenuActivity::loop() {
  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      // Cycle orientation preview locally; actual orientation change happens on menu exit.
      pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
      ReaderUtils::applyOrientation(renderer, pendingOrientation);
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
      selectedPageTurnOption = (selectedPageTurnOption + 1) % pageTurnLabels.size();
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::WRITING_MODE) {
      pendingWritingModePreference = (pendingWritingModePreference + 1) % writingModeLabels.size();
      requestUpdate();
      return;
    }

    setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, pendingWritingModePreference,
                         selectedPageTurnOption});
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{-1, pendingOrientation, pendingWritingModePreference, selectedPageTurnOption};
    setResult(std::move(result));
    finish();
    return;
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str());

  // Progress summary
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      progressLine.c_str());

  const int contentTop =
      screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, menuItems.size(), selectedIndex,
      [this](int index) { return I18N.get(menuItems[index].labelId); }, nullptr, nullptr,
      [this](int index) {
        const auto value = menuItems[index].action;
        if (value == MenuAction::ROTATE_SCREEN) {
          return I18N.get(orientationLabels[normalizeOrientation(pendingOrientation)]);
        } else if (value == MenuAction::WRITING_MODE) {
          return I18N.get(writingModeLabels[normalizeWritingModePreference(pendingWritingModePreference)]);
        } else if (value == MenuAction::AUTO_PAGE_TURN) {
          return pageTurnLabels[selectedPageTurnOption];
        } else {
          return "";
        }
      },
      true);

  // Footer / Hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
