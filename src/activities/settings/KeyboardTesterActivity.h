#pragma once

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

class KeyboardTesterActivity final : public Activity {
 public:
  explicit KeyboardTesterActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("KeyboardTester", renderer, mappedInput) {}

  static constexpr int COLS = 8;
  static constexpr int ROWS = 5;

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowsGlobalTouchBack() const override { return false; }

 private:
  enum class Mode : uint8_t { English, Romaji };

  Mode mode = Mode::English;
  std::string englishText;
  std::string committedKana;
  std::string pendingRomaji;

  void insertChar(char ch);
  void insertSpace();
  void backspace();
  void clearText();
  void finalizePendingRomaji();
  std::string displayText() const;
  Rect modeEnglishRect() const;
  Rect modeRomajiRect() const;
  bool handleTouch();
};
