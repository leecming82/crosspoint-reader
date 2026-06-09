#include "HalEnvSensor.h"

#include <Logging.h>
#include <Wire.h>

#include <algorithm>

namespace {
constexpr uint32_t ENV_I2C_FREQ = 400000;
constexpr int ENV_SDA = 13;
constexpr int ENV_SCL = 12;
constexpr uint8_t AHT20_ADDR = 0x38;
}  // namespace

HalEnvSensor halEnvSensor;

void HalEnvSensor::begin() {
#ifndef CROSSPOINT_BOARD_MURPHY_M4
  available = false;
  sensorType = SensorType::None;
  return;
#else
  if (begun) {
    return;
  }
  begun = true;

  Wire.begin(ENV_SDA, ENV_SCL, ENV_I2C_FREQ);
  Wire.setTimeOut(8);

  if (beginAht20(AHT20_ADDR)) {
    address = AHT20_ADDR;
    sensorType = SensorType::Aht20;
    available = true;
  } else {
    available = false;
    sensorType = SensorType::None;
  }

  LOG_INF("ENV", "Murphy env sensor: ok=%d type=AHT20 addr=0x%02x sda=GPIO%d scl=GPIO%d", available ? 1 : 0,
          address, ENV_SDA, ENV_SCL);
#endif
}

namespace {
bool probeAddress(const uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission(true) == 0;
}
}  // namespace

bool HalEnvSensor::beginAht20(const uint8_t addr) {
  if (!probeAddress(addr)) {
    return false;
  }

  Wire.beginTransmission(addr);
  Wire.write(0xBE);
  Wire.write(0x08);
  Wire.write(0x00);
  if (Wire.endTransmission(true) != 0) {
    return false;
  }
  delay(10);
  return true;
}

bool HalEnvSensor::read(Reading& reading) {
  reading = {};
  if (!available) {
    return false;
  }

  switch (sensorType) {
    case SensorType::Aht20:
      return readAht20(reading);
    case SensorType::None:
    default:
      return false;
  }
}

bool HalEnvSensor::readAht20(Reading& reading) const {
  Wire.beginTransmission(address);
  Wire.write(0xAC);
  Wire.write(0x33);
  Wire.write(0x00);
  if (Wire.endTransmission(true) != 0) {
    return false;
  }
  delay(80);

  const uint8_t got = Wire.requestFrom(address, static_cast<uint8_t>(7), static_cast<uint8_t>(true));
  if (got != 7) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }

  uint8_t bytes[7] = {};
  for (uint8_t i = 0; i < sizeof(bytes); ++i) {
    bytes[i] = Wire.read();
  }
  if (bytes[0] & 0x80) {
    return false;
  }

  const uint32_t rawHum = (static_cast<uint32_t>(bytes[1]) << 12) | (static_cast<uint32_t>(bytes[2]) << 4) |
                          (static_cast<uint32_t>(bytes[3]) >> 4);
  const uint32_t rawTemp = ((static_cast<uint32_t>(bytes[3]) & 0x0F) << 16) |
                           (static_cast<uint32_t>(bytes[4]) << 8) | bytes[5];
  reading.humidityPercent = std::clamp(static_cast<float>(rawHum) * 100.0f / 1048576.0f, 0.0f, 100.0f);
  reading.temperatureC = static_cast<float>(rawTemp) * 200.0f / 1048576.0f - 50.0f;
  reading.valid = true;
  return true;
}
