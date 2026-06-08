#pragma once

class GfxRenderer;
struct Rect;

namespace TouchUi {
Rect headerBackTapRect(const GfxRenderer& renderer);
Rect backButtonRect(const GfxRenderer& renderer);
Rect bottomActionRect(const GfxRenderer& renderer);
void drawHeaderWithBack(const GfxRenderer& renderer, const Rect screen, const char* title);
void drawCenteredPagerRow(const GfxRenderer& renderer, Rect listBounds, int visibleRow, const char* label);
void drawTouchButton(const GfxRenderer& renderer, Rect rect, const char* label);
}  // namespace TouchUi

