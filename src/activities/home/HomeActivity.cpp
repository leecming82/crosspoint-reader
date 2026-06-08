#include "HomeActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"
#include "util/TouchNavigator.h"

namespace {

uint32_t heapKb(const uint32_t bytes) { return (bytes + 512) / 1024; }
#ifdef CROSSPOINT_BOARD_MURPHY_M4
constexpr unsigned long HOME_INPUT_ARM_DELAY_MS = 250;
#else
constexpr unsigned long HOME_INPUT_ARM_DELAY_MS = 1500;
#endif

std::string homeMemoryStatusText() {
  return "Free: " + std::to_string(heapKb(ESP.getFreeHeap())) +
         "K Max Blk: " + std::to_string(heapKb(ESP.getMaxAllocHeap())) +
         "K Min Free: " + std::to_string(heapKb(ESP.getMinFreeHeap())) + "K";
}

}  // namespace

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsServers) {
    count++;
  }
  return count;
}

int HomeActivity::getMenuButtonCount() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (hasOpdsServers) {
    count++;
  }
  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    count++;
  }
  return count;
}

Rect HomeActivity::getMenuRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  int footerReserved = metrics.buttonHintsHeight;
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  footerReserved = 0;
#endif
  return Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
              pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                            metrics.homeMenuTopOffset + footerReserved)};
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  if (coverBufferStored || coverBuffer) {
    freeCoverBuffer();
    coverRendered = false;
  }

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          epub.generateThumbBmp(coverHeight);
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = xtc.generateThumbBmp(coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasOpdsServers);
  inputArmedAt = millis() + HOME_INPUT_ARM_DELAY_MS;

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  if (handleTouch()) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const bool armed = millis() >= inputArmedAt && !recentsLoading;
    if (armed) {
      activateSelection();
    }
  }
}

void HomeActivity::activateSelection() {
  if (selectorIndex < recentBooks.size()) {
    onSelectBook(recentBooks[selectorIndex].path);
    return;
  }

  const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
  const HomeMenuItem item = indexToMenuItem(menuIndex, hasOpdsServers);
  switch (item) {
    case HomeMenuItem::FILE_BROWSER:
      onFileBrowserOpen();
      break;
    case HomeMenuItem::RECENTS:
      onRecentsOpen();
      break;
    case HomeMenuItem::OPDS_BROWSER:
      onOpdsBrowserOpen();
      break;
    case HomeMenuItem::FILE_TRANSFER:
      onFileTransferOpen();
      break;
    case HomeMenuItem::SETTINGS_MENU:
      onSettingsOpen();
      break;
    default:
      break;
  }
}

bool HomeActivity::handleTouch() {
  if (!mappedInput.wasTapped()) {
    return false;
  }

  if (millis() < inputArmedAt) {
    return true;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int menuSelectedIndex = metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size();
  const Rect menuRect = getMenuRect();
  const int tappedMenuIndex =
      TouchNavigator::tappedListIndex(mappedInput, menuRect, getMenuButtonCount(), menuSelectedIndex,
                                      metrics.menuRowHeight, metrics.menuSpacing);
  if (tappedMenuIndex >= 0) {
    selectorIndex = metrics.homeContinueReadingInMenu ? tappedMenuIndex : static_cast<int>(recentBooks.size()) + tappedMenuIndex;
    activateSelection();
    return true;
  }

  const Rect coverRect{0, metrics.homeTopPadding, renderer.getScreenWidth(), metrics.homeCoverTileHeight};
  const bool coverHit = !metrics.homeContinueReadingInMenu && !recentsLoading && !recentBooks.empty() &&
                        TouchNavigator::wasTappedIn(mappedInput, coverRect);
  if (coverHit) {
    selectorIndex = 0;
    activateSelection();
    return true;
  }

  return true;
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  const std::string headerTitle =
      !recentBooks.empty() ? StringUtils::uiSafeBookTitle(recentBooks[0].title, recentBooks[0].path) : "";
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? headerTitle.c_str() : nullptr);

  if (SETTINGS.homeMemoryStatus) {
    const std::string memoryText = homeMemoryStatusText();
    const int textX = metrics.contentSidePadding;
    const int batteryReserveWidth = 95;
    const int maxTextWidth = std::max(0, pageWidth - textX - batteryReserveWidth);
    const int textY = SETTINGS.uiTheme == CrossPointSettings::UI_THEME::ROUNDEDRAFF ? metrics.topPadding + 34
                                                                                    : metrics.topPadding + 5;
    const std::string displayText =
        renderer.truncatedText(SMALL_FONT_ID, memoryText.c_str(), maxTextWidth, EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, textX, textY, displayText.c_str());
  }

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

  if (hasOpdsServers) {
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Library);
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(
      renderer, getMenuRect(),
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
