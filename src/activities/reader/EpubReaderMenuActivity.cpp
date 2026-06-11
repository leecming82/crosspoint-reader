#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <utility>

#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "components/icons/back24.h"
#include "fontIds.h"

namespace {
constexpr int BACK_ICON_SIZE = 24;
constexpr int BACK_ICON_VISUAL_OFFSET_Y = 5;

struct MenuPageLayout {
  int start = 0;
  int itemCount = 0;
  bool previous = false;
  bool next = false;
};

MenuPageLayout calculateMenuPageLayout(int selectedIndex, int itemCount, int listRows) {
  itemCount = std::max(0, itemCount);
  listRows = std::max(1, listRows);
  selectedIndex = std::clamp(selectedIndex, 0, std::max(0, itemCount - 1));

  MenuPageLayout layout;
  int start = 0;
  bool previous = false;

  while (start < itemCount) {
    const int remaining = itemCount - start;
    const int rowsAvailable = std::max(1, listRows - (previous ? 1 : 0));
    const bool next = remaining > rowsAvailable;
    const int itemsOnPage = next ? std::max(1, rowsAvailable - 1) : remaining;

    layout = MenuPageLayout{start, itemsOnPage, previous, next};
    if (selectedIndex < start + itemsOnPage || !next) {
      break;
    }

    start += itemsOnPage;
    previous = true;
  }

  return layout;
}

void drawReaderMenuHeaderTitle(const GfxRenderer& renderer, Rect backRect, const char* title) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int iconX = backRect.x + (backRect.width - BACK_ICON_SIZE) / 2;
  const int iconY = backRect.y + (backRect.height - BACK_ICON_SIZE) / 2 + BACK_ICON_VISUAL_OFFSET_Y;
  const auto orientation = renderer.getOrientation();
  if (orientation == GfxRenderer::Orientation::LandscapeClockwise ||
      orientation == GfxRenderer::Orientation::LandscapeCounterClockwise) {
    renderer.drawImage(Back24Icon, iconX, iconY, BACK_ICON_SIZE, BACK_ICON_SIZE);
  } else {
    renderer.drawIcon(Back24Icon, iconX, iconY, BACK_ICON_SIZE, BACK_ICON_SIZE);
  }

  const int titleX = backRect.x + backRect.width + 8;
  const int titleMaxWidth = renderer.getScreenWidth() - titleX - metrics.contentSidePadding * 2 - metrics.batteryWidth;
  const auto headerTitle = renderer.truncatedText(UI_12_FONT_ID, title, titleMaxWidth, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, titleX, metrics.topPadding + metrics.batteryBarHeight + 3, headerTitle.c_str(), true,
                    EpdFontFamily::BOLD);
}

#ifdef CROSSPOINT_BOARD_MURPHY_M4
void drawCenteredPagerRow(const GfxRenderer& renderer, const Rect listBounds, const int visibleRow, const char* label) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int rowHeight = metrics.listRowHeight;
  const int rowX = listBounds.x + metrics.contentSidePadding;
  const int rowWidth = listBounds.width - metrics.contentSidePadding * 2 - 1;
  const int rowY = listBounds.y + visibleRow * rowHeight;
  const Rect rowRect{rowX, rowY + 2, rowWidth, rowHeight - 4};

  renderer.fillRect(rowRect.x + 1, rowRect.y + 1, rowRect.width - 2, rowRect.height - 2, false);
  renderer.drawRoundedRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height, 1, 6, true);

  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  const int textY = rowRect.y + (rowRect.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, rowRect.x + (rowRect.width - textWidth) / 2, textY, label, true,
                    EpdFontFamily::BOLD);
}
#endif
}  // namespace

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t orientation,
                                               const uint8_t writingModePreference, const bool hasFootnotes,
                                               const bool allowVerticalWritingMode,
                                               const bool epubFontOverrideActive, std::string epubFontName,
                                               const uint8_t epubFontSizePx)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes)),
      tabMenuItems(buildTabbedMenuItems(hasFootnotes)),
      title(title),
      pendingOrientation(normalizeOrientation(orientation)),
      allowVerticalWritingMode(allowVerticalWritingMode),
      pendingWritingModePreference(normalizeWritingModePreference(writingModePreference)),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent),
      epubFontOverrideActive(epubFontOverrideActive),
      epubFontName(std::move(epubFontName)),
      epubFontSizePx(epubFontSizePx) {}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes) {
  std::vector<MenuItem> items;
  items.reserve(13);
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  items.push_back({MenuAction::ADD_BOOKMARK, StrId::STR_ADD_BOOKMARK});
  items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::WRITING_MODE, StrId::STR_READING_LAYOUT});
  items.push_back({MenuAction::RUBY_OFFSET, StrId::STR_RUBY_OFFSET});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  return items;
}

