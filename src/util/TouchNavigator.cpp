#include "TouchNavigator.h"

#include <algorithm>

bool TouchNavigator::contains(const Rect rect, const MappedInputManager::TouchPoint point) {
  return point.x >= rect.x && point.x < rect.x + rect.width && point.y >= rect.y && point.y < rect.y + rect.height;
}

bool TouchNavigator::wasTappedIn(const MappedInputManager& input, const Rect rect) {
  return input.wasTapped() && contains(rect, input.lastTap());
}

int TouchNavigator::tappedGridIndex(const MappedInputManager& input, const Rect rect, const int itemCount,
                                    const int columns) {
  if (!input.wasTapped() || itemCount <= 0 || columns <= 0 || rect.width <= 0 || rect.height <= 0) {
    return -1;
  }

  const auto tap = input.lastTap();
  if (!contains(rect, tap)) {
    return -1;
  }

  const int rows = (itemCount + columns - 1) / columns;
  const int cellWidth = std::max(1, rect.width / columns);
  const int cellHeight = std::max(1, rect.height / rows);
  const int col = std::min(columns - 1, (tap.x - rect.x) / cellWidth);
  const int row = std::min(rows - 1, (tap.y - rect.y) / cellHeight);
  const int index = row * columns + col;
  return index < itemCount ? index : -1;
}

int TouchNavigator::tappedListIndex(const MappedInputManager& input, const Rect rect, const int itemCount,
                                    const int selectedIndex, const int rowHeight, const int rowGap) {
  if (!input.wasTapped() || itemCount <= 0 || rowHeight <= 0) {
    return -1;
  }

  const auto tap = input.lastTap();
  if (!contains(rect, tap)) {
    return -1;
  }

  const int rowStep = rowHeight + std::max(0, rowGap);
  const int visibleRows = std::max(1, rect.height / rowStep);
  const int safeSelected = std::max(0, selectedIndex);
  const int pageStartIndex = (safeSelected / visibleRows) * visibleRows;
  const int localY = tap.y - rect.y;
  if (localY % rowStep >= rowHeight) {
    return -1;
  }
  const int row = localY / rowStep;
  const int index = pageStartIndex + row;
  return index < itemCount ? index : -1;
}

int TouchNavigator::tappedEqualTabIndex(const MappedInputManager& input, const Rect rect, const int tabCount) {
  if (!input.wasTapped() || tabCount <= 0 || rect.width <= 0) {
    return -1;
  }

  const auto tap = input.lastTap();
  if (!contains(rect, tap)) {
    return -1;
  }

  const int slotWidth = std::max(1, rect.width / tabCount);
  return std::min(tabCount - 1, (tap.x - rect.x) / slotWidth);
}
