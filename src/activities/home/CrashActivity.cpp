#include "CrashActivity.h"

#include <GfxRenderer.h>
#include <HalSystem.h>
#include <I18n.h>

#include "components/UITheme.h"
#include "components/icons/back24.h"
#include "fontIds.h"
#include "util/TouchNavigator.h"

namespace {
constexpr int BACK_ICON_SIZE = 24;
constexpr int BACK_ICON_VISUAL_OFFSET_Y = 5;

void drawCrashHeaderTitle(const GfxRenderer& renderer, Rect backRect, const char* title) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.drawIcon(Back24Icon, backRect.x + (backRect.width - BACK_ICON_SIZE) / 2,
                    backRect.y + (backRect.height - BACK_ICON_SIZE) / 2 + BACK_ICON_VISUAL_OFFSET_Y, BACK_ICON_SIZE,
                    BACK_ICON_SIZE);

  const int titleX = backRect.x + backRect.width + 8;
  const int titleMaxWidth = renderer.getScreenWidth() - titleX - metrics.contentSidePadding * 2 - metrics.batteryWidth;
  const auto headerTitle = renderer.truncatedText(UI_12_FONT_ID, title, titleMaxWidth, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, titleX, metrics.topPadding + metrics.batteryBarHeight + 3, headerTitle.c_str(), true,
                    EpdFontFamily::BOLD);
}
}  // namespace

void CrashActivity::onEnter() {
  Activity::onEnter();

  panicMessage = HalSystem::getPanicInfo(false);
  if (panicMessage.empty()) {
    panicMessage = tr(STR_CRASH_NO_REASON);
  }
  HalSystem::clearPanic();

  requestUpdateAndWait();
}

Rect CrashActivity::backButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{8, metrics.topPadding + metrics.batteryBarHeight - 10, 48, 48};
}

Rect CrashActivity::headerBackTapRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, 0, renderer.getScreenWidth() / 3, metrics.topPadding + metrics.headerHeight};
}

bool CrashActivity::handleTouch() {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  if (!mappedInput.wasTapped()) {
    return false;
  }

  if (TouchNavigator::wasTappedIn(mappedInput, headerBackTapRect())) {
    finish();
    return true;
  }
#endif
  return false;
}

void CrashActivity::loop() {
  if (handleTouch()) {
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back)) {
    finish();
  }
}

void CrashActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto contentWidth = pageWidth - 2 * metrics.contentSidePadding;
  const auto x = metrics.contentSidePadding;
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "");
  drawCrashHeaderTitle(renderer, backButtonRect(), tr(STR_CRASH_TITLE));
#else
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CRASH_TITLE));
#endif

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  auto descLines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_CRASH_DESCRIPTION), contentWidth, 10);
  for (const auto& line : descLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  y += metrics.verticalSpacing * 2;
  renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_CRASH_REASON));
  y += lineHeight + metrics.verticalSpacing;

  auto panicLines = renderer.wrappedText(UI_10_FONT_ID, panicMessage.c_str(), contentWidth, 5);
  for (const auto& line : panicLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
