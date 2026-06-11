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
                                               const int initialSize, const int initialWeight, std::string previewPath,
                                               const uint32_t previewFileSize, const bool returnSelectionOnly)
    : Activity("ReaderFontSize", renderer, mappedInput),
      originalSize_(initialSize),
      originalWeight_(initialWeight),
      value_(initialSize),
      weight_(initialWeight),
      returnSelectionOnly_(returnSelectionOnly),
      previewPath_(std::move(previewPath)),
      previewFileSize_(previewFileSize) {}

int ReaderFontSizeActivity::clampedValue(const int value) const {
  const int stepped = ((value + SMALL_STEP / 2) / SMALL_STEP) * SMALL_STEP;
  return std::clamp(stepped, MIN_SIZE, MAX_SIZE);
}

int ReaderFontSizeActivity::clampedWeight(const int value) const {
  const int stepped = ((value + WEIGHT_STEP / 2) / WEIGHT_STEP) * WEIGHT_STEP;
  return std::clamp(stepped, MIN_WEIGHT, MAX_WEIGHT);
}

void ReaderFontSizeActivity::onEnter() {
  Activity::onEnter();
  if (!returnSelectionOnly_) {
    originalSize_ = SETTINGS.readerTtfSizePx;
    originalWeight_ = SETTINGS.readerTtfWeight * 10;
  }
  value_ = clampedValue(originalSize_);
  weight_ = clampedWeight(originalWeight_);
  accepted_ = false;
  requestUpdate();
}

void ReaderFontSizeActivity::onExit() {
  if (!accepted_) {
    if (!returnSelectionOnly_) {
      SETTINGS.readerTtfSizePx = static_cast<uint8_t>(originalSize_);
      SETTINGS.readerTtfWeight = static_cast<uint8_t>(std::clamp(originalWeight_ / 10, 10, 90));
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

void ReaderFontSizeActivity::adjustWeight(const int delta) {
  const int next = clampedWeight(weight_ + delta);
  if (next == weight_) return;
  weight_ = next;
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
    SETTINGS.readerTtfWeight = static_cast<uint8_t>(std::clamp(weight_ / 10, 10, 90));
  }
  setResult(ReaderFontSettingsResult{static_cast<uint8_t>(value_), static_cast<uint16_t>(weight_)});
  finish();
}

Rect ReaderFontSizeActivity::sizeMinusButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int buttonHeight = 48;
  const int gap = metrics.contentSidePadding;
  const int buttonWidth = (renderer.getScreenWidth() - metrics.contentSidePadding * 2 - gap) / 2;
  const int y = renderer.getScreenHeight() - buttonHeight * 2 - 94;
  return Rect{metrics.contentSidePadding, y, buttonWidth, buttonHeight};
}

Rect ReaderFontSizeActivity::sizePlusButtonRect() const {
  const Rect minus = sizeMinusButtonRect();
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{minus.x + minus.width + metrics.contentSidePadding, minus.y, minus.width, minus.height};
}

Rect ReaderFontSizeActivity::weightMinusButtonRect() const {
  const Rect sizeMinus = sizeMinusButtonRect();
  return Rect{sizeMinus.x, sizeMinus.y + sizeMinus.height + 8, sizeMinus.width, sizeMinus.height};
}

Rect ReaderFontSizeActivity::weightPlusButtonRect() const {
  const Rect sizePlus = sizePlusButtonRect();
  return Rect{sizePlus.x, sizePlus.y + sizePlus.height + 8, sizePlus.width, sizePlus.height};
}

void ReaderFontSizeActivity::loop() {
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
    cancel();
    return;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, sizeMinusButtonRect())) {
    adjustValue(-SMALL_STEP);
    return;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, sizePlusButtonRect())) {
    adjustValue(SMALL_STEP);
    return;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, weightMinusButtonRect())) {
    adjustWeight(-WEIGHT_STEP);
    return;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, weightPlusButtonRect())) {
    adjustWeight(WEIGHT_STEP);
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
  buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { adjustWeight(WEIGHT_STEP); });
  buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { adjustWeight(-WEIGHT_STEP); });
}

void ReaderFontSizeActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  const auto& metrics = UITheme::getInstance().getMetrics();
  TouchUi::drawHeaderWithBack(renderer, screen, tr(STR_FONT_SIZE));

  char valueText[48];
  snprintf(valueText, sizeof(valueText), "%d px / %d", value_, weight_);
  renderer.drawCenteredText(UI_12_FONT_ID, 92, valueText, true, EpdFontFamily::BOLD);

  const int barWidth = std::min(320, std::max(0, renderer.getScreenWidth() - 72));
  constexpr int barHeight = 16;
  const int barX = std::max(0, (renderer.getScreenWidth() - barWidth) / 2);

  auto drawSlider = [&](const int y, const char* label, const int value, const int minValue, const int maxValue) {
    renderer.drawText(UI_10_FONT_ID, barX, y - 18, label, true);
    renderer.drawRect(barX, y, barWidth, barHeight);
    const int fillWidth = (barWidth - 4) * (value - minValue) / std::max(1, maxValue - minValue);
    if (fillWidth > 0) {
      renderer.fillRect(barX + 2, y + 2, fillWidth, barHeight - 4);
    }
    renderer.fillRect(std::max(barX + 2, barX + 2 + fillWidth - 2), y - 4, 4, barHeight + 8, true);
  };

  drawSlider(138, "Size", value_, MIN_SIZE, MAX_SIZE);
  drawSlider(184, "Weight", weight_, MIN_WEIGHT, MAX_WEIGHT);

  const int previewY = 222;
  const int previewX = metrics.contentSidePadding;
  const int previewWidth = renderer.getScreenWidth() - metrics.contentSidePadding * 2;
  const int previewHeight = std::min(320, std::max(120, sizeMinusButtonRect().y - previewY - 24));
  renderer.drawRect(previewX, previewY, previewWidth, previewHeight);

  bool hasTtf = false;
  bool loaded = false;
  if (returnSelectionOnly_) {
    hasTtf = !previewPath_.empty();
    loaded = hasTtf && TTF_READER_METRICS.ensureLoaded(previewPath_.c_str(), static_cast<uint8_t>(value_),
                                                       previewFileSize_, static_cast<uint16_t>(weight_));
  } else {
    SETTINGS.readerTtfSizePx = static_cast<uint8_t>(value_);
    SETTINGS.readerTtfWeight = static_cast<uint8_t>(std::clamp(weight_ / 10, 10, 90));
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

  TouchUi::drawTouchButton(renderer, sizeMinusButtonRect(), "Size -");
  TouchUi::drawTouchButton(renderer, sizePlusButtonRect(), "Size +");
  TouchUi::drawTouchButton(renderer, weightMinusButtonRect(), "Weight -");
  TouchUi::drawTouchButton(renderer, weightPlusButtonRect(), "Weight +");
  TouchUi::drawTouchButton(renderer, TouchUi::bottomActionRect(renderer), tr(STR_OK_BUTTON));

  renderer.displayBuffer();
}
