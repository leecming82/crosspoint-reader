#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchNavigator.h"
#include "util/TouchUi.h"

namespace {
constexpr int MENU_ITEM_COUNT = 3;
}  // namespace

void NetworkModeSelectionActivity::onEnter() {
  Activity::onEnter();

  // Reset selection
  selectedIndex = 0;

  // Trigger first update
  requestUpdate();
}

void NetworkModeSelectionActivity::onExit() { Activity::onExit(); }

void NetworkModeSelectionActivity::loop() {
  if (handleTouch()) {
    return;
  }

  // Handle back button - cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
    return;
  }

  // Handle confirm button - select current option
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    NetworkMode mode = NetworkMode::JOIN_NETWORK;
    if (selectedIndex == 1) {
      mode = NetworkMode::CONNECT_CALIBRE;
    } else if (selectedIndex == 2) {
      mode = NetworkMode::CREATE_HOTSPOT;
    }
    onModeSelected(mode);
    return;
  }

  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, MENU_ITEM_COUNT);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, MENU_ITEM_COUNT);
    requestUpdate();
  });
}

bool NetworkModeSelectionActivity::handleTouch() {
#ifndef CROSSPOINT_BOARD_MURPHY_M4
  return false;
#else
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
    onCancel();
    return true;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const Rect listBounds{screen.x, contentTop, screen.width, MENU_ITEM_COUNT * metrics.listRowHeight};
  const int tappedIndex =
      TouchNavigator::tappedListIndex(mappedInput, listBounds, MENU_ITEM_COUNT, 0, metrics.listRowHeight, 0);
  if (tappedIndex < 0) {
    return mappedInput.wasTapped();
  }

  selectedIndex = tappedIndex;
  NetworkMode mode = NetworkMode::JOIN_NETWORK;
  if (selectedIndex == 1) {
    mode = NetworkMode::CONNECT_CALIBRE;
  } else if (selectedIndex == 2) {
    mode = NetworkMode::CREATE_HOTSPOT;
  }
  onModeSelected(mode);
  return true;
#endif
}

void NetworkModeSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  TouchUi::drawHeaderWithBack(renderer, screen, tr(STR_FILE_TRANSFER));
#else
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FILE_TRANSFER));
#endif

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
#ifdef CROSSPOINT_BOARD_MURPHY_M4
      pageHeight - contentTop;
#else
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
#endif
  // Menu items and descriptions
  static constexpr StrId menuItems[MENU_ITEM_COUNT] = {StrId::STR_JOIN_NETWORK, StrId::STR_CALIBRE_WIRELESS,
                                                       StrId::STR_CREATE_HOTSPOT};
  static constexpr StrId menuDescs[MENU_ITEM_COUNT] = {StrId::STR_JOIN_DESC, StrId::STR_CALIBRE_DESC,
                                                       StrId::STR_HOTSPOT_DESC};
  static constexpr UIIcon menuIcons[MENU_ITEM_COUNT] = {UIIcon::Wifi, UIIcon::Library, UIIcon::Hotspot};

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEM_COUNT), selectedIndex,
      [](int index) { return std::string(I18N.get(menuItems[index])); },
      [](int index) { return std::string(I18N.get(menuDescs[index])); }, [](int index) { return menuIcons[index]; });

#ifndef CROSSPOINT_BOARD_MURPHY_M4
  // Draw help text at bottom
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

  renderer.displayBuffer();
}

void NetworkModeSelectionActivity::onModeSelected(NetworkMode mode) {
  setResult(NetworkModeResult{mode});
  finish();
}

void NetworkModeSelectionActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
