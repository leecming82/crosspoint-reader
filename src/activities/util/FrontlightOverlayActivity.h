#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

class FrontlightOverlayActivity final : public Activity {
 public:
  explicit FrontlightOverlayActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FrontlightOverlay", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowsGlobalTouchBack() const override { return false; }
  bool preventAutoSleep() override { return true; }

 private:
  struct Layout {
    Rect modal;
    Rect coolBar;
    Rect warmBar;
    Rect coolIcon;
    Rect warmIcon;
  };

  bool dirty = false;

  Layout layout() const;
  void drawIcon(const uint8_t* icon, const Rect& rect) const;
  void drawBar(const Rect& rect, uint8_t duty) const;
  bool updateFromTap(const Rect& bar, const MappedInputManager::TouchPoint& point, uint8_t& duty);
};
