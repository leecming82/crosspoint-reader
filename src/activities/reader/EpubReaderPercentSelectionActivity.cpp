#include "EpubReaderPercentSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchNavigator.h"
#include "util/TouchUi.h"

namespace {
// Fine/coarse slider step sizes for percent adjustments.
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;
}  // namespace

void EpubReaderPercentSelectionActivity::onEnter() {
  Activity::onEnter();
  previousRendererOrientation = renderer.getOrientation();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  // Set up rendering task and mark first frame dirty.
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::onExit() {
  renderer.setOrientation(previousRendererOrientation);
  Activity::onExit();
}

void EpubReaderPercentSelectionActivity::adjustPercent(const int delta) {
  // Apply delta and clamp within 0-100.
  percent += delta;
  if (percent < 0) {
    percent = 0;
  } else if (percent > 100) {
    percent = 100;
  }
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void EpubReaderPercentSelectionActivity::confirm() {
  setResult(PercentResult{percent});
  finish();
}

Rect EpubReaderPercentSelectionActivity::sliderRect() const {
  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen =
#ifdef CROSSPOINT_BOARD_MURPHY_M4
      theme.getScreenSafeArea(renderer, false, false);
#else
      theme.getScreenSafeArea(renderer, true, false);
#endif
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 4;
  constexpr int barHeight = 16;
  const int barWidth = std::min(360, screen.width - metrics.contentSidePadding * 2);
  const int barX = screen.x + (screen.width - barWidth) / 2;
  const int barY = contentTop + metrics.verticalSpacing * 2;
  return Rect{barX, barY - 20, barWidth, barHeight + 40};
}

Rect EpubReaderPercentSelectionActivity::decrementButtonRect() const {
  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen =
#ifdef CROSSPOINT_BOARD_MURPHY_M4
      theme.getScreenSafeArea(renderer, false, false);
#else
      theme.getScreenSafeArea(renderer, true, false);
#endif
  const Rect slider = sliderRect();
  const int gap = metrics.verticalSpacing * 2;
  const int buttonWidth = std::min(140, (screen.width - metrics.contentSidePadding * 2 - gap) / 2);
  const int totalWidth = buttonWidth * 2 + gap;
  const int buttonHeight = 54;
  const int x = screen.x + (screen.width - totalWidth) / 2;
  const int y = slider.y + slider.height + metrics.verticalSpacing * 3;
  return Rect{x, y, buttonWidth, buttonHeight};
}

Rect EpubReaderPercentSelectionActivity::incrementButtonRect() const {
  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  const Rect decrement = decrementButtonRect();
  return Rect{decrement.x + decrement.width + metrics.verticalSpacing * 2, decrement.y, decrement.width,
              decrement.height};
}

bool EpubReaderPercentSelectionActivity::handleTouch() {
#ifndef CROSSPOINT_BOARD_MURPHY_M4
  return false;
#else
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
    cancel();
    return true;
  }

  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::bottomActionRect(renderer))) {
    confirm();
    return true;
  }

  if (TouchNavigator::wasTappedIn(mappedInput, decrementButtonRect())) {
    adjustPercent(-kSmallStep);
    return true;
  }

  if (TouchNavigator::wasTappedIn(mappedInput, incrementButtonRect())) {
    adjustPercent(kSmallStep);
    return true;
  }

  const Rect rect = sliderRect();
  if (TouchNavigator::wasTappedIn(mappedInput, rect)) {
    const auto tap = mappedInput.lastTap();
    percent = std::clamp((tap.x - rect.x) * 100 / std::max(1, rect.width), 0, 100);
    requestUpdate();
    return true;
  }

  return mappedInput.wasTapped();
#endif
}

void EpubReaderPercentSelectionActivity::loop() {
  // Back cancels, confirm selects, arrows adjust the percent.
  if (handleTouch()) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirm();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustPercent(-kSmallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustPercent(kSmallStep); });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { adjustPercent(kLargeStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { adjustPercent(-kLargeStep); });
}

void EpubReaderPercentSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen =
#ifdef CROSSPOINT_BOARD_MURPHY_M4
      theme.getScreenSafeArea(renderer, false, false);
#else
      theme.getScreenSafeArea(renderer, true, false);
#endif

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  TouchUi::drawHeaderWithBack(renderer, screen, tr(STR_GO_TO_PERCENT));
#else
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_GO_TO_PERCENT));
#endif

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 4;

  const std::string percentText = std::to_string(percent) + "%";
  UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID, contentTop, percentText.c_str(), true,
                            EpdFontFamily::BOLD);

  // Draw slider track.
  const int barWidth = std::min(360, screen.width - metrics.contentSidePadding * 2);
  constexpr int barHeight = 16;
  const int barX = screen.x + (screen.width - barWidth) / 2;
  const int barY = contentTop + metrics.verticalSpacing * 2;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  // Fill slider based on percent.
  const int fillWidth = (barWidth - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  // Draw a simple knob centered at the current percent.
  const int knobX = barX + 2 + fillWidth - 2;
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  // Hint text for step sizes.
#ifndef CROSSPOINT_BOARD_MURPHY_M4
  UITheme::drawCenteredText(renderer, screen, SMALL_FONT_ID, barY + 30, tr(STR_PERCENT_STEP_HINT), true);

  // Button hints follow the current front button layout.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#else
  TouchUi::drawTouchButton(renderer, decrementButtonRect(), "-");
  TouchUi::drawTouchButton(renderer, incrementButtonRect(), "+");

  const std::string action = std::string(tr(STR_GO_TO_PERCENT)) + " " + percentText;
  TouchUi::drawTouchButton(renderer, TouchUi::bottomActionRect(renderer), action.c_str());
#endif

  renderer.displayBuffer();
}
