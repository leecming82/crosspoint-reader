#include "IntervalSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchNavigator.h"
#include "util/TouchUi.h"

int IntervalSelectionActivity::clampedValue(const int candidate) const {
  return std::clamp(candidate, minValue, maxValue);
}

void IntervalSelectionActivity::onEnter() {
  Activity::onEnter();
  value = clampedValue(value);
  requestUpdate();
}

void IntervalSelectionActivity::adjustValue(const int delta) {
  value = clampedValue(value + delta);
  requestUpdate();
}

void IntervalSelectionActivity::loop() {
  if (ignoreConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
      return;
    }
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
    }
  }

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int buttonHeight = 54;
  const int gap = metrics.contentSidePadding;
  const int buttonWidth = (renderer.getScreenWidth() - metrics.contentSidePadding * 2 - gap) / 2;
  const int buttonY = 178;
  const Rect minusRect{metrics.contentSidePadding, buttonY, buttonWidth, buttonHeight};
  const Rect plusRect{minusRect.x + minusRect.width + gap, buttonY, buttonWidth, buttonHeight};
  if (TouchNavigator::wasTappedIn(mappedInput, minusRect)) {
    adjustValue(-smallStep);
    return;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, plusRect)) {
    adjustValue(smallStep);
    return;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::bottomActionRect(renderer))) {
    setResult(IntervalResult{static_cast<uint32_t>(value)});
    finish();
    return;
  }
#endif

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(IntervalResult{static_cast<uint32_t>(value)});
    finish();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustValue(-smallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustValue(smallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { adjustValue(largeStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { adjustValue(-largeStep); });
}

void IntervalSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  TouchUi::drawHeaderWithBack(renderer, screen, I18N.get(titleId));
#else
  renderer.drawCenteredText(UI_12_FONT_ID, 15, I18N.get(titleId), true, EpdFontFamily::BOLD);
#endif

  char formattedValue[32];
  if (maxBoundaryLabelId != StrId::STR_NONE_OPT && value == maxValue) {
    snprintf(formattedValue, sizeof(formattedValue), "%s", I18N.get(maxBoundaryLabelId));
  } else if (valueFormatId != StrId::STR_NONE_OPT) {
    snprintf(formattedValue, sizeof(formattedValue), I18N.get(valueFormatId), static_cast<unsigned int>(value));
  } else {
    snprintf(formattedValue, sizeof(formattedValue), "%d", value);
  }
  renderer.drawCenteredText(UI_12_FONT_ID, 90, formattedValue, true, EpdFontFamily::BOLD);

  const int screenWidth = renderer.getScreenWidth();
  const int barWidth = std::min(360, std::max(0, screenWidth - 40));
  constexpr int barHeight = 16;
  const int barX = std::max(0, (screenWidth - barWidth) / 2);
  const int barY = 140;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  const int range = std::max(1, maxValue - minValue);
  const int fillWidth = (barWidth - 4) * (value - minValue) / range;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  const int knobX = std::max(barX + 2, barX + 2 + fillWidth - 2);
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  renderer.drawCenteredText(SMALL_FONT_ID, barY + 30, I18N.get(stepHintId), true);

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int buttonHeight = 54;
  const int gap = metrics.contentSidePadding;
  const int buttonWidth = (renderer.getScreenWidth() - metrics.contentSidePadding * 2 - gap) / 2;
  const int buttonY = 178;
  const Rect minusRect{metrics.contentSidePadding, buttonY, buttonWidth, buttonHeight};
  const Rect plusRect{minusRect.x + minusRect.width + gap, buttonY, buttonWidth, buttonHeight};
  TouchUi::drawTouchButton(renderer, minusRect, "-");
  TouchUi::drawTouchButton(renderer, plusRect, "+");
  TouchUi::drawTouchButton(renderer, TouchUi::bottomActionRect(renderer), tr(STR_OK_BUTTON));
#else
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

  renderer.displayBuffer();
}
