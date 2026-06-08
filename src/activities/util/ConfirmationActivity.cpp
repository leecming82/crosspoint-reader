#include "ConfirmationActivity.h"

#include <I18n.h>

#include "HalDisplay.h"
#include "components/UITheme.h"
#include "util/TouchNavigator.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body)
    : Activity("Confirmation", renderer, mappedInput), heading(heading), body(body) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  if (!heading.empty()) {
    safeHeading = renderer.truncatedText(fontId, heading.c_str(), maxWidth, EpdFontFamily::BOLD);
  }
  if (!body.empty()) {
    safeBody = renderer.truncatedText(fontId, body.c_str(), maxWidth, EpdFontFamily::REGULAR);
  }

  int totalHeight = 0;
  if (!safeHeading.empty()) totalHeight += lineHeight;
  if (!safeBody.empty()) totalHeight += lineHeight;
  if (!safeHeading.empty() && !safeBody.empty()) totalHeight += spacing;

  startY = (renderer.getScreenHeight() - totalHeight) / 2;

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY;
  LOG_DBG("CONF", "currentY: %d", currentY);
  // Draw Heading
  if (!safeHeading.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeHeading.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight + spacing;
  }

  // Draw Body
  if (!safeBody.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeBody.c_str(), true, EpdFontFamily::REGULAR);
  }

  // Draw UI Elements
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  {
    const Rect cancelRect = cancelButtonRect();
    const Rect confirmRect = confirmButtonRect();
    const char* cancelLabel = I18N.get(StrId::STR_CANCEL);
    const char* confirmLabel = I18N.get(StrId::STR_CONFIRM);

    renderer.drawRoundedRect(cancelRect.x, cancelRect.y, cancelRect.width, cancelRect.height, 1, 6, true);
    renderer.drawRoundedRect(confirmRect.x, confirmRect.y, confirmRect.width, confirmRect.height, 1, 6, true);

    const int cancelTextWidth = renderer.getTextWidth(UI_12_FONT_ID, cancelLabel, EpdFontFamily::BOLD);
    const int confirmTextWidth = renderer.getTextWidth(UI_12_FONT_ID, confirmLabel, EpdFontFamily::BOLD);
    const int textY = cancelRect.y + (cancelRect.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
    renderer.drawText(UI_12_FONT_ID, cancelRect.x + (cancelRect.width - cancelTextWidth) / 2, textY, cancelLabel, true,
                      EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, confirmRect.x + (confirmRect.width - confirmTextWidth) / 2, textY, confirmLabel,
                      true, EpdFontFamily::BOLD);
  }
#else
  const auto labels = mappedInput.mapLabels("", "", I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

Rect ConfirmationActivity::actionRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, renderer.getScreenHeight() - metrics.buttonHintsHeight, renderer.getScreenWidth(),
              metrics.buttonHintsHeight};
}

Rect ConfirmationActivity::cancelButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int buttonHeight = 56;
  const int gap = metrics.contentSidePadding;
  const int width = (renderer.getScreenWidth() - metrics.contentSidePadding * 2 - gap) / 2;
  const int y = renderer.getScreenHeight() - buttonHeight - 16;
  return Rect{metrics.contentSidePadding, y, width, buttonHeight};
}

Rect ConfirmationActivity::confirmButtonRect() const {
  const Rect cancelRect = cancelButtonRect();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = cancelRect.x + cancelRect.width + metrics.contentSidePadding;
  return Rect{x, cancelRect.y, cancelRect.width, cancelRect.height};
}

void ConfirmationActivity::confirm() {
  ActivityResult res;
  res.isCancelled = false;
  setResult(std::move(res));
  finish();
}

void ConfirmationActivity::cancel() {
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}

bool ConfirmationActivity::handleTouch() {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  if (!mappedInput.wasTapped()) {
    return false;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, cancelButtonRect())) {
    cancel();
    return true;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, confirmButtonRect())) {
    confirm();
    return true;
  }
  return true;
#else
  const int action = TouchNavigator::tappedActionIndex(mappedInput, actionRect(), 2);
  if (action == 0) {
    cancel();
    return true;
  }
  if (action == 1) {
    confirm();
    return true;
  }
  return mappedInput.wasTapped();
#endif
}

void ConfirmationActivity::loop() {
  if (handleTouch()) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    confirm();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    cancel();
    return;
  }
}
