#include "TouchUi.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "components/UITheme.h"
#include "components/icons/back24.h"
#include "fontIds.h"

namespace {
constexpr int BACK_ICON_SIZE = 24;
constexpr int BACK_ICON_VISUAL_OFFSET_Y = 5;
}  // namespace

namespace TouchUi {

Rect headerBackTapRect(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, 0, renderer.getScreenWidth() / 3, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing};
}

Rect backButtonRect(const GfxRenderer&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{8, metrics.topPadding + metrics.batteryBarHeight - 10, 48, 48};
}

Rect bottomActionRect(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, renderer.getScreenHeight() - metrics.listRowHeight, renderer.getScreenWidth(), metrics.listRowHeight};
}

void drawHeaderWithBack(const GfxRenderer& renderer, const Rect screen, const char* title) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight}, "");

  const Rect backRect = backButtonRect(renderer);
  const int iconX = backRect.x + (backRect.width - BACK_ICON_SIZE) / 2;
  const int iconY = backRect.y + (backRect.height - BACK_ICON_SIZE) / 2 + BACK_ICON_VISUAL_OFFSET_Y;
  const auto orientation = renderer.getOrientation();
  if (orientation == GfxRenderer::Orientation::LandscapeClockwise ||
      orientation == GfxRenderer::Orientation::LandscapeCounterClockwise) {
    renderer.drawImage(Back24Icon, iconX, iconY, BACK_ICON_SIZE, BACK_ICON_SIZE);
  } else {
    renderer.drawIcon(Back24Icon, iconX, iconY, BACK_ICON_SIZE, BACK_ICON_SIZE);
  }

  const int titleX = backRect.x + backRect.width + 8;
  const int titleMaxWidth = renderer.getScreenWidth() - titleX - metrics.contentSidePadding * 2 - metrics.batteryWidth;
  const auto headerTitle = renderer.truncatedText(UI_12_FONT_ID, title, titleMaxWidth, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, titleX, metrics.topPadding + metrics.batteryBarHeight + 3, headerTitle.c_str(), true,
                    EpdFontFamily::BOLD);
}

void drawCenteredPagerRow(const GfxRenderer& renderer, const Rect listBounds, const int visibleRow, const char* label) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int rowHeight = metrics.listRowHeight;
  const int rowX = listBounds.x + metrics.contentSidePadding;
  const int rowWidth = listBounds.width - metrics.contentSidePadding * 2 - 1;
  const int rowY = listBounds.y + visibleRow * rowHeight;
  const Rect rowRect{rowX, rowY + 2, rowWidth, rowHeight - 4};

  renderer.fillRect(rowRect.x + 1, rowRect.y + 1, rowRect.width - 2, rowRect.height - 2, false);
  renderer.drawRoundedRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height, 1, 6, true);

  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  const int textY = rowRect.y + (rowRect.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, rowRect.x + (rowRect.width - textWidth) / 2, textY, label, true,
                    EpdFontFamily::BOLD);
}

void drawTouchButton(const GfxRenderer& renderer, const Rect rect, const char* label) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  renderer.drawRoundedRect(rect.x + metrics.contentSidePadding, rect.y + 4,
                           rect.width - metrics.contentSidePadding * 2, rect.height - 8, 1, 6, true);

  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, rect.x + (rect.width - textWidth) / 2, textY, label, true, EpdFontFamily::BOLD);
}

}  // namespace TouchUi

