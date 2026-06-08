#include "MurphyTheme.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <string>

#include "components/icons/book.h"
#include "components/icons/book24.h"
#include "components/icons/bookmark.h"
#include "components/icons/file24.h"
#include "components/icons/folder.h"
#include "components/icons/folder24.h"
#include "components/icons/hotspot.h"
#include "components/icons/image24.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/text24.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "fontIds.h"

namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int maxListValueWidth = 200;
constexpr int mainMenuIconSize = 32;
constexpr int listIconSize = 24;
constexpr int iconVisualOffsetY = 5;
constexpr int listTitleVisualOffsetY = 1;

const uint8_t* iconForName(const UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder:
      return FolderIcon;
    case UIIcon::Book:
      return BookIcon;
    case UIIcon::Recent:
      return RecentIcon;
    case UIIcon::Settings:
      return Settings2Icon;
    case UIIcon::Transfer:
      return TransferIcon;
    case UIIcon::Library:
      return LibraryIcon;
    case UIIcon::Wifi:
      return WifiIcon;
    case UIIcon::Hotspot:
      return HotspotIcon;
    case UIIcon::Bookmark:
      return BookmarkIcon;
    default:
      return nullptr;
  }
}

const uint8_t* listIconForName(const UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder:
      return Folder24Icon;
    case UIIcon::Text:
      return Text24Icon;
    case UIIcon::Image:
      return Image24Icon;
    case UIIcon::Book:
      return Book24Icon;
    case UIIcon::File:
      return File24Icon;
    default:
      return nullptr;
  }
}
}  // namespace

void MurphyTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                  const char* btn4) const {
  (void)renderer;
  (void)btn1;
  (void)btn2;
  (void)btn3;
  (void)btn4;
}

void MurphyTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                 const std::function<std::string(int index)>& buttonLabel,
                                 const std::function<UIIcon(int index)>& rowIcon) const {
  (void)selectedIndex;

  const int tileWidth = rect.width - MurphyMetrics::values.contentSidePadding * 2;

  for (int i = 0; i < buttonCount; ++i) {
    Rect tileRect =
        Rect{rect.x + MurphyMetrics::values.contentSidePadding,
             rect.y + i * (MurphyMetrics::values.menuRowHeight + MurphyMetrics::values.menuSpacing), tileWidth,
             MurphyMetrics::values.menuRowHeight};

    renderer.drawRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, 1, cornerRadius, true);

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    int textX = tileRect.x + 16;
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = tileRect.y + (MurphyMetrics::values.menuRowHeight - lineHeight) / 2;

    if (rowIcon != nullptr) {
      const uint8_t* iconBitmap = iconForName(rowIcon(i));
      if (iconBitmap != nullptr) {
        const int iconY = tileRect.y + (tileRect.height - mainMenuIconSize) / 2 + iconVisualOffsetY;
        renderer.drawIcon(iconBitmap, textX, iconY, mainMenuIconSize, mainMenuIconSize);
        textX += mainMenuIconSize + hPaddingInSelection + 2;
      }
    }

    renderer.drawText(UI_12_FONT_ID, textX, textY, label, true, EpdFontFamily::BOLD);
  }
}

void MurphyTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                           const std::function<std::string(int index)>& rowTitle,
                           const std::function<std::string(int index)>& rowSubtitle,
                           const std::function<UIIcon(int index)>& rowIcon,
                           const std::function<std::string(int index)>& rowValue, bool highlightValue,
                           const std::function<bool(int index)>& rowDimmed) const {
  (void)highlightValue;

  const int rowHeight =
      (rowSubtitle != nullptr) ? MurphyMetrics::values.listWithSubtitleRowHeight : MurphyMetrics::values.listRowHeight;
  const int pageItems = std::max(1, rect.height / rowHeight);

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  const bool showScroll = totalPages > 1;
  if (showScroll) {
    const int scrollAreaHeight = rect.height;
    const int scrollBarHeight = std::max(1, (scrollAreaHeight * pageItems) / itemCount);
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - MurphyMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - MurphyMetrics::values.scrollBarWidth, scrollBarY, MurphyMetrics::values.scrollBarWidth,
                      scrollBarHeight, true);
  }

  const int contentWidth =
      rect.width -
      (showScroll ? (MurphyMetrics::values.scrollBarWidth + MurphyMetrics::values.scrollBarRightOffset) : 1);
  const int pageStartIndex = (std::max(0, selectedIndex) / pageItems) * pageItems;
  const int rowX = rect.x + MurphyMetrics::values.contentSidePadding;
  const int rowWidth = contentWidth - MurphyMetrics::values.contentSidePadding * 2;
  const int iconX = rowX + hPaddingInSelection + 2;
  const int baseTextX = rowIcon != nullptr ? iconX + listIconSize + hPaddingInSelection + 2 : rowX + 14;

  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int rowY = rect.y + (i % pageItems) * rowHeight;
    const Rect rowRect{rowX, rowY + 2, rowWidth, rowHeight - 4};
    renderer.drawRoundedRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height, 1, cornerRadius, true);

    int rowTextWidth = rowWidth - (baseTextX - rowX) - hPaddingInSelection;
    std::string valueText;
    int valueWidth = 0;
    if (rowValue != nullptr) {
      valueText = renderer.truncatedText(UI_10_FONT_ID, rowValue(i).c_str(), maxListValueWidth);
      valueWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str()) + hPaddingInSelection;
      rowTextWidth -= valueWidth;
    }

    auto itemName = rowTitle(i);
    auto item = renderer.truncatedText(UI_12_FONT_ID, itemName.c_str(), rowTextWidth);
    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int subtitleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int subtitleGap = 4;
    const int contentCenterY = rowRect.y + rowRect.height / 2;
    const bool hasSubtitle = rowSubtitle != nullptr;
    const int textBlockHeight = hasSubtitle ? titleLineHeight + subtitleGap + subtitleLineHeight : titleLineHeight;
    const int titleY = contentCenterY - textBlockHeight / 2 + listTitleVisualOffsetY;

    if (rowIcon != nullptr) {
      const uint8_t* iconBitmap = listIconForName(rowIcon(i));
      if (iconBitmap != nullptr) {
        const int iconY = contentCenterY - listIconSize / 2 + iconVisualOffsetY;
        renderer.drawIcon(iconBitmap, iconX, iconY, listIconSize, listIconSize);
      }
    }

    renderer.drawText(UI_12_FONT_ID, baseTextX, titleY, item.c_str(), true);

    if (rowDimmed && rowDimmed(i)) {
      const int titleWidth = renderer.getTextWidth(UI_12_FONT_ID, item.c_str());
      const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
      for (int py = titleY; py < titleY + lineH; py++)
        for (int px = baseTextX; px < baseTextX + titleWidth; px++)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }

    if (hasSubtitle) {
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
      renderer.drawText(SMALL_FONT_ID, baseTextX, titleY + titleLineHeight + subtitleGap, subtitle.c_str(), true);
    }

    if (!valueText.empty()) {
      const int valueY = rowY + (rowHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - MurphyMetrics::values.contentSidePadding - valueWidth,
                        valueY, valueText.c_str(), true);
    }
  }
}