std::vector<std::vector<EpubReaderMenuActivity::MenuItem>> EpubReaderMenuActivity::buildTabbedMenuItems(
    bool hasFootnotes) {
  std::vector<std::vector<MenuItem>> tabs(static_cast<int>(MenuTab::Count));

  auto& navigate = tabs[static_cast<int>(MenuTab::Navigate)];
  navigate.reserve(5);
  navigate.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  navigate.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  navigate.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  if (hasFootnotes) {
    navigate.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  navigate.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});

  auto& reading = tabs[static_cast<int>(MenuTab::Reading)];
  reading.reserve(5);
  reading.push_back({MenuAction::ADD_BOOKMARK, StrId::STR_ADD_BOOKMARK});
  reading.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  reading.push_back({MenuAction::WRITING_MODE, StrId::STR_READING_LAYOUT});
  reading.push_back({MenuAction::EPUB_FONT, StrId::STR_FONT_FAMILY});
  reading.push_back({MenuAction::EPUB_FONT_SIZE, StrId::STR_FONT_SIZE});
  reading.push_back({MenuAction::EPUB_FONT_GLOBAL, StrId::STR_DEFAULT_VALUE});
  reading.push_back({MenuAction::RUBY_OFFSET, StrId::STR_RUBY_OFFSET});
  reading.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});

  auto& tools = tabs[static_cast<int>(MenuTab::Tools)];
  tools.reserve(4);
  tools.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  tools.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  tools.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  tools.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});

  return tabs;
}

uint8_t EpubReaderMenuActivity::normalizeOrientation(const uint8_t orientation) const {
  return orientation < CrossPointSettings::ORIENTATION_COUNT ? orientation : CrossPointSettings::PORTRAIT;
}

uint8_t EpubReaderMenuActivity::normalizeWritingModePreference(const uint8_t writingModePreference) const {
  return writingModePreference < writingModeOptionCount() ? writingModePreference
                                                          : CrossPointSettings::WRITING_MODE_BOOK_DEFAULT;
}

uint8_t EpubReaderMenuActivity::writingModeOptionCount() const {
  return allowVerticalWritingMode ? CrossPointSettings::WRITING_MODE_PREFERENCE_COUNT
                                  : CrossPointSettings::WRITING_MODE_VERTICAL_RL;
}

const std::vector<EpubReaderMenuActivity::MenuItem>& EpubReaderMenuActivity::activeMenuItems() const {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  return tabMenuItems[std::clamp(selectedTabIndex, 0, static_cast<int>(MenuTab::Count) - 1)];
#else
  return menuItems;
#endif
}

Rect EpubReaderMenuActivity::screenRect() const {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  return UITheme::getInstance().getScreenSafeArea(renderer, false, false);
#else
  return UITheme::getInstance().getScreenSafeArea(renderer, true, false);
#endif
}

Rect EpubReaderMenuActivity::contentRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = screenRect();
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight * 2 +
                         metrics.verticalSpacing;
  const int contentBottom = renderer.getScreenHeight() - metrics.verticalSpacing;
