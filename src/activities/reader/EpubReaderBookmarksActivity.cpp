#include "EpubReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <util/BookmarkUtil.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchList.h"
#include "util/TouchNavigator.h"
#include "util/TouchUi.h"

namespace {
constexpr int ENTER_DELETE_MODE_MS = 700;
constexpr int DELETE_MODE_OFF = 0;
constexpr int DELETE_MODE_DISPLAY = 1;
constexpr int DELETE_MODE_CONFIRM = 2;

// Layout constants used in renderScreen
constexpr int LINE_HEIGHT = 60;

bool hasStoredLocalPosition(const BookmarkEntry& bookmark, const std::shared_ptr<Epub>& epub) {
  return epub && bookmark.computedChapterPageCount > 0 && bookmark.computedSpineIndex < epub->getSpineItemsCount();
}

}  // namespace

void EpubReaderBookmarksActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  const std::string path = BookmarkUtil::getBookmarkPath(epubPath);
  if (Storage.exists(path.c_str())) {
    String json = Storage.readFile(path.c_str());
    if (json.isEmpty()) {
      LOG_ERR("EPB", "Failed to load bookmarks from %s. Empty bookmark file", path.c_str());
      bookmarks.clear();
      bookmarks.shrink_to_fit();
    } else {
      JsonSettingsIO::loadBookmarks(bookmarks, json.c_str());

      // pre-compute bookmark page values for quicker rendering
      for (auto& bookmark : bookmarks) {
        if (hasStoredLocalPosition(bookmark, epub)) {
          if (bookmark.computedChapterProgress >= bookmark.computedChapterPageCount) {
            bookmark.computedChapterProgress = bookmark.computedChapterPageCount - 1;
          }
          continue;
        }
        CrossPointPosition pos = ProgressMapper::toCrossPoint(epub, {bookmark.xpath, bookmark.percentage}, renderer);
        bookmark.computedSpineIndex = pos.spineIndex;
        bookmark.computedChapterPageCount = pos.totalPages;
        bookmark.computedChapterProgress = pos.pageNumber;
      }
    }
  } else {
    LOG_DBG("EPB", "No bookmark file found at %s, starting with empty bookmarks", path.c_str());
    bookmarks.clear();
    bookmarks.shrink_to_fit();
  }
  LOG_DBG("EPB", "Loaded %d bookmarks for book: %s", static_cast<int>(bookmarks.size()), epubPath.c_str());

  // Trigger first update
  requestUpdate();
}

void EpubReaderBookmarksActivity::onExit() { Activity::onExit(); }

int EpubReaderBookmarksActivity::getGutterBottom(const GfxRenderer& renderer) const {
  const auto orientation = renderer.getOrientation();
  const bool isPortrait = orientation == GfxRenderer::Orientation::Portrait;
  return isPortrait ? 75 : 40;  // Reserve vertical space for button hints at the bottom
}

int EpubReaderBookmarksActivity::getListHeight(const GfxRenderer& renderer) const {
  const auto pageHeight = renderer.getScreenHeight();
  return pageHeight - getGutterBottom(renderer) - LINE_HEIGHT;  // Reserve vertical space for title and button hints
}

Rect EpubReaderBookmarksActivity::cancelButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int buttonHeight = 56;
  const int gap = metrics.contentSidePadding;
  const int width = (renderer.getScreenWidth() - metrics.contentSidePadding * 2 - gap) / 2;
  const int y = renderer.getScreenHeight() - buttonHeight - 16;
  return Rect{metrics.contentSidePadding, y, width, buttonHeight};
}

Rect EpubReaderBookmarksActivity::deleteButtonRect() const {
  const Rect cancelRect = cancelButtonRect();
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{cancelRect.x + cancelRect.width + metrics.contentSidePadding, cancelRect.y, cancelRect.width,
              cancelRect.height};
}

Rect EpubReaderBookmarksActivity::contentRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = renderer.getScreenHeight() - metrics.verticalSpacing;
  return Rect{screen.x, contentTop, screen.width, std::max(0, contentBottom - contentTop)};
#else
  return Rect{0, LINE_HEIGHT, renderer.getScreenWidth(), getListHeight(renderer)};
#endif
}

