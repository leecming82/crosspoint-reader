#pragma once

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

#include <string>

class ReaderFontSizeActivity final : public Activity {
 public:
  explicit ReaderFontSizeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReaderFontSize", renderer, mappedInput) {}
  ReaderFontSizeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int initialSize, int initialWeight,
                         std::string previewPath, uint32_t previewFileSize, bool returnSelectionOnly);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowsGlobalTouchBack() const override { return false; }

 private:
  static constexpr int MIN_SIZE = 18;
  static constexpr int MAX_SIZE = 72;
  static constexpr int SMALL_STEP = 2;
  static constexpr int MIN_WEIGHT = 100;
  static constexpr int MAX_WEIGHT = 900;
  static constexpr int WEIGHT_STEP = 50;

  int originalSize_ = 36;
  int originalWeight_ = 400;
  int value_ = 36;
  int weight_ = 400;
  bool accepted_ = false;
  bool returnSelectionOnly_ = false;
  std::string previewPath_;
  uint32_t previewFileSize_ = 0;
  ButtonNavigator buttonNavigator_;

  int clampedValue(int value) const;
  int clampedWeight(int value) const;
  void adjustValue(int delta);
  void adjustWeight(int delta);
  void cancel();
  void accept();
  Rect sizeMinusButtonRect() const;
  Rect sizePlusButtonRect() const;
  Rect weightMinusButtonRect() const;
  Rect weightPlusButtonRect() const;
};
