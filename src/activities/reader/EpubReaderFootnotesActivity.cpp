#include "EpubReaderFootnotesActivity.h"

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

void EpubReaderFootnotesActivity::onEnter() {
  Activity::onEnter();
  previousRendererOrientation = renderer.getOrientation();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  selectedIndex = 0;
  requestUpdate();
}

void EpubReaderFootnotesActivity::onExit() {
  renderer.setOrientation(previousRendererOrientation);
  Activity::onExit();
}

Rect EpubReaderFootnotesActivity::contentRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = renderer.getScreenHeight() - metrics.verticalSpacing;
  return Rect{screen.x, contentTop, screen.width, std::max(0, contentBottom - contentTop)};
#else
  return Rect{0, 60, renderer.getScreenWidth(), renderer.getScreenHeight() - 60};
#endif
}

void EpubReaderFootnotesActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void EpubReaderFootnotesActivity::selectCurrent() {
  if (selectedIndex >= 0 && selectedIndex < static_cast<int>(footnotes.size())) {
    setResult(FootnoteResult{footnotes[selectedIndex].href});
    finish();
  }
}

bool EpubReaderFootnotesActivity::handleTouch() {
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
  const auto layout = TouchList::calculatePageLayout(selectedIndex, static_cast<int>(footnotes.size()), listRows);
  const int visibleRow =
      TouchNavigator::tappedListIndex(mappedInput, listBounds, TouchList::visibleRowCount(layout), 0, rowHeight, 0);
  if (visibleRow < 0) {
    return mappedInput.wasTapped();
  }

  if (TouchList::isPreviousPageRow(layout, visibleRow)) {
    selectedIndex =
        TouchList::calculatePageLayout(std::max(0, layout.start - 1), static_cast<int>(footnotes.size()), listRows)
            .start;
    requestUpdate();
    return true;
  }
  if (TouchList::isNextPageRow(layout, visibleRow)) {
    selectedIndex = std::min(static_cast<int>(footnotes.size()) - 1, layout.start + layout.itemCount);
    requestUpdate();
    return true;
  }

  const int itemIndex = TouchList::visibleRowToItemIndex(layout, visibleRow);
  if (itemIndex >= 0) {
    selectedIndex = itemIndex;
    selectCurrent();
    return true;
  }

  return true;
#endif
}

void EpubReaderFootnotesActivity::loop() {
  if (handleTouch()) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    selectCurrent();
    return;
  }

  buttonNavigator.onNext([this] {
    if (!footnotes.empty()) {
      selectedIndex = (selectedIndex + 1) % footnotes.size();
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (!footnotes.empty()) {
      selectedIndex = (selectedIndex - 1 + footnotes.size()) % footnotes.size();
      requestUpdate();
    }
  });
}

void EpubReaderFootnotesActivity::render(RenderLock&&) {
  renderer.clearScreen();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  TouchUi::drawHeaderWithBack(renderer, screen, tr(STR_FOOTNOTES));

  if (footnotes.empty()) {
    GUI.drawHelpText(renderer, contentRect(), tr(STR_NO_FOOTNOTES));
    renderer.displayBuffer();
    return;
  }

  const Rect listBounds = contentRect();
  const int rowHeight = metrics.listRowHeight;
  const int listRows = std::max(1, listBounds.height / rowHeight);
  const auto layout = TouchList::calculatePageLayout(selectedIndex, static_cast<int>(footnotes.size()), listRows);
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
        std::string label = footnotes[itemIndex].number;
        return label.empty() ? std::string(tr(STR_LINK)) : label;
      },
      nullptr, nullptr, nullptr, true);
  if (layout.previous) {
    TouchUi::drawCenteredPagerRow(renderer, listBounds, 0, tr(STR_PREV_PAGE));
  }
  if (layout.next) {
    TouchUi::drawCenteredPagerRow(renderer, listBounds, TouchList::visibleRowCount(layout) - 1, tr(STR_NEXT_PAGE));
  }

  renderer.displayBuffer();
  return;
#endif

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  // Landscape orientation: reserve a horizontal gutter for button hints.
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Inverted portrait: reserve vertical space for hints at the top.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  // Manual centering to honor content gutters.
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_FOOTNOTES), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_FOOTNOTES), true, EpdFontFamily::BOLD);

  if (footnotes.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 90 + contentY, tr(STR_NO_FOOTNOTES));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  constexpr int lineHeight = 36;
  const int screenWidth = renderer.getScreenWidth();
  const int marginLeft = contentX + 20;

  const int visibleCount = std::max(1, (renderer.getScreenHeight() - contentY) / lineHeight);
  if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
  if (selectedIndex >= scrollOffset + visibleCount) scrollOffset = selectedIndex - visibleCount + 1;

  for (int i = scrollOffset; i < static_cast<int>(footnotes.size()) && i < scrollOffset + visibleCount; i++) {
    const int y = 60 + contentY + (i - scrollOffset) * lineHeight;
    const bool isSelected = (i == selectedIndex);

    if (isSelected) {
      renderer.fillRect(0, y, screenWidth, lineHeight, true);
    }

    // Show footnote number and abbreviated href
    std::string label = footnotes[i].number;
    if (label.empty()) {
      label = tr(STR_LINK);
    }
    renderer.drawText(UI_10_FONT_ID, marginLeft, y + 4, label.c_str(), !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