void EpubReaderBookmarksActivity::openSelectedBookmark() {
  if (bookmarks.empty()) {
    return;
  }

  auto bookmark = bookmarks.at(selectorIndex);
  if (hasStoredLocalPosition(bookmark, epub)) {
    const int page = std::min<int>(bookmark.computedChapterProgress, bookmark.computedChapterPageCount - 1);
    setResult(ProgressChangeResult{static_cast<int>(bookmark.computedSpineIndex), page});
  } else {
    CrossPointPosition pos = ProgressMapper::toCrossPoint(epub, {bookmark.xpath, bookmark.percentage}, renderer);
    setResult(ProgressChangeResult{pos.spineIndex, pos.pageNumber});
  }
  finish();
}

void EpubReaderBookmarksActivity::cancelDelete() {
  confirmingDelete = DELETE_MODE_OFF;
  requestUpdate();
}

void EpubReaderBookmarksActivity::deleteSelectedBookmark() {
  if (bookmarks.empty() || selectorIndex < 0 || selectorIndex >= static_cast<int>(bookmarks.size())) {
    confirmingDelete = DELETE_MODE_OFF;
    requestUpdate();
    return;
  }

  bookmarks.erase(bookmarks.begin() + selectorIndex);
  const std::string path = BookmarkUtil::getBookmarkPath(epubPath);
  Storage.mkdir(BookmarkUtil::getBookmarksDir().c_str());
  if (!JsonSettingsIO::saveBookmarks(bookmarks, path.c_str())) {
    LOG_ERR("EPB", "Failed to save bookmarks after delete");
  }

  if (selectorIndex >= static_cast<int>(bookmarks.size()) && selectorIndex > 0) {
    selectorIndex--;
  }

  confirmingDelete = DELETE_MODE_OFF;
  requestUpdate();
}

bool EpubReaderBookmarksActivity::handleTouch() {
#ifndef CROSSPOINT_BOARD_MURPHY_M4
  return false;
#else
  if (confirmingDelete >= DELETE_MODE_DISPLAY) {
    if (TouchNavigator::wasTappedIn(mappedInput, cancelButtonRect())) {
      cancelDelete();
      return true;
    }
    if (TouchNavigator::wasTappedIn(mappedInput, deleteButtonRect())) {
      deleteSelectedBookmark();
      return true;
    }
    return mappedInput.wasTapped();
  }

  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }

  const Rect listBounds = contentRect();
  const int rowHeight = UITheme::getInstance().getMetrics().listWithSubtitleRowHeight;
  const int listRows = std::max(1, listBounds.height / rowHeight);
  const auto layout = TouchList::calculatePageLayout(selectorIndex, static_cast<int>(bookmarks.size()), listRows);

  const auto handleVisibleRow = [this, listRows, layout](const int visibleRow, const bool open) {
    if (visibleRow < 0) {
      return false;
    }
    if (TouchList::isPreviousPageRow(layout, visibleRow)) {
      selectorIndex =
          TouchList::calculatePageLayout(std::max(0, layout.start - 1), static_cast<int>(bookmarks.size()), listRows)
              .start;
      requestUpdate();
      return true;
    }
    if (TouchList::isNextPageRow(layout, visibleRow)) {
      selectorIndex = std::min(static_cast<int>(bookmarks.size()) - 1, layout.start + layout.itemCount);
      requestUpdate();
      return true;
    }

    const int itemIndex = TouchList::visibleRowToItemIndex(layout, visibleRow);
    if (itemIndex >= 0) {
      selectorIndex = itemIndex;
      if (open) {
        openSelectedBookmark();
      } else {
        confirmingDelete = DELETE_MODE_DISPLAY;
        requestUpdate();
      }
      return true;
    }
    return false;
  };

  if (mappedInput.wasTouchLongPressed()) {
    const auto point = mappedInput.lastTouchLongPress();
    if (TouchNavigator::contains(listBounds, point)) {
      const int visibleRow = (point.y - listBounds.y) / rowHeight;
      return handleVisibleRow(visibleRow, false);
    }
  }

  const int visibleRow =
      TouchNavigator::tappedListIndex(mappedInput, listBounds, TouchList::visibleRowCount(layout), 0, rowHeight, 0);
  if (handleVisibleRow(visibleRow, true)) {
    return true;
  }

  return mappedInput.wasTapped();
#endif
}

