#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchList.h"
#include "util/TouchNavigator.h"
#include "util/TouchUi.h"

int EpubReaderChapterSelectionActivity::getTotalItems() const { return static_cast<int>(navigationEntries.size()); }

void EpubReaderChapterSelectionActivity::buildNavigationEntries() {
  navigationEntries = EpubReaderNavigation::buildEntries(epub);
}

void EpubReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();
  previousRendererOrientation = renderer.getOrientation();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  if (!epub) {
    return;
  }

  buildNavigationEntries();
  selectorIndex = 0;
  for (int i = 0; i < static_cast<int>(navigationEntries.size()); i++) {
    if (navigationEntries[i].spineIndex == currentSpineIndex) {
      selectorIndex = i;
      break;
    }
  }
  if (selectorIndex == 0 && !navigationEntries.empty() && navigationEntries[0].spineIndex != currentSpineIndex) {
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    for (int i = 0; i < static_cast<int>(navigationEntries.size()); i++) {
      if (navigationEntries[i].spineIndex >= 0 &&
          epub->getTocIndexForSpineIndex(navigationEntries[i].spineIndex) == tocIndex) {
        selectorIndex = i;
        break;
      }
    }
  }

  // Trigger first update
  requestUpdate();
}

void EpubReaderChapterSelectionActivity::onExit() {
  renderer.setOrientation(previousRendererOrientation);
  Activity::onExit();
}

Rect EpubReaderChapterSelectionActivity::contentRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = renderer.getScreenHeight() - metrics.verticalSpacing;
#else
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = screen.height - metrics.verticalSpacing;
#endif
  return Rect{screen.x, contentTop, screen.width, std::max(0, contentBottom - contentTop)};
}

void EpubReaderChapterSelectionActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void EpubReaderChapterSelectionActivity::selectCurrent() {
  const auto entry = (selectorIndex >= 0 && selectorIndex < static_cast<int>(navigationEntries.size()))
                         ? navigationEntries[selectorIndex]
                         : EpubReaderNavigation::Entry{};
  if (entry.spineIndex == -1) {
    cancel();
    return;
  }

  setResult(ChapterResult{entry.spineIndex, entry.anchor});
  finish();
}

bool EpubReaderChapterSelectionActivity::handleTouch() {
#ifndef CROSSPOINT_BOARD_MURPHY_M4
  return false;
#else
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
    cancel();
    return true;
  }

  const Rect listBounds = contentRect();
  const int rowHeight = UITheme::getInstance().getMetrics().listRowHeight;
  const int listRows = std::max(1, listBounds.height / rowHeight);
  const auto layout = TouchList::calculatePageLayout(selectorIndex, getTotalItems(), listRows);
  const int visibleRow =
      TouchNavigator::tappedListIndex(mappedInput, listBounds, TouchList::visibleRowCount(layout), 0, rowHeight, 0);
  if (visibleRow < 0) {
    return mappedInput.wasTapped();
  }

  if (TouchList::isPreviousPageRow(layout, visibleRow)) {
    selectorIndex = TouchList::calculatePageLayout(std::max(0, layout.start - 1), getTotalItems(), listRows).start;
    requestUpdate();
    return true;
  }

  if (TouchList::isNextPageRow(layout, visibleRow)) {
    selectorIndex = std::min(getTotalItems() - 1, layout.start + layout.itemCount);
    requestUpdate();
    return true;
  }

  const int itemIndex = TouchList::visibleRowToItemIndex(layout, visibleRow);
  if (itemIndex >= 0) {
    selectorIndex = itemIndex;
    selectCurrent();
    return true;
  }

  return true;
#endif
}

void EpubReaderChapterSelectionActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);
  const int totalItems = getTotalItems();

  if (handleTouch()) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    selectCurrent();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
  }

  buttonNavigator.onNextRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });
}

void EpubReaderChapterSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen =
#ifdef CROSSPOINT_BOARD_MURPHY_M4
      UITheme::getInstance().getScreenSafeArea(renderer, false, false);
#else
      UITheme::getInstance().getScreenSafeArea(renderer, true, false);
#endif

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  TouchUi::drawHeaderWithBack(renderer, screen, tr(STR_SELECT_CHAPTER));
#else
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_SELECT_CHAPTER));
#endif

  const Rect listBounds = contentRect();
  const int totalItems = getTotalItems();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const int listRows = std::max(1, listBounds.height / metrics.listRowHeight);
  const auto layout = TouchList::calculatePageLayout(selectorIndex, totalItems, listRows);
  GUI.drawList(
      renderer, listBounds, TouchList::visibleRowCount(layout), -1,
      [this, layout](int visibleRow) {
        if (TouchList::isPreviousPageRow(layout, visibleRow)) {
          return std::string(tr(STR_PREV_PAGE));
        }
        if (TouchList::isNextPageRow(layout, visibleRow)) {
          return std::string(tr(STR_NEXT_PAGE));
        }

        const int itemIndex = TouchList::visibleRowToItemIndex(layout, visibleRow);
        if (itemIndex < 0) {
          return std::string();
        }
        const auto& item = navigationEntries[itemIndex];
        std::string indent((item.level - 1) * 2, ' ');
        return indent + item.title;
      },
      nullptr, nullptr, nullptr, true);
  if (layout.previous) {
    TouchUi::drawCenteredPagerRow(renderer, listBounds, 0, tr(STR_PREV_PAGE));
  }
  if (layout.next) {
    TouchUi::drawCenteredPagerRow(renderer, listBounds, TouchList::visibleRowCount(layout) - 1, tr(STR_NEXT_PAGE));
  }
#else
  GUI.drawList(renderer, listBounds, totalItems, selectorIndex, [this](int index) {
    const auto& item = navigationEntries[index];
    std::string indent((item.level - 1) * 2, ' ');
    return indent + item.title;
  });
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

  renderer.displayBuffer();
}
