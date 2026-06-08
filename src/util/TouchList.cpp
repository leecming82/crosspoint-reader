#include "TouchList.h"

#include <algorithm>

namespace TouchList {

TouchListPageLayout calculatePageLayout(int selectedIndex, int itemCount, int listRows) {
  itemCount = std::max(0, itemCount);
  listRows = std::max(1, listRows);
  selectedIndex = std::clamp(selectedIndex, 0, std::max(0, itemCount - 1));

  TouchListPageLayout layout;
  int start = 0;
  bool previous = false;

  while (start < itemCount) {
    const int remaining = itemCount - start;
    const int rowsAvailable = std::max(1, listRows - (previous ? 1 : 0));
    const bool next = remaining > rowsAvailable;
    const int itemsOnPage = next ? std::max(1, rowsAvailable - 1) : remaining;

    layout = TouchListPageLayout{start, itemsOnPage, previous, next};
    if (selectedIndex < start + itemsOnPage || !next) {
      break;
    }

    start += itemsOnPage;
    previous = true;
  }

  return layout;
}

int visibleRowCount(const TouchListPageLayout& layout) {
  return layout.itemCount + (layout.previous ? 1 : 0) + (layout.next ? 1 : 0);
}

bool isPreviousPageRow(const TouchListPageLayout& layout, const int visibleRow) {
  return layout.previous && visibleRow == 0;
}

bool isNextPageRow(const TouchListPageLayout& layout, const int visibleRow) {
  return layout.next && visibleRow == visibleRowCount(layout) - 1;
}

int visibleRowToItemIndex(const TouchListPageLayout& layout, const int visibleRow) {
  if (visibleRow < 0 || isPreviousPageRow(layout, visibleRow) || isNextPageRow(layout, visibleRow)) {
    return -1;
  }

  const int itemOffset = visibleRow - (layout.previous ? 1 : 0);
  if (itemOffset < 0 || itemOffset >= layout.itemCount) {
    return -1;
  }
  return layout.start + itemOffset;
}

}  // namespace TouchList