#else
  const int contentTop =
      screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
#endif
  return Rect{screen.x, contentTop, screen.width, std::max(0, contentBottom - contentTop)};
}

Rect EpubReaderMenuActivity::headerBackRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = screenRect();
  return Rect{screen.x, screen.y + metrics.topPadding, std::min(screen.width, 120), metrics.headerHeight};
}

Rect EpubReaderMenuActivity::backButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = screenRect();
  return Rect{screen.x + 8, screen.y + metrics.topPadding + metrics.batteryBarHeight - 10, 48, 48};
}

Rect EpubReaderMenuActivity::progressRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = screenRect();
  return Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight};
}

Rect EpubReaderMenuActivity::tabBarRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = screenRect();
  return Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight, screen.width,
              metrics.tabBarHeight};
}

Rect EpubReaderMenuActivity::footerHintsRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, renderer.getScreenHeight() - metrics.buttonHintsHeight, renderer.getScreenWidth(),
              metrics.buttonHintsHeight};
}

int EpubReaderMenuActivity::listPageItems() const {
  return std::max(1, contentRect().height / UITheme::getInstance().getMetrics().listRowHeight);
}

int EpubReaderMenuActivity::currentListPageStart() const {
  return calculateMenuPageLayout(selectedIndex, static_cast<int>(activeMenuItems().size()), listPageItems()).start;
}

int EpubReaderMenuActivity::visibleMenuItemCount() const {
  return calculateMenuPageLayout(selectedIndex, static_cast<int>(activeMenuItems().size()), listPageItems()).itemCount;
}

bool EpubReaderMenuActivity::hasPreviousListPage() const {
  return calculateMenuPageLayout(selectedIndex, static_cast<int>(activeMenuItems().size()), listPageItems()).previous;
}

bool EpubReaderMenuActivity::hasNextListPage() const {
  return calculateMenuPageLayout(selectedIndex, static_cast<int>(activeMenuItems().size()), listPageItems()).next;
}

int EpubReaderMenuActivity::visibleListRowCount() const {
  return visibleMenuItemCount() + (hasPreviousListPage() ? 1 : 0) + (hasNextListPage() ? 1 : 0);
}

bool EpubReaderMenuActivity::isPreviousPageRow(const int visibleRow) const {
  return hasPreviousListPage() && visibleRow == 0;
}

bool EpubReaderMenuActivity::isNextPageRow(const int visibleRow) const {
  return hasNextListPage() && visibleRow == visibleListRowCount() - 1;
}

int EpubReaderMenuActivity::visibleRowToMenuIndex(const int visibleRow) const {
  if (visibleRow < 0 || isPreviousPageRow(visibleRow) || isNextPageRow(visibleRow)) {
    return -1;
  }

  const int itemOffset = visibleRow - (hasPreviousListPage() ? 1 : 0);
  if (itemOffset < 0 || itemOffset >= visibleMenuItemCount()) {
    return -1;
  }
  return currentListPageStart() + itemOffset;
}

void EpubReaderMenuActivity::selectTab(const int tabIndex) {
  selectedTabIndex = std::clamp(tabIndex, 0, static_cast<int>(MenuTab::Count) - 1);
  selectedIndex = 0;
}

void EpubReaderMenuActivity::activateSelectedAction() {
  const auto& items = activeMenuItems();
  if (items.empty()) {
    return;
  }
  selectedIndex = std::clamp(selectedIndex, 0, static_cast<int>(items.size()) - 1);
  const auto selectedAction = items[selectedIndex].action;
  if (selectedAction == MenuAction::ROTATE_SCREEN) {
    // Cycle the pending orientation; the reader applies the renderer change when the menu closes.
    pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
    selectedPageTurnOption = (selectedPageTurnOption + 1) % pageTurnLabels.size();
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::WRITING_MODE) {
    pendingWritingModePreference = (pendingWritingModePreference + 1) % writingModeOptionCount();
    requestUpdate();
    return;
  }

  setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, pendingWritingModePreference,
                       selectedPageTurnOption});
  finish();
}

