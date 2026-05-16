#pragma once

#include <CrossPointSettings.h>
#include <Epub/FootnoteEntry.h>

#include <cstring>
#include <functional>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderFootnotesActivity final : public Activity {
 public:
  explicit EpubReaderFootnotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::vector<FootnoteEntry>& footnotes,
                                       const uint8_t effectiveReadingLayout)
      : Activity("EpubReaderFootnotes", renderer, mappedInput),
        footnotes(footnotes),
        effectiveReadingLayout(effectiveReadingLayout) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  const std::vector<FootnoteEntry>& footnotes;
  int selectedIndex = 0;
  int scrollOffset = 0;
  uint8_t effectiveReadingLayout = CrossPointSettings::READING_LAYOUT_HORIZONTAL_PORTRAIT;
  GfxRenderer::Orientation previousRendererOrientation = GfxRenderer::Orientation::Portrait;
  ButtonNavigator buttonNavigator;
};