void EpubReaderBookmarksActivity::loop() {
  // Delete confirmation mode
  if (confirmingDelete >= DELETE_MODE_DISPLAY) {
    if (handleTouch()) {
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (confirmingDelete == DELETE_MODE_DISPLAY) {
        confirmingDelete = DELETE_MODE_CONFIRM;  // first confirmation, update text
        requestUpdate();
        return;
      }
      deleteSelectedBookmark();
      return;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelDelete();
      return;
    }
  }

  if (handleTouch()) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {  // Open
    openSelectedBookmark();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() > ENTER_DELETE_MODE_MS) {
    if (bookmarks.empty()) {
      return;
    }
    confirmingDelete = DELETE_MODE_DISPLAY;
    requestUpdate();
  }

  buttonNavigator.onNextRelease([this] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, bookmarks.size());
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, bookmarks.size());
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, bookmarks.size(),
                                                   GUI.getListPageItems(getListHeight(renderer), true));
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, bookmarks.size(),
                                                       GUI.getListPageItems(getListHeight(renderer), true));
    requestUpdate();
  });
}

void EpubReaderBookmarksActivity::render(RenderLock&&) {
  renderer.clearScreen();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  TouchUi::drawHeaderWithBack(renderer, screen, tr(STR_BOOKMARKS));

  const auto getBookmarkTitle = [this](int index) {
    return bookmarks.at(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index).summary;
  };
  const auto getBookmarkSubtitle = [this](int index) {
    auto bookmark = bookmarks.at(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index);
    auto tocIndex = epub->getTocIndexForSpineIndex(bookmark.computedSpineIndex);
    auto tocTitle = (tocIndex >= 0) ? (epub->getTocItem(tocIndex)).title : tr(STR_UNNAMED);
    const int percent = static_cast<int>(std::clamp(bookmark.percentage, 0.0f, 1.0f) * 100.0f + 0.5f);
    return std::to_string(percent) + "% - " + std::to_string(bookmark.computedChapterProgress + 1) + "/" +
           std::to_string(bookmark.computedChapterPageCount) + " - " + tocTitle;
  };

  if (confirmingDelete >= DELETE_MODE_DISPLAY) {
    GUI.drawHelpText(renderer, Rect{0, renderer.getScreenHeight() / 2 - LINE_HEIGHT * 2, renderer.getScreenWidth(),
                                    LINE_HEIGHT},
                     tr(STR_CONFIRM_DELETE_BOOKMARK));
    if (!bookmarks.empty()) {
      GUI.drawList(renderer, Rect{0, renderer.getScreenHeight() / 2, renderer.getScreenWidth(), LINE_HEIGHT}, 1, 0,
                   getBookmarkTitle, getBookmarkSubtitle);
    }
    TouchUi::drawTouchButton(renderer, cancelButtonRect(), tr(STR_CANCEL));
    TouchUi::drawTouchButton(renderer, deleteButtonRect(), tr(STR_DELETE));
    renderer.displayBuffer();
    return;
  }

  if (bookmarks.empty()) {
    GUI.drawHelpText(renderer, contentRect(), tr(STR_BOOKMARK_INSTRUCTIONS));
    renderer.displayBuffer();
    return;
  }

  const Rect listBounds = contentRect();
  const int rowHeight = metrics.listWithSubtitleRowHeight;
  const int listRows = std::max(1, listBounds.height / rowHeight);
  const auto layout = TouchList::calculatePageLayout(selectorIndex, static_cast<int>(bookmarks.size()), listRows);
  GUI.drawList(
      renderer, listBounds, TouchList::visibleRowCount(layout), -1,
      [this, layout](int visibleRow) {
        if (TouchList::isPreviousPageRow(layout, visibleRow)) {
          return std::string(tr(STR_PREV_PAGE));
        }
        if (TouchList::isNextPageRow(layout, visibleRow)) {
          return std::string(tr(STR_NEXT_PAGE));
        }
        const int itemIndex = TouchList::visibleRowToItemIndex(layout, visibleRow);
        return itemIndex >= 0 ? bookmarks.at(itemIndex).summary : std::string();
      },
      [this, layout, getBookmarkSubtitle](int visibleRow) {
        const int itemIndex = TouchList::visibleRowToItemIndex(layout, visibleRow);
        return itemIndex >= 0 ? getBookmarkSubtitle(itemIndex) : std::string();
      },
      nullptr, nullptr, true);
  if (layout.previous) {
    TouchUi::drawCenteredPagerRow(renderer, listBounds, 0, tr(STR_PREV_PAGE));
  }
  if (layout.next) {
    TouchUi::drawCenteredPagerRow(renderer, listBounds, TouchList::visibleRowCount(layout) - 1, tr(STR_NEXT_PAGE));
  }

  renderer.displayBuffer();
  return;
  }
