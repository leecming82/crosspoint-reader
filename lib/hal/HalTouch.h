#pragma once

#include <Arduino.h>

class HalTouch {
 public:
  static constexpr uint16_t ScreenWidth = 800;
  static constexpr uint16_t ScreenHeight = 480;

  struct Point {
    uint16_t x = 0;
    uint16_t y = 0;
  };

  void begin();
  void update();
  bool wasTapped() const;
  Point lastTap() const;
  bool hadActivity() const;

 private:
  bool enabled = false;
  bool initialized = false;
  bool wasDown = false;
  bool tapAvailable = false;
  bool activity = false;
  unsigned long downAt = 0;
  Point downPoint;
  Point tapPoint;

  bool readRaw(Point& raw, bool& down);
  static Point transform(Point raw);
};

extern HalTouch halTouch;
