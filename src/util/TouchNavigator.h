#pragma once

#include "MappedInputManager.h"
#include "components/themes/BaseTheme.h"

class TouchNavigator final {
 public:
  static bool contains(Rect rect, MappedInputManager::TouchPoint point);
  static bool wasTappedIn(const MappedInputManager& input, Rect rect);

  static int tappedGridIndex(const MappedInputManager& input, Rect rect, int itemCount, int columns);
  static int tappedListIndex(const MappedInputManager& input, Rect rect, int itemCount, int selectedIndex, int rowHeight,
                             int rowGap = 0);
  static int tappedEqualTabIndex(const MappedInputManager& input, Rect rect, int tabCount);
};
