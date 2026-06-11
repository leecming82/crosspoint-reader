#include "ReaderFontSizeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "TtfReaderMetrics.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchNavigator.h"
#include "util/TouchUi.h"

namespace {
constexpr const char* PREVIEW_LINE_EN = "Read ABC";
constexpr const char* PREVIEW_LINE_NUM = "123";
constexpr const char* PREVIEW_LINE_JP = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";
constexpr const char* PREVIEW_LINE_KANA =
    "\xE3\x81\x82\xE3\x81\x84 "
    "\xE3\x80\x8C\xE6\x9C\xAC\xE3\x80\x8D";
constexpr const char* PREVIEW_LINE_PUNCT =
    "\xE3\x80\x81"
    "\xE3\x80\x82";
}  // namespace

ReaderFontSizeActivity::ReaderFontSizeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const int initialSize, std::string previewPath,
                                               const uint32_t previewFileSize, const bool returnSelectionOnly)
    : Activity("ReaderFontSize", renderer, mappedInput),
      originalSize_(initialSize),
      value_(initialSize),
      returnSelectionOnly_(returnSelectionOnly),
      previewPath_(std::move(previewPath)),
      previewFileSize_(previewFileSize) {}

int ReaderFontSizeActivity::clampedValue(const int value) const {
  const int stepped = ((value + SMALL_STEP / 2) / SMALL_STEP) * SMALL_STEP;
  return std::clamp(stepped, MIN_SIZE, MAX_SIZE);
}

void ReaderFontSizeActivity::onEnter() {
  Activity::onEnter();
  if (!returnSelectionOnly_) {
    originalSize_ = SETTINGS.readerTtfSizePx;
  }
  value_ = clampedValue(originalSize_);
  accepted_ = false;
  requestUpdate();
}

void ReaderFontSizeActivity::onExit() {
  if (!accepted_) {
    if (!returnSelectionOnly_) {
      SETTINGS.readerTtfSizePx = static_cast<uint8_t>(originalSize_);
    }
  }
  Activity::onExit();
}

void ReaderFontSizeActivity::adjustValue(const int delta) {
  const int next = clampedValue(value_ + delta);
  if (next == value_) return;
  value_ = next;
  requestUpdate();
}

void ReaderFontSizeActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void ReaderFontSizeActivity::accept() {
  accepted_ = true;
  if (!returnSelectionOnly_) {
    SETTINGS.readerTtfSizePx = static_cast<uint8_t>(value_);
  }
  setResult(IntervalResult{static_cast<uint32_t>(value_)});
  finish();
}

Rect ReaderFontSizeActivity::minusButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int buttonHeight = 54;
  const int gap = metrics.contentSidePadding;
  const int buttonWidth = (renderer.getScreenWidth() - metrics.contentSidePadding * 2 - gap) / 2;
  const int y = renderer.getScreenHeight() - buttonHeight - 86;
  return Rect{metrics.contentSidePadding, y, buttonWidth, buttonHeight};
}

Rect ReaderFontSizeActivity::plusButtonRect() const {
  const Rect minus = minusButtonRect();
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{minus.x + minus.width + metrics.contentSidePadding, minus.y, minus.width, minus.height};
}

void ReaderFontSizeActivity::loop() {
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
    cancel();
    return;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, minusButtonRect())) {
    adjustValue(-SMALL_STEP);
    return;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, plusButtonRect())) {
    adjustValue(SMALL_STEP);
    return;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::bottomActionRect(renderer))) {
    accept();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    accept();
    return;
  }

  buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustValue(-SMALL_STEP); });
  buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustValue(SMALL_STEP); });
  buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { adjustValue(LARGE_STEP); });
  buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { adjustValue(-LARGE_STEP); });
}

void ReaderFontSizeActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  const auto& metrics = UITheme::getInstance().getMetrics();
  TouchUi::drawHeaderWithBack(renderer, screen, tr(STR_FONT_SIZE));

  char valueText[32];
  snprintf(valueText, sizeof(valueText), "%d px", value_);
  renderer.drawCenteredText(UI_12_FONT_ID, 86, valueText, true, EpdFontFamily::BOLD);

  const int barWidth = std::min(360, std::max(0, renderer.getScreenWidth() - 40));
  constexpr int barHeight = 16;
  const int barX = std::max(0, (renderer.getScreenWidth() - barWidth) / 2);
  const int barY = 128;
  renderer.drawRect(barX, barY, barWidth, barHeight);
  const int fillWidth = (barWidth - 4) * (value_ - MIN_SIZE) / std::max(1, MAX_SIZE - MIN_SIZE);
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }
  renderer.fillRect(std::max(barX + 2, barX + 2 + fillWidth - 2), barY - 4, 4, barHeight + 8, true);

  const int previewY = 160;
  const int previewX = metrics.contentSidePadding;
  const int previewWidth = renderer.getScreenWidth() - metrics.contentSidePadding * 2;
  const int previewHeight = minusButtonRect().y - previewY - 12;
  renderer.drawRect(previewX, previewY, previewWidth, previewHeight);

  bool hasTtf = false;
  bool loaded = false;
  if (returnSelectionOnly_) {
    hasTtf = !previewPath_.empty();
    loaded = hasTtf && TTF_READER_METRICS.ensureLoaded(previewPath_.c_str(), static_cast<uint8_t>(value_),
                                                       previewFileSize_);
  } else {
    SETTINGS.readerTtfSizePx = static_cast<uint8_t>(value_);
    hasTtf = SETTINGS.readerTtfPath[0] != '\0';
    loaded = hasTtf && TTF_READER_METRICS.ensureLoadedFromSettings();
  }
  if (loaded) {
    renderer.setReaderFontMetricsProvider(&TTF_READER_METRICS);
    const int fontId = TTF_READER_METRICS.fontId();
    const int lineHeight = std::max(1, renderer.getLineHeight(fontId));
    const int gap = std::max(2, lineHeight / 6);
    int y = previewY + 12;
    renderer.drawText(fontId, previewX + 12, y, PREVIEW_LINE_EN, true);
    y += lineHeight + gap;
    renderer.drawText(fontId, previewX + 12, y, PREVIEW_LINE_NUM, true);
    y += lineHeight + gap;
    renderer.drawText(fontId, previewX + 12, y, PREVIEW_LINE_JP, true);
    y += lineHeight + gap;
    if (y + lineHeight < previewY + previewHeight - 8) {
      renderer.drawText(fontId, previewX + 12, y, PREVIEW_LINE_KANA, true);
      y += lineHeight + gap;
    }
    if (y + lineHeight < previewY + previewHeight - 8) {
      renderer.drawText(fontId, previewX + 12, y, PREVIEW_LINE_PUNCT, true);
    }
  } else {
    renderer.drawText(UI_10_FONT_ID, previewX + 12, previewY + 24, "Select a TTF font first.", true);
    renderer.drawText(UI_10_FONT_ID, previewX + 12, previewY + 48, "Copy .ttf files into /TTF.", true);
  }

  TouchUi::drawTouchButton(renderer, minusButtonRect(), "-");
  TouchUi::drawTouchButton(renderer, plusButtonRect(), "+");
  TouchUi::drawTouchButton(renderer, TouchUi::bottomActionRect(renderer), tr(STR_OK_BUTTON));

  renderer.displayBuffer();
}
