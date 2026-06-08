#pragma once

#include <functional>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

class ClearCacheActivity final : public Activity {
 public:
  explicit ClearCacheActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClearCache", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, CLEARING, SUCCESS, FAILED };

  State state = WARNING;

  void goBack() { finish(); }

  int clearedCount = 0;
  int failedCount = 0;
  Rect cancelButtonRect() const;
  Rect confirmButtonRect() const;
  Rect backButtonRect() const;
  void confirmClear();
  void clearCache();
};
