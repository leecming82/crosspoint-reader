#pragma once

#include <Arduino.h>

class HalEnvSensor {
 public:
  enum class SensorType : uint8_t {
    None = 0,
    Sht40,
    Aht20,
  };

  struct Reading {
    bool valid = false;
    float temperatureC = 0.0f;
    float humidityPercent = 0.0f;
  };

  void begin();
  bool isAvailable() const { return available; }
  bool read(Reading& reading);

 private:
  bool begun = false;
  bool available = false;
  SensorType sensorType = SensorType::None;
  uint8_t address = 0;

  bool beginAht20(uint8_t addr);
  bool readAht20(Reading& reading) const;
};

extern HalEnvSensor halEnvSensor;
