#include "ButtonRemapActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchNavigator.h"
#include "util/TouchUi.h"

namespace {
// UI steps correspond to logical roles in order: Back, Confirm, Left, Right.
constexpr uint8_t kRoleCount = 4;
// Marker used when a role has not been assigned yet.
constexpr uint8_t kUnassigned = 0xFF;
// Duration to show temporary error text when reassigning a button.
constexpr unsigned long kErrorDisplayMs = 1500;
}  // namespace

void ButtonRemapActivity::onEnter() {
  Activity::onEnter();

  // Start with all roles unassigned to avoid duplicate blocking.
  currentStep = 0;
  tempMapping[0] = kUnassigned;
  tempMapping[1] = kUnassigned;
  tempMapping[2] = kUnassigned;
  tempMapping[3] = kUnassigned;
  errorMessage.clear();
  errorUntil = 0;
  requestUpdate();
}

void ButtonRemapActivity::onExit() { Activity::onExit(); }

void ButtonRemapActivity::loop() {
  // Clear any temporary warning after its timeout.
  if (errorUntil > 0 && millis() > errorUntil) {
    errorMessage.clear();
    errorUntil = 0;
    requestUpdate();
    return;
  }

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
    finish();
    return;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int buttonHeight = 54;
  const int gap = metrics.contentSidePadding;
  const int buttonWidth = (renderer.getScreenWidth() - metrics.contentSidePadding * 2 - gap) / 2;
  const int buttonY = renderer.getScreenHeight() - buttonHeight - 16;
  const Rect resetRect{metrics.contentSidePadding, buttonY, buttonWidth, buttonHeight};
  const Rect cancelRect{resetRect.x + resetRect.width + gap, buttonY, resetRect.width, resetRect.height};
  if (TouchNavigator::wasTappedIn(mappedInput, resetRect)) {
    SETTINGS.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
    SETTINGS.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
    SETTINGS.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
    SETTINGS.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
    SETTINGS.saveToFile();
    finish();
    return;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, cancelRect)) {
    finish();
    return;
  }
#endif

  // Side buttons:
  // - Up: reset mapping to defaults and exit.
  // - Down: cancel without saving.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    // Persist default mapping immediately so the user can recover quickly.
    SETTINGS.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
    SETTINGS.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
    SETTINGS.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
    SETTINGS.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
    SETTINGS.saveToFile();
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    // Exit without changing settings.
    finish();
    return;
  }

  {
    // Make sure UI done rendering before accepting another assignment.
    // This avoids rapid double-presses that can advance the step without a visible redraw.
    RenderLock lock(*this);

    // Wait for a front button press to assign to the current role.
    const int pressedButton = mappedInput.getPressedFrontButton();
    if (pressedButton < 0) {
      return;
    }

    // Update temporary mapping and advance the remap step.
    // Only accept the press if this hardware button isn't already assigned elsewhere.
    if (!validateUnassigned(static_cast<uint8_t>(pressedButton))) {
      requestUpdate();
      return;
    }
    tempMapping[currentStep] = static_cast<uint8_t>(pressedButton);
    currentStep++;

    if (currentStep >= kRoleCount) {
      // All roles assigned; save to settings and exit.
      applyTempMapping();
      SETTINGS.saveToFile();
      finish();
      return;
    }

    requestUpdate();
  }
}

void ButtonRemapActivity::render(RenderLock&&) {
  const auto labelForHardware = [&](uint8_t hardwareIndex) -> const char* {
    for (uint8_t i = 0; i < kRoleCount; i++) {
      if (tempMapping[i] == hardwareIndex) {
        return getRoleName(i);
      }
    }
    return "-";
  };

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  TouchUi::drawHeaderWithBack(renderer, screen, tr(STR_REMAP_FRONT_BUTTONS));
#else
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_REMAP_FRONT_BUTTONS));
#endif
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    tr(STR_REMAP_PROMPT));

  int topOffset = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  int contentHeight =
#ifdef CROSSPOINT_BOARD_MURPHY_M4
      pageHeight - topOffset - 72;
#else
      pageHeight - topOffset - metrics.buttonHintsHeight - metrics.verticalSpacing;
