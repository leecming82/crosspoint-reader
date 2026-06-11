#include "ClearCacheActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/TouchNavigator.h"
#include "util/TouchUi.h"

#ifdef CROSSPOINT_BOARD_MURPHY_M4
#include "TtfReaderMetrics.h"
#endif

namespace {
void drawTouchButton(const GfxRenderer& renderer, const Rect rect, const char* label) {
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, 6, true);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, rect.x + (rect.width - textWidth) / 2, textY, label, true, EpdFontFamily::BOLD);
}
}  // namespace

void ClearCacheActivity::onEnter() {
  Activity::onEnter();

  state = WARNING;
  requestUpdate();
}

void ClearCacheActivity::onExit() { Activity::onExit(); }

void ClearCacheActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  TouchUi::drawHeaderWithBack(renderer, screen, tr(STR_CLEAR_READING_CACHE));
#else
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLEAR_READING_CACHE));
#endif

  if (state == WARNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 60, tr(STR_CLEAR_CACHE_WARNING_1), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, tr(STR_CLEAR_CACHE_WARNING_2), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CLEAR_CACHE_WARNING_3), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, tr(STR_CLEAR_CACHE_WARNING_4), true);

#ifdef CROSSPOINT_BOARD_MURPHY_M4
    drawTouchButton(renderer, cancelButtonRect(), tr(STR_CANCEL));
    drawTouchButton(renderer, confirmButtonRect(), tr(STR_CLEAR_BUTTON));
#else
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CLEAR_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif
    renderer.displayBuffer();
    return;
  }

  if (state == CLEARING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_CLEARING_CACHE));
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CACHE_CLEARED), true, EpdFontFamily::BOLD);
    std::string resultText = std::to_string(clearedCount) + " " + std::string(tr(STR_ITEMS_REMOVED));
    if (failedCount > 0) {
      resultText += ", " + std::to_string(failedCount) + " " + std::string(tr(STR_FAILED_LOWER));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());

#ifdef CROSSPOINT_BOARD_MURPHY_M4
    drawTouchButton(renderer, backButtonRect(), tr(STR_BACK));
#else
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CLEAR_CACHE_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CHECK_SERIAL_OUTPUT));

#ifdef CROSSPOINT_BOARD_MURPHY_M4
    drawTouchButton(renderer, backButtonRect(), tr(STR_BACK));
#else
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif
    renderer.displayBuffer();
    return;
  }
}

Rect ClearCacheActivity::cancelButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int buttonHeight = 56;
  const int gap = metrics.contentSidePadding;
  const int width = (renderer.getScreenWidth() - metrics.contentSidePadding * 2 - gap) / 2;
  const int y = renderer.getScreenHeight() - buttonHeight - 16;
  return Rect{metrics.contentSidePadding, y, width, buttonHeight};
}

Rect ClearCacheActivity::confirmButtonRect() const {
  const Rect cancelRect = cancelButtonRect();
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{cancelRect.x + cancelRect.width + metrics.contentSidePadding, cancelRect.y, cancelRect.width,
              cancelRect.height};
}

Rect ClearCacheActivity::backButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int buttonHeight = 56;
  return Rect{metrics.contentSidePadding, renderer.getScreenHeight() - buttonHeight - 16,
              renderer.getScreenWidth() - metrics.contentSidePadding * 2, buttonHeight};
}

void ClearCacheActivity::confirmClear() {
  LOG_DBG("CLEAR_CACHE", "User confirmed, starting cache clear");
  {
    RenderLock lock(*this);
    state = CLEARING;
  }
  requestUpdateAndWait();

  clearCache();
}

void ClearCacheActivity::clearCache() {
  LOG_DBG("CLEAR_CACHE", "Clearing all caches...");

  clearedCount = 0;
  failedCount = 0;

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const bool hadTtfGlyphCache = Storage.exists("/.crosspoint/ttf_cache");
  if (TTF_READER_METRICS.clearPersistentGlyphCache()) {
    if (hadTtfGlyphCache) clearedCount++;
  } else {
    failedCount++;
  }
#endif

  // Open .crosspoint directory
  auto root = Storage.open("/.crosspoint");
  if (!root || !root.isDirectory()) {
    LOG_DBG("CLEAR_CACHE", "Cache directory does not exist");
    if (root) root.close();
    state = failedCount == 0 ? SUCCESS : FAILED;
    requestUpdate();
    return;
  }

  char name[128];

  // Iterate through all entries in the directory
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    String itemName(name);

    // Only delete directories matching known book cache names.
    if (file.isDirectory() && isBookCacheDirectoryName(itemName.c_str())) {
      String fullPath = "/.crosspoint/" + itemName;
      LOG_DBG("CLEAR_CACHE", "Removing cache: %s", fullPath.c_str());

      file.close();  // Close before attempting to delete

      if (Storage.removeDir(fullPath.c_str())) {
        clearedCount++;
      } else {
        LOG_ERR("CLEAR_CACHE", "Failed to remove: %s", fullPath.c_str());
        failedCount++;
      }
    } else {
      file.close();
    }
  }
  root.close();

  LOG_DBG("CLEAR_CACHE", "All caches cleared: %d removed, %d failed", clearedCount, failedCount);

  state = failedCount == 0 ? SUCCESS : FAILED;
  requestUpdate();
}

void ClearCacheActivity::loop() {
  if (state == WARNING) {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
    if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
      LOG_DBG("CLEAR_CACHE", "User cancelled");
      goBack();
      return;
    }
    if (TouchNavigator::wasTappedIn(mappedInput, cancelButtonRect())) {
      LOG_DBG("CLEAR_CACHE", "User cancelled");
      goBack();
      return;
    }
    if (TouchNavigator::wasTappedIn(mappedInput, confirmButtonRect())) {
      confirmClear();
      return;
    }
#endif
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      confirmClear();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      LOG_DBG("CLEAR_CACHE", "User cancelled");
      goBack();
    }
    return;
  }

  if (state == SUCCESS || state == FAILED) {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
    if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
      goBack();
      return;
    }
    if (TouchNavigator::wasTappedIn(mappedInput, backButtonRect())) {
      goBack();
      return;
    }
#endif
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}
