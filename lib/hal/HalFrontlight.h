#pragma once

#include <Arduino.h>

class HalFrontlight {
 public:
  enum class Channel : uint8_t {
    Cool = 0,
    Warm = 1,
  };

  static constexpr uint8_t MURPHY_COOL_PIN = 47;
  static constexpr uint8_t MURPHY_WARM_PIN = 48;

  void begin();
  bool isAvailable() const { return available; }
  uint8_t getRaw(Channel channel) const;
  void set(uint8_t cool, uint8_t warm);
  void setRaw(Channel channel, uint8_t duty);
  void off();

 private:
  bool begun = false;
  bool available = false;
  uint8_t coolDuty = 0;
  uint8_t warmDuty = 0;

  uint8_t pinFor(Channel channel) const;
};

extern HalFrontlight halFrontlight;
