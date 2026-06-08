#pragma once

#include <CrossPointSettings.h>
#include <Epub/FootnoteEntry.h>

#include <cstring>
#include <functional>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

class EpubReaderFootnotesActivity final : public Activity {
 public:
  explicit EpubReaderFootnotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::vector<FootnoteEntry>& footnotes)
      : Activity("EpubReaderFootnotes", renderer, mappedInput), footnotes(footnotes) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  const std::vector<FootnoteEntry>& footnotes;
  int selectedIndex = 0;
  int scrollOffset = 0;
  GfxRenderer::Orientation previousRendererOrientation = GfxRenderer::Orientation::Portrait;
  ButtonNavigator buttonNavigator;

  Rect contentRect() const;
  void cancel();
  void selectCurrent();
  bool handleTouch();
};
