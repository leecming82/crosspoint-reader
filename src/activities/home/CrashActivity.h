#pragma once
#include "activities/Activity.h"

struct Rect;

class CrashActivity final : public Activity {
  std::string panicMessage;
  Rect backButtonRect() const;
  Rect headerBackTapRect() const;
  bool handleTouch();

 public:
  explicit CrashActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Crash", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowsGlobalTouchBack() const override { return false; }
};
