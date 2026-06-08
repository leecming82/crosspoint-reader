#include "OpdsServerListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "OpdsSettingsActivity.h"
#include "activities/ActivityManager.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchList.h"
#include "util/TouchNavigator.h"
#include "util/TouchUi.h"

int OpdsServerListActivity::getItemCount() const {
  int count = static_cast<int>(OPDS_STORE.getCount());
  // In settings mode, append a virtual "Add Server" item; in picker mode, only show real servers
  if (!pickerMode) {
    count++;
  }
  return count;
}

void OpdsServerListActivity::onEnter() {
  Activity::onEnter();

  // Reload from disk in case servers were added/removed by a subactivity or the web UI
  OPDS_STORE.loadFromFile();
  selectedIndex = 0;
  requestUpdate();
}

void OpdsServerListActivity::onExit() { Activity::onExit(); }

void OpdsServerListActivity::loop() {
  if (handleTouch()) {
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (pickerMode) {
      activityManager.goHome(HomeMenuItem::OPDS_BROWSER);
    } else {
      finish();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int itemCount = getItemCount();
  if (itemCount > 0) {
    buttonNavigator.onNext([this, itemCount] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, itemCount] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
      requestUpdate();
    });
  }
}

void OpdsServerListActivity::handleSelection() {
  const auto serverCount = static_cast<int>(OPDS_STORE.getCount());

  if (pickerMode) {
    // Picker mode: selecting a server navigates to the OPDS browser
    if (selectedIndex < serverCount) {
      const auto* server = OPDS_STORE.getServer(static_cast<size_t>(selectedIndex));
      if (server) {
        activityManager.replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, *server));
      }
    }
    return;
  }

  // Settings mode: open editor for selected server, or create a new one
  auto resultHandler = [this](const ActivityResult&) {
    // Reload server list when returning from editor
    OPDS_STORE.loadFromFile();
    selectedIndex = 0;
  };

  if (selectedIndex < serverCount) {
    startActivityForResult(std::make_unique<OpdsSettingsActivity>(renderer, mappedInput, selectedIndex), resultHandler);
  } else {
    startActivityForResult(std::make_unique<OpdsSettingsActivity>(renderer, mappedInput, -1), resultHandler);
  }
}

bool OpdsServerListActivity::handleTouch() {
#ifndef CROSSPOINT_BOARD_MURPHY_M4
  return false;
#else
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
    if (pickerMode) {
      activityManager.goHome(HomeMenuItem::OPDS_BROWSER);
    } else {
      finish();
    }
    return true;
  }

  const int itemCount = getItemCount();
  if (itemCount <= 0) {
    return mappedInput.wasTapped();
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const Rect listBounds{screen.x, contentTop, screen.width, renderer.getScreenHeight() - contentTop};
  const int listRows = std::max(1, listBounds.height / metrics.listRowHeight);
  const auto layout = TouchList::calculatePageLayout(selectedIndex, itemCount, listRows);
  const int visibleRow = TouchNavigator::tappedListIndex(mappedInput, listBounds, TouchList::visibleRowCount(layout),
                                                        0, metrics.listRowHeight, 0);
  if (visibleRow < 0) {
    return mappedInput.wasTapped();
  }
  if (TouchList::isPreviousPageRow(layout, visibleRow)) {
    selectedIndex = TouchList::calculatePageLayout(std::max(0, layout.start - 1), itemCount, listRows).start;
    requestUpdate();
    return true;
  }
  if (TouchList::isNextPageRow(layout, visibleRow)) {
    selectedIndex = std::min(itemCount - 1, layout.start + layout.itemCount);
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

void OpdsServerListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  TouchUi::drawHeaderWithBack(renderer, screen, tr(STR_OPDS_SERVERS));
#else
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_OPDS_SERVERS));
#endif

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
#ifdef CROSSPOINT_BOARD_MURPHY_M4
      pageHeight - contentTop;
#else
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
#endif
  const int itemCount = getItemCount();

  if (itemCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_SERVERS));
  } else {
    const auto& servers = OPDS_STORE.getServers();
    const auto serverCount = static_cast<int>(servers.size());

#ifdef CROSSPOINT_BOARD_MURPHY_M4
    const Rect listBounds{0, contentTop, pageWidth, contentHeight};
    const int listRows = std::max(1, contentHeight / metrics.listRowHeight);
    const auto layout = TouchList::calculatePageLayout(selectedIndex, itemCount, listRows);
    const int visibleSelected = layout.previous + selectedIndex - layout.start;
    GUI.drawList(
        renderer, listBounds, TouchList::visibleRowCount(layout), visibleSelected,
        [&servers, serverCount, layout](int visibleRow) {
          if (TouchList::isPreviousPageRow(layout, visibleRow)) return std::string(tr(STR_PREV_PAGE));
          if (TouchList::isNextPageRow(layout, visibleRow)) return std::string(tr(STR_NEXT_PAGE));
          const int index = TouchList::visibleRowToItemIndex(layout, visibleRow);
          if (index < serverCount) {
            const auto& server = servers[index];
            return server.name.empty() ? server.url : server.name;
          }
          return std::string(I18n::getInstance().get(StrId::STR_ADD_SERVER));
        },
        [&servers, serverCount, layout](int visibleRow) {
          const int index = TouchList::visibleRowToItemIndex(layout, visibleRow);
          if (index >= 0 && index < serverCount && !servers[index].name.empty()) {
            return servers[index].url;
          }
          return std::string("");
        });
    if (layout.previous) TouchUi::drawCenteredPagerRow(renderer, listBounds, 0, tr(STR_PREV_PAGE));
    if (layout.next) {
      TouchUi::drawCenteredPagerRow(renderer, listBounds, TouchList::visibleRowCount(layout) - 1, tr(STR_NEXT_PAGE));
    }
#else
    // Primary label: server name (falling back to URL if unnamed).
    // Secondary label: server URL (shown as subtitle when name is set).
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex,
        [&servers, serverCount](int index) {
          if (index < serverCount) {
            const auto& server = servers[index];
            return server.name.empty() ? server.url : server.name;
          }
          return std::string(I18n::getInstance().get(StrId::STR_ADD_SERVER));
        },
        [&servers, serverCount](int index) {
          if (index < serverCount && !servers[index].name.empty()) {
            return servers[index].url;
          }
          return std::string("");
        });
#endif
  }

#ifndef CROSSPOINT_BOARD_MURPHY_M4
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

  renderer.displayBuffer();
}
