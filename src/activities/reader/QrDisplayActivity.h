#pragma once
#include <CrossPointSettings.h>
#include <I18n.h>

#include <string>

#include "activities/Activity.h"

class QrDisplayActivity final : public Activity {
 public:
  explicit QrDisplayActivity(
      GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& textPayload,
      const uint8_t effectiveReadingLayout = CrossPointSettings::READING_LAYOUT_HORIZONTAL_PORTRAIT)
      : Activity("QrDisplay", renderer, mappedInput),
        textPayload(textPayload),
        effectiveReadingLayout(effectiveReadingLayout) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string textPayload;
  uint8_t effectiveReadingLayout = CrossPointSettings::READING_LAYOUT_HORIZONTAL_PORTRAIT;
  GfxRenderer::Orientation previousRendererOrientation = GfxRenderer::Orientation::Portrait;
};
