#pragma once

#include <GfxRenderer.h>

#include "components/themes/BaseTheme.h"

class TouchKeyboardLayout {
 public:
  TouchKeyboardLayout(const GfxRenderer& renderer, int contentRows, int contentCols, int bottomKeyCount,
                      int bottomReservePx, bool narrowContentAnchoredToBottom = false, int anchorBottomCol = 0,
                      int canonicalCols = 10);

  Rect contentKeyRect(int row, int col) const;
  Rect bottomKeyRect(int col) const;
  bool hitContentKey(int x, int y, int& row, int& col) const;
  bool hitBottomKey(int x, int y, int& col) const;
  int keyboardTopY() const { return keyboardStartY; }
  int bottomRowY() const { return bottomY; }

 private:
  int pageWidth = 0;
  int contentRows = 0;
  int contentCols = 0;
  int bottomKeyCount = 0;
  int canonicalCols = 10;
  int keyHeight = 0;
  int bottomKeyHeight = 0;
  int keySpacing = 0;
  int bottomSpacing = 0;
  int keyWidth = 0;
  int contentLeft = 0;
  int canonicalKeyWidth = 0;
  int canonicalContentWidth = 0;
  int bottomKeyWidth = 0;
  int bottomLeft = 0;
  int keyboardStartY = 0;
  int bottomY = 0;
  bool narrowContentAnchoredToBottom = false;
  int anchorBottomCol = 0;

  Rect midpointColumnRect(int left, int keyW, int spacing, int col, int cols, int edgeLeft, int edgeRight, int yTop,
                          int yBottom) const;
};