#endif
  GUI.drawList(
      renderer, Rect{0, topOffset, pageWidth, contentHeight}, kRoleCount, currentStep,
      [&](int index) { return getRoleName(static_cast<uint8_t>(index)); }, nullptr, nullptr,
      [&](int index) {
        uint8_t assignedButton = tempMapping[static_cast<uint8_t>(index)];
        return (assignedButton == kUnassigned) ? tr(STR_UNASSIGNED) : getHardwareName(assignedButton);
      },
      true);

  // Temporary warning banner for duplicates.
  if (!errorMessage.empty()) {
    GUI.drawHelpText(renderer,
                     Rect{0, pageHeight - metrics.buttonHintsHeight - metrics.contentSidePadding - 15, pageWidth, 20},
                     errorMessage.c_str());
  }

  // Provide side button actions at the bottom of the screen (split across two lines).
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  constexpr int buttonHeight = 54;
  const int gap = metrics.contentSidePadding;
  const int buttonWidth = (renderer.getScreenWidth() - metrics.contentSidePadding * 2 - gap) / 2;
  const int buttonY = renderer.getScreenHeight() - buttonHeight - 16;
  const Rect resetRect{metrics.contentSidePadding, buttonY, buttonWidth, buttonHeight};
  const Rect cancelRect{resetRect.x + resetRect.width + gap, buttonY, resetRect.width, resetRect.height};
  TouchUi::drawTouchButton(renderer, resetRect, "Reset");
  TouchUi::drawTouchButton(renderer, cancelRect, tr(STR_CANCEL));
#else
  GUI.drawHelpText(renderer,
                   Rect{0, topOffset + 4 * metrics.listRowHeight + 4 * metrics.verticalSpacing, pageWidth, 20},
                   tr(STR_REMAP_RESET_HINT));
  GUI.drawHelpText(renderer,
                   Rect{0, topOffset + 4 * metrics.listRowHeight + 5 * metrics.verticalSpacing + 20, pageWidth, 20},
                   tr(STR_REMAP_CANCEL_HINT));
  // Live preview of logical labels under front buttons.
  // This mirrors the on-device front button order: Back, Confirm, Left, Right.
  GUI.drawButtonHints(renderer, labelForHardware(CrossPointSettings::FRONT_HW_BACK),
                      labelForHardware(CrossPointSettings::FRONT_HW_CONFIRM),
                      labelForHardware(CrossPointSettings::FRONT_HW_LEFT),
                      labelForHardware(CrossPointSettings::FRONT_HW_RIGHT));
#endif
  renderer.displayBuffer();
}

void ButtonRemapActivity::applyTempMapping() {
  // Commit temporary mapping into settings (logical role -> hardware).
  SETTINGS.frontButtonBack = tempMapping[0];
  SETTINGS.frontButtonConfirm = tempMapping[1];
  SETTINGS.frontButtonLeft = tempMapping[2];
  SETTINGS.frontButtonRight = tempMapping[3];
}

bool ButtonRemapActivity::validateUnassigned(const uint8_t pressedButton) {
  // Block reusing a hardware button already assigned to another role.
  for (uint8_t i = 0; i < kRoleCount; i++) {
    if (tempMapping[i] == pressedButton && i != currentStep) {
      errorMessage = tr(STR_ALREADY_ASSIGNED);
      errorUntil = millis() + kErrorDisplayMs;
      return false;
    }
  }
  return true;
}

const char* ButtonRemapActivity::getRoleName(const uint8_t roleIndex) const {
  switch (roleIndex) {
    case 0:
      return tr(STR_BACK);
    case 1:
      return tr(STR_CONFIRM);
    case 2:
      return tr(STR_DIR_LEFT);
    case 3:
    default:
      return tr(STR_DIR_RIGHT);
  }
}

const char* ButtonRemapActivity::getHardwareName(const uint8_t buttonIndex) const {
  switch (buttonIndex) {
    case CrossPointSettings::FRONT_HW_BACK:
      return tr(STR_HW_BACK_LABEL);
    case CrossPointSettings::FRONT_HW_CONFIRM:
      return tr(STR_HW_CONFIRM_LABEL);
    case CrossPointSettings::FRONT_HW_LEFT:
      return tr(STR_HW_LEFT_LABEL);
    case CrossPointSettings::FRONT_HW_RIGHT:
      return tr(STR_HW_RIGHT_LABEL);
    default:
      return "Unknown";
  }
}
