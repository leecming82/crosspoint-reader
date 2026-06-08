#include "FrontlightOverlayActivity.h"

#include <HalDisplay.h>
#include <HalFrontlight.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "components/icons/cool_light24.h"
#include "components/icons/warm_light24.h"
#include "fontIds.h"

namespace {
constexpr int FRONTLIGHT_LEVELS = 8;
constexpr int FRONTLIGHT_ICON_SIZE = 24;

bool containsPoint(const Rect& rect, const MappedInputManager::TouchPoint& point) {
  return point.x >= rect.x && point.x < rect.x + rect.width && point.y >= rect.y && point.y < rect.y + rect.height;
}

uint8_t dutyForLevel(const int level) {
  const int clamped = std::clamp(level, 0, FRONTLIGHT_LEVELS);
  return static_cast<uint8_t>((clamped * 255 + FRONTLIGHT_LEVELS / 2) / FRONTLIGHT_LEVELS);
}

int levelForDuty(const uint8_t duty) {
  return static_cast<int>((static_cast<uint16_t>(duty) * FRONTLIGHT_LEVELS + 127) / 255);
}
}  // namespace

void FrontlightOverlayActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void FrontlightOverlayActivity::onExit() {
  if (dirty) {
    SETTINGS.saveToFile();
    dirty = false;
  }
  Activity::onExit();
}

FrontlightOverlayActivity::Layout FrontlightOverlayActivity::layout() const {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int modalW = std::min(screenW - 32, 420);
  const int modalH = std::min(screenH - 32, 520);
  const int modalX = (screenW - modalW) / 2;
  const int modalY = (screenH - modalH) / 2;
  const int barH = std::max(220, modalH - 150);
  const int barW = std::min(76, std::max(56, modalW / 5));
  const int barGap = std::min(96, std::max(48, modalW / 5));
  const int barsW = barW * 2 + barGap;
  const int barY = modalY + 82;
  const int coolX = modalX + (modalW - barsW) / 2;
  const int warmX = coolX + barW + barGap;

  Layout result;
  result.modal = Rect{modalX, modalY, modalW, modalH};
  result.coolBar = Rect{coolX, barY, barW, barH};
  result.warmBar = Rect{warmX, barY, barW, barH};
  result.coolIcon = Rect{coolX + (barW - FRONTLIGHT_ICON_SIZE) / 2, modalY + 38, FRONTLIGHT_ICON_SIZE,
                         FRONTLIGHT_ICON_SIZE};
  result.warmIcon = Rect{warmX + (barW - FRONTLIGHT_ICON_SIZE) / 2, modalY + 38, FRONTLIGHT_ICON_SIZE,
                         FRONTLIGHT_ICON_SIZE};
  return result;
}

void FrontlightOverlayActivity::drawIcon(const uint8_t* icon, const Rect& rect) const {
  const auto orientation = renderer.getOrientation();
  if (orientation == GfxRenderer::Orientation::LandscapeClockwise ||
      orientation == GfxRenderer::Orientation::LandscapeCounterClockwise) {
    renderer.drawImage(icon, rect.x, rect.y, rect.width, rect.height);
  } else {
    renderer.drawIcon(icon, rect.x, rect.y, rect.width, rect.height);
  }
}

void FrontlightOverlayActivity::drawBar(const Rect& rect, const uint8_t duty) const {
  renderer.fillRect(rect.x - 2, rect.y - 2, rect.width + 4, rect.height + 4, false);
  renderer.drawRoundedRect(rect.x - 2, rect.y - 2, rect.width + 4, rect.height + 4, 1, 7, true);

  const int level = levelForDuty(duty);
  const int segmentH = rect.height / FRONTLIGHT_LEVELS;
  for (int i = 0; i < FRONTLIGHT_LEVELS; ++i) {
    const int cellY = rect.y + rect.height - (i + 1) * segmentH;
    const Rect cell{rect.x + 8, cellY + 1, rect.width - 16, std::max(1, segmentH - 2)};
    renderer.drawRect(cell.x, cell.y, cell.width, cell.height, true);
    if (i < level) {
      renderer.fillRect(cell.x + 1, cell.y + 1, cell.width - 2, std::max(1, cell.height - 2), true);
    }
  }
}

bool FrontlightOverlayActivity::updateFromTap(const Rect& bar, const MappedInputManager::TouchPoint& point,
                                              uint8_t& duty) {
  if (!containsPoint(bar, point)) return false;
  int level = levelForDuty(duty);
  if (point.y < bar.y + bar.height / 2) {
    ++level;
  } else {
    --level;
  }
  duty = dutyForLevel(level);
  return true;
}

void FrontlightOverlayActivity::loop() {
  if (!mappedInput.wasTapped()) return;

  const auto point = mappedInput.lastTap();
  const Layout l = layout();
  bool changed = false;
  changed |= updateFromTap(l.coolBar, point, SETTINGS.frontlightCoolDuty);
  changed |= updateFromTap(l.warmBar, point, SETTINGS.frontlightWarmDuty);
  if (changed) {
    halFrontlight.set(SETTINGS.frontlightCoolDuty, SETTINGS.frontlightWarmDuty);
    dirty = true;
    requestUpdate();
    return;
  }
  if (!containsPoint(l.modal, point)) {
    finish();
  }
}

void FrontlightOverlayActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Layout l = layout();
  renderer.fillRoundedRect(l.modal.x, l.modal.y, l.modal.width, l.modal.height, 8, Color::White);
  renderer.drawRoundedRect(l.modal.x, l.modal.y, l.modal.width, l.modal.height, 1, 8, true);

  const char* title = "Frontlight";
  const int titleW = renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, l.modal.x + (l.modal.width - titleW) / 2, l.modal.y + 12, title, true,
                    EpdFontFamily::BOLD);

  drawIcon(CoolLight24Icon, l.coolIcon);
  drawIcon(WarmLight24Icon, l.warmIcon);
  drawBar(l.coolBar, SETTINGS.frontlightCoolDuty);
  drawBar(l.warmBar, SETTINGS.frontlightWarmDuty);

  char value[8];
  snprintf(value, sizeof(value), "%u", SETTINGS.frontlightCoolDuty);
  int valueW = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, l.coolBar.x + (l.coolBar.width - valueW) / 2,
                    l.coolBar.y + l.coolBar.height + 16, value, true, EpdFontFamily::BOLD);
  snprintf(value, sizeof(value), "%u", SETTINGS.frontlightWarmDuty);
  valueW = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, l.warmBar.x + (l.warmBar.width - valueW) / 2,
                    l.warmBar.y + l.warmBar.height + 16, value, true, EpdFontFamily::BOLD);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
