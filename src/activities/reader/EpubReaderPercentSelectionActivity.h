#pragma once

#include <CrossPointSettings.h>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderPercentSelectionActivity final : public Activity {
 public:
  // Slider-style percent selector for jumping within a book.
  explicit EpubReaderPercentSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const int initialPercent, const uint8_t effectiveReadingLayout)
      : Activity("EpubReaderPercentSelection", renderer, mappedInput),
        percent(initialPercent),
        effectiveReadingLayout(effectiveReadingLayout) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Current percent value (0-100) shown on the slider.
  int percent = 0;
  uint8_t effectiveReadingLayout = CrossPointSettings::READING_LAYOUT_HORIZONTAL_PORTRAIT;
  GfxRenderer::Orientation previousRendererOrientation = GfxRenderer::Orientation::Portrait;

  ButtonNavigator buttonNavigator;

  // Change the current percent by a delta and clamp within bounds.
  void adjustPercent(int delta);
};
