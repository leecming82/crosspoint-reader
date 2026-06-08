#pragma once

#include <Arduino.h>

class HalTouch {
 public:
  static constexpr uint16_t ScreenWidth = 480;
  static constexpr uint16_t ScreenHeight = 800;

  struct Point {
    uint16_t x = 0;
    uint16_t y = 0;
  };

  void begin();
  void setLogicalSize(uint16_t width, uint16_t height);
  void setLogicalOrientation(uint8_t orientation);
  void update();
  bool wasTapped() const;
  Point lastTap() const;
  bool wasLongPressed() const;
  Point lastLongPress() const;
  bool hadActivity() const;

 private:
  bool enabled = false;
  bool initialized = false;
  bool wasDown = false;
  bool tapAvailable = false;
  bool longPressAvailable = false;
  bool longPressEmitted = false;
  bool activity = false;
  unsigned long downAt = 0;
  Point downPoint;
  Point tapPoint;
  Point longPressPoint;
  uint16_t logicalWidth = ScreenWidth;
  uint16_t logicalHeight = ScreenHeight;
  uint8_t logicalOrientation = 0;

  bool readRaw(Point& raw, bool& down);
  Point transform(Point raw) const;
};

extern HalTouch halTouch;
