#include "TouchKeyboard.h"

#include "components/UITheme.h"

namespace {
constexpr int TOUCH_SLOP_Y = 8;
constexpr int NARROW_EDGE_SLOP_X = 12;

bool contains(const Rect rect, const int x, const int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}
}  // namespace

TouchKeyboardLayout::TouchKeyboardLayout(const GfxRenderer& renderer, const int contentRows, const int contentCols,
                                         const int bottomKeyCount, const int bottomReservePx,
                                         const bool narrowContentAnchoredToBottom, const int anchorBottomCol,
                                         const int canonicalCols)
    : pageWidth(renderer.getScreenWidth()),
      contentRows(contentRows),
      contentCols(contentCols),
      bottomKeyCount(bottomKeyCount),
      canonicalCols(canonicalCols),
      narrowContentAnchoredToBottom(narrowContentAnchoredToBottom),
      anchorBottomCol(anchorBottomCol) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  keyHeight = metrics.keyboardKeyHeight;
  bottomKeyHeight = metrics.keyboardBottomKeyHeight;
  keySpacing = metrics.keyboardKeySpacing;
  bottomSpacing = metrics.keyboardBottomKeySpacing;

  const int keyboardWidth = pageWidth * metrics.keyboardWidthPercent / 100;
  keyWidth = (keyboardWidth - (contentCols - 1) * keySpacing) / contentCols;
  contentLeft = (pageWidth - (contentCols * keyWidth + (contentCols - 1) * keySpacing)) / 2;

  canonicalKeyWidth = (keyboardWidth - (canonicalCols - 1) * keySpacing) / canonicalCols;
  canonicalContentWidth = canonicalCols * canonicalKeyWidth + (canonicalCols - 1) * keySpacing;
  bottomKeyWidth = (canonicalContentWidth - (bottomKeyCount - 1) * bottomSpacing) / bottomKeyCount;
  bottomLeft = (pageWidth - (bottomKeyCount * bottomKeyWidth + (bottomKeyCount - 1) * bottomSpacing)) / 2;

  const int bottomRowGap = metrics.keyboardBottomKeySpacing > 0 ? 4 : 0;
  keyboardStartY = renderer.getScreenHeight() - bottomReservePx - (keyHeight + keySpacing) * contentRows -
                   bottomKeyHeight - bottomRowGap + metrics.keyboardVerticalOffset;
  bottomY = keyboardStartY + contentRows * (keyHeight + keySpacing) + bottomRowGap;

  if (narrowContentAnchoredToBottom) {
    const int urlTotalWidth = contentCols * keyWidth + (contentCols - 1) * keySpacing;
    const int anchorCenterX = bottomLeft + anchorBottomCol * (bottomKeyWidth + bottomSpacing) + bottomKeyWidth / 2;
    contentLeft = anchorCenterX - urlTotalWidth / 2;
  }
}

Rect TouchKeyboardLayout::contentKeyRect(const int row, const int col) const {
  return Rect{contentLeft + col * (keyWidth + keySpacing), keyboardStartY + row * (keyHeight + keySpacing), keyWidth,
              keyHeight};
}

Rect TouchKeyboardLayout::bottomKeyRect(const int col) const {
  return Rect{bottomLeft + col * (bottomKeyWidth + bottomSpacing), bottomY, bottomKeyWidth, bottomKeyHeight};
}

Rect TouchKeyboardLayout::midpointColumnRect(const int left, const int keyW, const int spacing, const int col,
                                            const int cols, const int edgeLeft, const int edgeRight, const int yTop,
                                            const int yBottom) const {
  const int centerX = left + col * (keyW + spacing) + keyW / 2;
  const int x0 = col == 0 ? edgeLeft : (left + (col - 1) * (keyW + spacing) + keyW / 2 + centerX) / 2;
  const int x1 = col == cols - 1 ? edgeRight : (centerX + left + (col + 1) * (keyW + spacing) + keyW / 2) / 2;
  return Rect{x0, yTop, x1 - x0, yBottom - yTop};
}

bool TouchKeyboardLayout::hitContentKey(const int x, const int y, int& row, int& col) const {
  const int rowRight = contentLeft + contentCols * keyWidth + (contentCols - 1) * keySpacing;
  const int edgeLeft = narrowContentAnchoredToBottom ? contentLeft - NARROW_EDGE_SLOP_X : 0;
  const int edgeRight = narrowContentAnchoredToBottom ? rowRight + NARROW_EDGE_SLOP_X : pageWidth;

  for (int r = 0; r < contentRows; ++r) {
    const int rowY = keyboardStartY + r * (keyHeight + keySpacing);
    const int centerY = rowY + keyHeight / 2;
    const int yTop =
        r == 0 ? rowY - TOUCH_SLOP_Y
               : (keyboardStartY + (r - 1) * (keyHeight + keySpacing) + keyHeight / 2 + centerY) / 2;
    const int yBottom =
        r == contentRows - 1
            ? rowY + keyHeight + TOUCH_SLOP_Y
            : (centerY + keyboardStartY + (r + 1) * (keyHeight + keySpacing) + keyHeight / 2) / 2;

    for (int c = 0; c < contentCols; ++c) {
      if (contains(
              midpointColumnRect(contentLeft, keyWidth, keySpacing, c, contentCols, edgeLeft, edgeRight, yTop, yBottom),
              x, y)) {
        row = r;
        col = c;
        return true;
      }
    }
  }
  return false;
}

bool TouchKeyboardLayout::hitBottomKey(const int x, const int y, int& col) const {
  const int yTop = bottomY - TOUCH_SLOP_Y;
  const int yBottom = bottomY + bottomKeyHeight + TOUCH_SLOP_Y;
  for (int c = 0; c < bottomKeyCount; ++c) {
    if (contains(
            midpointColumnRect(bottomLeft, bottomKeyWidth, bottomSpacing, c, bottomKeyCount, 0, pageWidth, yTop, yBottom),
            x, y)) {
      col = c;
      return true;
    }
  }
  return false;
}
