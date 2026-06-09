#pragma once

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

class HardwareDiagnosticsActivity final : public Activity {
 public:
  explicit HardwareDiagnosticsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("HardwareDiagnostics", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowsGlobalTouchBack() const override { return false; }

 private:
  int chargeRaw = -1;
  int charging = -1;
  int batteryRaw = -1;
  int batterySenseMv = -1;
  int batterySystemMv = -1;
  int batteryPercent = -1;
  bool envAvailable = false;
  bool envReadOk = false;
  float envTemperatureC = 0.0f;
  float envHumidityPercent = 0.0f;
  unsigned long sampleMs = 0;
  bool frontlightDirty = false;

  void refreshReadings();
  Rect refreshButtonRect() const;
  Rect frontlight47Rect() const;
  Rect frontlight48Rect() const;
  void cycleFrontlightCool();
  void cycleFrontlightWarm();
};
