#include "RecentBooksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/icons/back24.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
// Hold threshold for the long-press "remove from list" action (firmware convention).
constexpr unsigned long LONG_PRESS_MS = 1000;
constexpr int BACK_ICON_SIZE = 24;
constexpr int BACK_ICON_VISUAL_OFFSET_Y = 5;
}  // namespace

void RecentBooksActivity::loadRecentBooks() { recentBooks = RECENT_BOOKS.getBooks(); }

Rect RecentBooksActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const int contentHeight = pageHeight - contentTop - metrics.verticalSpacing;
#else
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
#endif
  return Rect{0, contentTop, pageWidth, contentHeight};
}

Rect RecentBooksActivity::headerBackTapRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, 0, renderer.getScreenWidth() / 3, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing};
}

Rect RecentBooksActivity::backButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{8, metrics.topPadding + metrics.batteryBarHeight - 10, 48, 48};
}

int RecentBooksActivity::listIndexForPoint(const MappedInputManager::TouchPoint point) const {
  const Rect rect = listRect();
  const int rowHeight = UITheme::getInstance().getMetrics().listWithSubtitleRowHeight;
  if (recentBooks.empty() || rowHeight <= 0 || !TouchNavigator::contains(rect, point)) {
    return -1;
  }

  const int visibleRows = std::max(1, rect.height / rowHeight);
  const int pageStartIndex = (static_cast<int>(selectorIndex) / visibleRows) * visibleRows;
  const int row = (point.y - rect.y) / rowHeight;
  const int index = pageStartIndex + row;
  return index < static_cast<int>(recentBooks.size()) ? index : -1;
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Prune entries whose backing files are gone; this is one of two interaction
  // points where the persistent store gets cleaned (the other is addBook).
  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
}

bool RecentBooksActivity::handleTouch() {
  if (!mappedInput.wasTapped() && !mappedInput.wasTouchLongPressed()) {
    return false;
  }

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  if (mappedInput.wasTouchLongPressed()) {
    const int longPressedIndex = listIndexForPoint(mappedInput.lastTouchLongPress());
    if (longPressedIndex >= 0) {
      selectorIndex = static_cast<size_t>(longPressedIndex);
      promptRemoveBook(recentBooks[selectorIndex].path, recentBooks[selectorIndex].title);
    }
    return true;
  }

  if (TouchNavigator::wasTappedIn(mappedInput, headerBackTapRect())) {
    onGoHome();
    return true;
  }
#endif

  const int tappedIndex = TouchNavigator::tappedListIndex(mappedInput, listRect(), static_cast<int>(recentBooks.size()),
                                                          static_cast<int>(selectorIndex),
                                                          UITheme::getInstance().getMetrics().listWithSubtitleRowHeight,
                                                          0);
  if (tappedIndex >= 0) {
    selectorIndex = static_cast<size_t>(tappedIndex);
    LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
    onSelectBook(recentBooks[selectorIndex].path);
    return true;
  }

  return false;
}

void RecentBooksActivity::loop() {
  if (handleTouch()) {
    return;
  }

  const int pageItems = std::max(1, listRect().height / UITheme::getInstance().getMetrics().listWithSubtitleRowHeight);

  // After a long-press has fired, swallow input until Confirm is physically released
  // (so the release doesn't also open the book; re-arm only once the button is up).
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return;
  }

  // Long-press Confirm on the selected book: prompt to remove it from the list.
  // Fires when the hold times out while still held (firmware hold-to-act pattern,
  // cf. FileBrowserActivity BACK long-press).
  if (!recentBooks.empty() && selectorIndex < recentBooks.size() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    promptRemoveBook(recentBooks[selectorIndex].path, recentBooks[selectorIndex].title);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex < static_cast<int>(recentBooks.size())) {
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  }

  int listSize = static_cast<int>(recentBooks.size());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBA", "Remove from recents cancelled");
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      LOG_DBG("RBA", "Removed from recents: %s", path.c_str());
      loadRecentBooks();
      if (recentBooks.empty()) {
        selectorIndex = 0;
      } else if (selectorIndex >= recentBooks.size()) {
        selectorIndex = recentBooks.size() - 1;
      }
      requestUpdate(true);
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
      std::move(handler));
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "");
  {
    const Rect backRect = backButtonRect();
    renderer.drawIcon(Back24Icon, backRect.x + (backRect.width - BACK_ICON_SIZE) / 2,
                      backRect.y + (backRect.height - BACK_ICON_SIZE) / 2 + BACK_ICON_VISUAL_OFFSET_Y, BACK_ICON_SIZE,
                      BACK_ICON_SIZE);

    const int titleX = backRect.x + backRect.width + 8;
    const int titleMaxWidth = pageWidth - titleX - metrics.contentSidePadding * 2 - metrics.batteryWidth;
    const auto title =
        renderer.truncatedText(UI_12_FONT_ID, tr(STR_MENU_RECENT_BOOKS), titleMaxWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, titleX, metrics.topPadding + metrics.batteryBarHeight + 3, title.c_str(), true,
                      EpdFontFamily::BOLD);
  }
#else
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MENU_RECENT_BOOKS));
#endif

  const Rect listBounds = listRect();

  // Recent tab
  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, listBounds.y + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    GUI.drawList(
        renderer, listBounds, recentBooks.size(), selectorIndex,
        [this](int index) { return StringUtils::uiSafeBookTitle(recentBooks[index].title, recentBooks[index].path); },
        [this](int index) { return StringUtils::uiSafeAuthor(recentBooks[index].author); },
        [this](int index) { return UITheme::getFileIcon(recentBooks[index].path); });
  }

#ifndef CROSSPOINT_BOARD_MURPHY_M4
  // Help text
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

  renderer.displayBuffer();
}
