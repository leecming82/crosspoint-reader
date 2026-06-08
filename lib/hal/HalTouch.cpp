#include "HalTouch.h"

#include <HalGPIO.h>
#include <Logging.h>
#include <Wire.h>

namespace {
constexpr uint8_t TOUCH_ADDR = 0x2E;
constexpr uint32_t TOUCH_I2C_FREQ = 400000;
constexpr int TOUCH_SDA = 13;
constexpr int TOUCH_SCL = 12;
constexpr int TOUCH_INT = 44;
constexpr int TOUCH_RST = 7;
constexpr int TOUCH_PWR = 45;
constexpr uint16_t RAW_WIDTH = 480;
constexpr uint16_t RAW_HEIGHT = 800;
constexpr unsigned long MAX_TAP_MS = 900;
constexpr unsigned long LONG_PRESS_MS = 800;
}  // namespace

HalTouch halTouch;

void HalTouch::begin() {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  enabled = true;
#else
  enabled = false;
#endif
  initialized = false;
  wasDown = false;
  tapAvailable = false;
  longPressAvailable = false;
  longPressEmitted = false;
  activity = false;
  if (!enabled) {
    return;
  }

  pinMode(TOUCH_PWR, OUTPUT);
  digitalWrite(TOUCH_PWR, LOW);
  pinMode(TOUCH_INT, INPUT_PULLUP);
  delay(500);

  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(50);
  digitalWrite(TOUCH_RST, HIGH);
  delay(100);

  Wire.begin(TOUCH_SDA, TOUCH_SCL, TOUCH_I2C_FREQ);
  Wire.setTimeOut(8);
  Wire.beginTransmission(TOUCH_ADDR);
  initialized = Wire.endTransmission(true) == 0;
  LOG_DBG("TOUCH", "Murphy touch begin: ok=%d sda=GPIO%d scl=GPIO%d addr=0x%02x", initialized ? 1 : 0,
          TOUCH_SDA, TOUCH_SCL, TOUCH_ADDR);
}

void HalTouch::setLogicalSize(const uint16_t width, const uint16_t height) {
  if (width == 0 || height == 0) {
    return;
  }
  logicalWidth = width;
  logicalHeight = height;
}

bool HalTouch::readRaw(Point& raw, bool& down) {
  uint8_t bytes[7] = {};
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  const uint8_t got = Wire.requestFrom(TOUCH_ADDR, static_cast<uint8_t>(sizeof(bytes)), static_cast<uint8_t>(true));
  if (got != sizeof(bytes)) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  for (uint8_t i = 0; i < sizeof(bytes); ++i) {
    bytes[i] = Wire.read();
  }

  const uint8_t count = bytes[2] & 0x0F;
  if (count == 0) {
    down = false;
    raw = {};
    return true;
  }
  if (count > 2) {
    return false;
  }

  const uint8_t event = bytes[3] >> 6;
  raw.x = (static_cast<uint16_t>(bytes[3] & 0x0F) << 8) | bytes[4];
  raw.y = (static_cast<uint16_t>(bytes[5] & 0x0F) << 8) | bytes[6];
  down = (event == 0 || event == 2) && raw.x <= RAW_WIDTH && raw.y <= RAW_HEIGHT;
  return true;
}

HalTouch::Point HalTouch::transform(Point raw) const {
  Point out;
  uint32_t x = (static_cast<uint32_t>(raw.x) * logicalWidth) / RAW_WIDTH;
  uint32_t y = (static_cast<uint32_t>(raw.y) * logicalHeight) / RAW_HEIGHT;
  if (x >= logicalWidth) {
    x = logicalWidth - 1;
  }
  if (y >= logicalHeight) {
    y = logicalHeight - 1;
  }
  out.x = static_cast<uint16_t>(x);
  out.y = static_cast<uint16_t>(y);
  return out;
}

void HalTouch::update() {
  tapAvailable = false;
  longPressAvailable = false;
  activity = false;
  if (!enabled || !initialized) {
    return;
  }

  Point raw;
  bool down = false;
  if (!readRaw(raw, down)) {
    return;
  }

  if (down && !wasDown) {
    wasDown = true;
    activity = true;
    downAt = millis();
    downPoint = transform(raw);
    longPressEmitted = false;
  } else if (!down && wasDown) {
    wasDown = false;
    activity = true;
    if (!longPressEmitted && millis() - downAt <= MAX_TAP_MS) {
      tapPoint = downPoint;
      tapAvailable = true;
    }
  } else if (down) {
    activity = true;
    if (!longPressEmitted && millis() - downAt >= LONG_PRESS_MS) {
      longPressPoint = downPoint;
      longPressAvailable = true;
      longPressEmitted = true;
    }
  }
}

bool HalTouch::wasTapped() const { return tapAvailable; }

HalTouch::Point HalTouch::lastTap() const { return tapPoint; }

bool HalTouch::wasLongPressed() const { return longPressAvailable; }

HalTouch::Point HalTouch::lastLongPress() const { return longPressPoint; }

bool HalTouch::hadActivity() const { return activity || tapAvailable || longPressAvailable; }