#endif

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  // Landscape orientation: reserve a horizontal gutter for button hints.
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Inverted portrait: reserve vertical space for hints at the top.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const bool isPortrait = orientation == GfxRenderer::Orientation::Portrait;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 40 : 0;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int hintGutterBottom = getGutterBottom(renderer);
  const int contentY = hintGutterHeight;
  const int listY = contentY + LINE_HEIGHT;  // Reserve vertical space for title
  const int listHeight = getListHeight(renderer);
  const int numBookmarks = bookmarks.size();

  // Manual centering to honor content gutters.
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_BOOKMARKS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_BOOKMARKS), true, EpdFontFamily::BOLD);

  const auto getBookmarkTitle = [this](int index) {
    return bookmarks.at(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index).summary;
  };
  const auto getBookmarkSubtitle = [this](int index) {
    auto bookmark = bookmarks.at(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index);
    auto tocIndex = epub->getTocIndexForSpineIndex(bookmark.computedSpineIndex);
    auto tocTitle = (tocIndex >= 0) ? (epub->getTocItem(tocIndex)).title : tr(STR_UNNAMED);
    const int percent = static_cast<int>(std::clamp(bookmark.percentage, 0.0f, 1.0f) * 100.0f + 0.5f);
    return std::to_string(percent) + "% - " + std::to_string(bookmark.computedChapterProgress + 1) + "/" +
           std::to_string(bookmark.computedChapterPageCount) + " - " + tocTitle;
  };
  const auto getBookmarkIcon = [isPortrait](int index) {
    // only enabled icon in portrait mode due to limitation with rotating icons for other orientations
    return isPortrait ? UIIcon::Bookmark : UIIcon::None;
  };

  if (numBookmarks > 0) {
    if (confirmingDelete >= DELETE_MODE_DISPLAY) {
      GUI.drawHelpText(renderer, Rect{0, pageHeight / 2 - LINE_HEIGHT * 2, contentWidth, LINE_HEIGHT},
                       tr(STR_CONFIRM_DELETE_BOOKMARK));

      // render list with just the selected item for the user to confirm to delete
      GUI.drawList(renderer, Rect{contentX, pageHeight / 2, contentWidth, LINE_HEIGHT}, 1, 0, getBookmarkTitle,
                   getBookmarkSubtitle, getBookmarkIcon);
    } else {
      GUI.drawList(renderer, Rect{contentX, listY, contentWidth, listHeight}, numBookmarks, selectorIndex,
                   getBookmarkTitle, getBookmarkSubtitle, getBookmarkIcon);

      GUI.drawHelpText(renderer, Rect{contentX, pageHeight - hintGutterBottom, contentWidth, LINE_HEIGHT},
                       tr(STR_HOLD_CONFIRM_TO_DELETE));
    }
  } else {
    GUI.drawHelpText(renderer, Rect{contentX, LINE_HEIGHT * 2, contentWidth, LINE_HEIGHT},
                     tr(STR_BOOKMARK_INSTRUCTIONS));
  }

#if defined(CROSSPOINT_BOARD_MURPHY_M4)
  if (confirmingDelete >= DELETE_MODE_DISPLAY) {
    TouchUi::drawTouchButton(renderer, cancelButtonRect(), tr(STR_CANCEL));
    TouchUi::drawTouchButton(renderer, deleteButtonRect(), tr(STR_DELETE));
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), bookmarks.size() > 0 ? tr(STR_OPEN) : "", tr(STR_DIR_UP),
                                              tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
#else
  const auto backLabel = confirmingDelete >= DELETE_MODE_DISPLAY ? tr(STR_CANCEL) : tr(STR_BACK);
  const auto confirmLabel =
      bookmarks.size() > 0 ? (confirmingDelete >= DELETE_MODE_DISPLAY ? tr(STR_DELETE) : tr(STR_OPEN)) : "";
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

  renderer.displayBuffer();
}
