#pragma once

#include "components/themes/BaseTheme.h"

struct TouchListPageLayout {
  int start = 0;
  int itemCount = 0;
  bool previous = false;
  bool next = false;
};

namespace TouchList {
TouchListPageLayout calculatePageLayout(int selectedIndex, int itemCount, int listRows);
int visibleRowCount(const TouchListPageLayout& layout);
bool isPreviousPageRow(const TouchListPageLayout& layout, int visibleRow);
bool isNextPageRow(const TouchListPageLayout& layout, int visibleRow);
int visibleRowToItemIndex(const TouchListPageLayout& layout, int visibleRow);
}  // namespace TouchList

