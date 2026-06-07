#pragma once

#include <HalGPIO.h>
#include <HalTouch.h>

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };
  using TouchPoint = HalTouch::Point;

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  MappedInputManager(HalGPIO& gpio, HalTouch& touch) : gpio(gpio), touch(touch) {}

  void beginTouch() const { touch.begin(); }
  void update() const;
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  bool hadTouchActivity() const;
  bool wasTapped() const;
  TouchPoint lastTap() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

 private:
  HalGPIO& gpio;
  HalTouch& touch;

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
};
