#include "LanguageSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "fontIds.h"
#include "util/TouchList.h"
#include "util/TouchNavigator.h"
#include "util/TouchUi.h"

void LanguageSelectActivity::onEnter() {
  Activity::onEnter();

  // Set current selection based on current language
  const auto currentLang = static_cast<uint8_t>(I18N.getLanguage());
  const auto* begin = std::begin(SORTED_LANGUAGE_INDICES);
  const auto* end = std::end(SORTED_LANGUAGE_INDICES);
  const auto* it = std::find(begin, end, currentLang);
  selectedIndex = (it != end) ? std::distance(begin, it) : 0;

  requestUpdate();
}

void LanguageSelectActivity::onExit() { Activity::onExit(); }

void LanguageSelectActivity::loop() {
  if (handleTouch()) {
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(static_cast<int>(selectedIndex), totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(static_cast<int>(selectedIndex), totalItems);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectedIndex), totalItems, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectedIndex), totalItems, pageItems);
    requestUpdate();
  });
}

void LanguageSelectActivity::handleSelection() {
  const uint8_t langIndex = SORTED_LANGUAGE_INDICES[selectedIndex];

  {
    RenderLock lock(*this);
    I18N.setLanguage(static_cast<Language>(langIndex));
  }

  SETTINGS.language = langIndex;
  SETTINGS.saveToFile();

  // Return to previous page
  onBack();
}

bool LanguageSelectActivity::handleTouch() {
#ifndef CROSSPOINT_BOARD_MURPHY_M4
  return false;
#else
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
    onBack();
    return true;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const Rect listBounds{screen.x, contentTop, screen.width, renderer.getScreenHeight() - contentTop};
  const int listRows = std::max(1, listBounds.height / metrics.listRowHeight);
  const auto layout = TouchList::calculatePageLayout(selectedIndex, totalItems, listRows);
  const int visibleRow = TouchNavigator::tappedListIndex(mappedInput, listBounds, TouchList::visibleRowCount(layout),
                                                        0, metrics.listRowHeight, 0);
  if (visibleRow < 0) {
    return mappedInput.wasTapped();
  }

  if (TouchList::isPreviousPageRow(layout, visibleRow)) {
    selectedIndex = TouchList::calculatePageLayout(std::max(0, layout.start - 1), totalItems, listRows).start;
    requestUpdate();
    return true;
  }
  if (TouchList::isNextPageRow(layout, visibleRow)) {
    selectedIndex = std::min(totalItems - 1, layout.start + layout.itemCount);
    requestUpdate();
    return true;
  }

  const int itemIndex = TouchList::visibleRowToItemIndex(layout, visibleRow);
  if (itemIndex >= 0) {
    selectedIndex = itemIndex;
    handleSelection();
  }
  return true;
#endif
}

void LanguageSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  TouchUi::drawHeaderWithBack(renderer, screen, tr(STR_LANGUAGE));
#else
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LANGUAGE));
#endif

  // Current language marker
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
#ifdef CROSSPOINT_BOARD_MURPHY_M4
      pageHeight - contentTop;
#else
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
#endif
  const auto currentLang = static_cast<uint8_t>(I18N.getLanguage());
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const int listRows = std::max(1, contentHeight / metrics.listRowHeight);
  const auto layout = TouchList::calculatePageLayout(selectedIndex, totalItems, listRows);
  const Rect listBounds{0, contentTop, pageWidth, contentHeight};
  const int visibleSelected = layout.previous + selectedIndex - layout.start;
  GUI.drawList(
      renderer, listBounds, TouchList::visibleRowCount(layout), visibleSelected,
      [layout](int visibleRow) {
        if (TouchList::isPreviousPageRow(layout, visibleRow)) return std::string(tr(STR_PREV_PAGE));
        if (TouchList::isNextPageRow(layout, visibleRow)) return std::string(tr(STR_NEXT_PAGE));
        const int index = TouchList::visibleRowToItemIndex(layout, visibleRow);
        return std::string(I18N.getLanguageName(static_cast<Language>(SORTED_LANGUAGE_INDICES[index])));
      },
      nullptr, nullptr,
      [layout, currentLang](int visibleRow) {
        const int index = TouchList::visibleRowToItemIndex(layout, visibleRow);
        return index >= 0 && SORTED_LANGUAGE_INDICES[index] == currentLang ? tr(STR_SELECTED) : "";
      },
      true);
  if (layout.previous) TouchUi::drawCenteredPagerRow(renderer, listBounds, 0, tr(STR_PREV_PAGE));
  if (layout.next) {
    TouchUi::drawCenteredPagerRow(renderer, listBounds, TouchList::visibleRowCount(layout) - 1, tr(STR_NEXT_PAGE));
  }
#else
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalItems, selectedIndex,
      [this](int index) { return I18N.getLanguageName(static_cast<Language>(SORTED_LANGUAGE_INDICES[index])); },
      nullptr, nullptr,
      [this, currentLang](int index) { return SORTED_LANGUAGE_INDICES[index] == currentLang ? tr(STR_SELECTED) : ""; },
      true);

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

  renderer.displayBuffer();
}