void EpubReaderMenuActivity::cancelMenu() {
  ActivityResult result;
  result.isCancelled = true;
  result.data = MenuResult{-1, pendingOrientation, pendingWritingModePreference, selectedPageTurnOption};
  setResult(std::move(result));
  finish();
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
  if (handleTouch()) {
    return;
  }

  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(activeMenuItems().size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(activeMenuItems().size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelectedAction();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancelMenu();
    return;
  }
}

bool EpubReaderMenuActivity::handleTouch() {
  if (!mappedInput.wasTapped()) {
    return false;
  }

  if (TouchNavigator::wasTappedIn(mappedInput, headerBackRect())) {
    cancelMenu();
    return true;
  }

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const int tabIndex = TouchNavigator::tappedEqualTabIndex(mappedInput, tabBarRect(), static_cast<int>(MenuTab::Count));
  if (tabIndex >= 0) {
    selectTab(tabIndex);
    requestUpdate();
    return true;
  }

  const int visibleRow = TouchNavigator::tappedListIndex(mappedInput, contentRect(), visibleListRowCount(), 0,
                                                        UITheme::getInstance().getMetrics().listRowHeight, 0);
  if (visibleRow >= 0) {
    if (isPreviousPageRow(visibleRow)) {
      const auto previousPage =
          calculateMenuPageLayout(std::max(0, currentListPageStart() - 1), static_cast<int>(activeMenuItems().size()),
                                  listPageItems());
      selectedIndex = previousPage.start;
      requestUpdate();
      return true;
    }

    if (isNextPageRow(visibleRow)) {
      selectedIndex = std::min(static_cast<int>(activeMenuItems().size()) - 1,
                               currentListPageStart() + visibleMenuItemCount());
      requestUpdate();
      return true;
    }

    const int menuIndex = visibleRowToMenuIndex(visibleRow);
    if (menuIndex >= 0) {
      selectedIndex = menuIndex;
      activateSelectedAction();
      return true;
    }
  }

  return true;
#else
  const int footerIndex = TouchNavigator::tappedGridIndex(mappedInput, footerHintsRect(), 4, 4);
  if (footerIndex == 0) {
    cancelMenu();
    return true;
  }
  if (footerIndex == 1) {
    activateSelectedAction();
    return true;
  }
  if (footerIndex == 2) {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
    return true;
  }
  if (footerIndex == 3) {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
    return true;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int tappedIndex = TouchNavigator::tappedListIndex(mappedInput, contentRect(), static_cast<int>(menuItems.size()),
                                                          selectedIndex, metrics.listRowHeight, 0);
  if (tappedIndex >= 0) {
    selectedIndex = tappedIndex;
    activateSelectedAction();
    return true;
  }

  return true;
#endif
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = screenRect();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight}, "");
  drawReaderMenuHeaderTitle(renderer, backButtonRect(), title.c_str());
#else
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str());
#endif

  // Progress summary
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  GUI.drawSubHeader(renderer, progressRect(), progressLine.c_str());

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  std::vector<TabInfo> tabs;
  tabs.reserve(static_cast<int>(MenuTab::Count));
  tabs.push_back({"Navigate", selectedTabIndex == static_cast<int>(MenuTab::Navigate)});
  tabs.push_back({"Reading", selectedTabIndex == static_cast<int>(MenuTab::Reading)});
  tabs.push_back({"Tools", selectedTabIndex == static_cast<int>(MenuTab::Tools)});
  GUI.drawTabBar(renderer, tabBarRect(), tabs, false);
#endif

  const Rect listRect = contentRect();
  const auto& items = activeMenuItems();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const int visibleSelected = (hasPreviousListPage() ? 1 : 0) + (selectedIndex - currentListPageStart());
  GUI.drawList(
      renderer, listRect, visibleListRowCount(), visibleSelected,
      [this, &items](int visibleRow) {
        if (isPreviousPageRow(visibleRow)) {
          return std::string(tr(STR_PREV_PAGE));
        }
        if (isNextPageRow(visibleRow)) {
          return std::string(tr(STR_NEXT_PAGE));
        }

        const int menuIndex = visibleRowToMenuIndex(visibleRow);
        if (menuIndex >= 0) {
          return std::string(I18N.get(items[menuIndex].labelId));
        }
        return std::string();
      },
      nullptr, nullptr,
      [this, &items](int visibleRow) -> std::string {
        const int menuIndex = visibleRowToMenuIndex(visibleRow);
        if (menuIndex < 0) {
          return "";
        }

        const auto value = items[menuIndex].action;
        if (value == MenuAction::ROTATE_SCREEN) {
          return std::string(I18N.get(orientationLabels[normalizeOrientation(pendingOrientation)]));
        } else if (value == MenuAction::WRITING_MODE) {
          return std::string(I18N.get(writingModeLabels[normalizeWritingModePreference(pendingWritingModePreference)]));
        } else if (value == MenuAction::AUTO_PAGE_TURN) {
          return std::string(pageTurnLabels[selectedPageTurnOption]);
        } else if (value == MenuAction::EPUB_FONT) {
          return epubFontOverrideActive && !epubFontName.empty() ? epubFontName : std::string(tr(STR_DEFAULT_VALUE));
        } else if (value == MenuAction::EPUB_FONT_SIZE) {
          return epubFontOverrideActive && epubFontSizePx > 0 ? std::to_string(epubFontSizePx) + " px"
                                                              : std::string(tr(STR_DEFAULT_VALUE));
        } else if (value == MenuAction::EPUB_FONT_GLOBAL) {
          return epubFontOverrideActive ? std::string(tr(STR_STATE_ON)) : std::string(tr(STR_SELECTED));
        }
        return "";
      },
      true);

  if (hasPreviousListPage()) {
    drawCenteredPagerRow(renderer, listRect, 0, tr(STR_PREV_PAGE));
  }
  if (hasNextListPage()) {
    drawCenteredPagerRow(renderer, listRect, visibleListRowCount() - 1, tr(STR_NEXT_PAGE));
  }
#else
  GUI.drawList(
      renderer, listRect, items.size(), selectedIndex, [&items](int index) { return I18N.get(items[index].labelId); },
      nullptr, nullptr,
      [this, &items](int index) -> std::string {
        const auto value = items[index].action;
        if (value == MenuAction::ROTATE_SCREEN) {
          return std::string(I18N.get(orientationLabels[normalizeOrientation(pendingOrientation)]));
        } else if (value == MenuAction::WRITING_MODE) {
          return std::string(I18N.get(writingModeLabels[normalizeWritingModePreference(pendingWritingModePreference)]));
        } else if (value == MenuAction::AUTO_PAGE_TURN) {
          return std::string(pageTurnLabels[selectedPageTurnOption]);
        } else if (value == MenuAction::EPUB_FONT) {
          return epubFontOverrideActive && !epubFontName.empty() ? epubFontName : std::string(tr(STR_DEFAULT_VALUE));
        } else if (value == MenuAction::EPUB_FONT_SIZE) {
          return epubFontOverrideActive && epubFontSizePx > 0 ? std::to_string(epubFontSizePx) + " px"
                                                              : std::string(tr(STR_DEFAULT_VALUE));
        } else if (value == MenuAction::EPUB_FONT_GLOBAL) {
          return epubFontOverrideActive ? std::string(tr(STR_STATE_ON)) : std::string(tr(STR_SELECTED));
        } else {
          return "";
        }
      },
      true);

  // Footer / Hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

  renderer.displayBuffer();
}
