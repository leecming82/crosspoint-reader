#include <HalGPIO.h>
#include <Logging.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <esp_sleep.h>

// Global HalGPIO instance
HalGPIO gpio;

namespace X3GPIO {

struct X3ProbeResult {
  bool bq27220 = false;
  bool ds3231 = false;
  bool qmi8658 = false;

  uint8_t score() const {
    return static_cast<uint8_t>(bq27220) + static_cast<uint8_t>(ds3231) + static_cast<uint8_t>(qmi8658);
  }
};

bool readI2CReg8(uint8_t addr, uint8_t reg, uint8_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) {
    return false;
  }
  *outValue = Wire.read();
  return true;
}

bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *outValue = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

bool readBQ27220CurrentMA(int16_t* outCurrent) {
  uint16_t raw = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_CUR_REG, &raw)) {
    return false;
  }
  *outCurrent = static_cast<int16_t>(raw);
  return true;
}

bool probeBQ27220Signature() {
  uint16_t soc = 0;
  uint16_t voltageMv = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_SOC_REG, &soc)) {
    return false;
  }
  if (soc > 100) {
    return false;
  }
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_VOLT_REG, &voltageMv)) {
    return false;
  }
  return voltageMv >= 2500 && voltageMv <= 5000;
}

bool probeDS3231Signature() {
  uint8_t sec = 0;
  if (!readI2CReg8(I2C_ADDR_DS3231, DS3231_SEC_REG, &sec)) {
    return false;
  }
  const uint8_t tensDigit = (sec >> 4) & 0x07;
  const uint8_t onesDigit = sec & 0x0F;

  return tensDigit <= 5 && onesDigit <= 9;
}

bool probeQMI8658Signature() {
  uint8_t whoami = 0;
  if (readI2CReg8(I2C_ADDR_QMI8658, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
    return true;
  }
  if (readI2CReg8(I2C_ADDR_QMI8658_ALT, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
    return true;
  }
  return false;
}

X3ProbeResult runX3ProbePass() {
  X3ProbeResult result;
  Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
  Wire.setTimeOut(6);

  result.bq27220 = probeBQ27220Signature();
  result.ds3231 = probeDS3231Signature();
  result.qmi8658 = probeQMI8658Signature();

  Wire.end();
  pinMode(20, INPUT);
  pinMode(0, INPUT);
  return result;
}

}  // namespace X3GPIO

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3
constexpr unsigned long MURPHY_DEBOUNCE_DELAY_MS = 5;
constexpr uint8_t MURPHY_BTN_TOP = 1 << 0;
constexpr uint8_t MURPHY_BTN_MIDDLE = 1 << 1;
constexpr uint8_t MURPHY_BTN_BOTTOM = 1 << 2;

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) {
    return defaultValue;
  }
  const uint8_t raw = prefs.getUChar(key, static_cast<uint8_t>(defaultValue));
  prefs.end();
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) {
    return defaultValue;
  }
  return static_cast<NvsDeviceValue>(raw);
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) {
    return;
  }
  prefs.putUChar(key, static_cast<uint8_t>(value));
  prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
#if defined(CROSSPOINT_BOARD_HZ52)
  // HZ5.2 has no shared-bus fingerprint to probe for: the X3 probe drives C3 pins
  // that are the parallel display bus here, so it must never run on this board.
  LOG_INF("HW", "Board profile forced by build: HZ5.2");
  return HalGPIO::DeviceType::HZ52;
#elif defined(CROSSPOINT_BOARD_MURPHY_M4)
  LOG_INF("HW", "Board profile forced by build: Murphy M4");
  return HalGPIO::DeviceType::MurphyM4;
#else
  // Explicit override for recovery/support:
  // 0 = auto, 1 = force X4, 2 = force X3
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }

  // No cache yet: run active X3 fingerprint probe and persist result.
  const X3GPIO::X3ProbeResult pass1 = X3GPIO::runX3ProbePass();
  delay(2);
  const X3GPIO::X3ProbeResult pass2 = X3GPIO::runX3ProbePass();

  const uint8_t score1 = pass1.score();
  const uint8_t score2 = pass2.score();
  LOG_INF("HW", "X3 probe scores: pass1=%u(bq=%d rtc=%d imu=%d) pass2=%u(bq=%d rtc=%d imu=%d)", score1, pass1.bq27220,
          pass1.ds3231, pass1.qmi8658, score2, pass2.bq27220, pass2.ds3231, pass2.qmi8658);
  const bool x3Confirmed = (score1 >= 2) && (score2 >= 2);
  const bool x4Confirmed = (score1 == 0) && (score2 == 0);

  if (x3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }

  if (x4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
    return HalGPIO::DeviceType::X4;
  }

  // Conservative fallback for first boot with inconclusive probes.
  return HalGPIO::DeviceType::X4;
#endif
}

uint8_t murphyReadPhysicalButtons() {
  uint8_t state = 0;
  if (digitalRead(1) == LOW) {
    state |= MURPHY_BTN_TOP;
  }
  if (digitalRead(2) == LOW) {
    state |= MURPHY_BTN_MIDDLE;
  }
  if (digitalRead(0) == LOW) {
    state |= MURPHY_BTN_BOTTOM;
  }
  return state;
}

uint8_t murphyShortPressButton(uint8_t physicalButton) {
  if (physicalButton == MURPHY_BTN_TOP) {
    return HalGPIO::BTN_UP;
  }
  if (physicalButton == MURPHY_BTN_MIDDLE) {
    return HalGPIO::BTN_DOWN;
  }
  if (physicalButton == MURPHY_BTN_BOTTOM) {
    return HalGPIO::BTN_POWER;
  }
  return 0xFF;
}

uint8_t murphyLongPressButton(uint8_t physicalButton) {
  return physicalButton == MURPHY_BTN_BOTTOM ? HalGPIO::BTN_POWER : 0xFF;
}

}  // namespace

void HalGPIO::begin() {
  // SPI_BUS_* aliases the display pins on the shared-bus boards (X3/X4/Murphy) and
  // resolves to the SD-only FSPI pins on HZ5.2, whose panel is parallel, not SPI.
  SPI.begin(SPI_BUS_SCLK, SPI_BUS_MISO, SPI_BUS_MOSI, EPD_CS);

  _deviceType = detectDeviceTypeWithFingerprint();

  if (deviceIsMurphyM4()) {
    pinMode(1, INPUT_PULLUP);
    pinMode(2, INPUT_PULLUP);
    pinMode(0, INPUT_PULLUP);
    pinMode(MURPHY_CHARGE_STATUS_PIN, INPUT_PULLUP);
    murphyRawState = murphyReadPhysicalButtons();
    murphyLastRawState = murphyRawState;
    murphyPhysicalState = murphyRawState;
    lastUsbConnected = isUsbConnected();
    LOG_INF("GPIO", "Murphy M4 buttons: top=GPIO1, middle=GPIO2, bottom=GPIO0");
    return;
  }

  if (deviceIsHz52()) {
    // Bring up the peripheral power rail before anything tries to talk to a peripheral.
    // Stock firmware holds GPIO46 HIGH persistently -- in both idle and active JTAG
    // snapshots -- and with it low the SD card does not respond at all. This is a board
    // rail rather than a storage detail, so it belongs here in board init.
    const auto& hz52 = boardProfileFor(DeviceType::HZ52);
    if (hz52.sdEnablePin >= 0) {
      pinMode(hz52.sdEnablePin, OUTPUT);
      digitalWrite(hz52.sdEnablePin, hz52.sdEnableActiveLow ? LOW : HIGH);
    }
    LOG_INF("GPIO", "HZ5.2 SD power gate asserted (GPIO%d)", hz52.sdEnablePin);

    // Bypass InputManager for the same reason the Murphy path does: it assumes the
    // C3/X4 map, where POWER_BUTTON_PIN=3 and the button ladder is an ADC on GPIO1/2.
    // On HZ5.2 those pads are the SD SPI clock, the battery divider and SD MISO.
    // Claiming GPIO3 as a button input re-routes it away from FSPICLK, which leaves
    // the SD clock dead and hangs the first transfer forever in spiTransferByteNL().
    pinMode(HZ52_BTN_ISOLATED, INPUT_PULLUP);
    pinMode(HZ52_BTN_PAIR_UPPER, INPUT_PULLUP);
    pinMode(HZ52_BTN_PAIR_LOWER, INPUT_PULLUP);
    LOG_INF("GPIO", "HZ5.2 buttons: isolated=GPIO%d upper=GPIO%d lower=GPIO%d (InputManager bypassed)",
            HZ52_BTN_ISOLATED, HZ52_BTN_PAIR_UPPER, HZ52_BTN_PAIR_LOWER);
    return;
  }

  inputMgr.begin();

  if (deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
    pinMode(UART0_RXD, INPUT);
  }
}

void HalGPIO::update() {
  if (deviceIsMurphyM4()) {
    const unsigned long now = millis();
    const uint8_t rawState = murphyReadPhysicalButtons();
    murphyPressedEvents = 0;
    murphyReleasedEvents = 0;
    murphyFrontlightEvent = false;
    murphyScreenshotEvent = false;
    murphySleepEvent = false;

    if (rawState != murphyLastRawState) {
      murphyLastDebounceTime = now;
      murphyLastRawState = rawState;
    }

    if ((now - murphyLastDebounceTime) > MURPHY_DEBOUNCE_DELAY_MS && rawState != murphyRawState) {
      const uint8_t previousPhysicalState = murphyRawState;
      murphyRawState = rawState;

      if (previousPhysicalState == 0 && rawState != 0) {
        murphyPhysicalState = rawState;
        murphyPressStart = now;
        murphyPowerLongPressActive = false;
        murphyLongPressHandled = false;
        murphyCurrentState = 0;
      } else if (previousPhysicalState != 0 && rawState == 0) {
        const unsigned long heldTime = now - murphyPressStart;
        if (heldTime >= MURPHY_LONG_PRESS_MS && !murphyLongPressHandled) {
          if (murphyPhysicalState == MURPHY_BTN_TOP) {
            murphyFrontlightEvent = true;
            murphyLongPressHandled = true;
          } else if (murphyPhysicalState == MURPHY_BTN_MIDDLE) {
            murphyScreenshotEvent = true;
            murphyLongPressHandled = true;
          }
        }
        const uint8_t logicalButton = heldTime >= MURPHY_LONG_PRESS_MS ? murphyLongPressButton(murphyPhysicalState)
                                                                       : murphyShortPressButton(murphyPhysicalState);
        if (logicalButton <= BTN_POWER) {
          const uint8_t logicalMask = 1 << logicalButton;
          if ((murphyCurrentState & logicalMask) == 0) {
            murphyPressedEvents |= logicalMask;
          }
          murphyReleasedEvents |= logicalMask;
          murphyCurrentState &= ~logicalMask;
          if (logicalButton == BTN_POWER) {
            murphyPowerPressFinish = now;
          }
        }
        murphyPressFinish = now;
        murphyPhysicalState = 0;
        murphyPowerLongPressActive = false;
        murphyLongPressHandled = false;
      }
    }

    if (murphyRawState != 0 && !murphyLongPressHandled && (now - murphyPressStart) >= MURPHY_LONG_PRESS_MS) {
      if (murphyRawState == MURPHY_BTN_TOP) {
        murphyFrontlightEvent = true;
        murphyLongPressHandled = true;
      } else if (murphyRawState == MURPHY_BTN_MIDDLE) {
        murphyScreenshotEvent = true;
        murphyLongPressHandled = true;
      } else if (murphyRawState == MURPHY_BTN_BOTTOM && !murphyPowerLongPressActive) {
        const uint8_t powerMask = 1 << BTN_POWER;
        murphyPowerLongPressActive = true;
        murphyPowerPressStart = murphyPressStart;
        murphyCurrentState |= powerMask;
        murphyPressedEvents |= powerMask;
      }
    }

    const bool connected = isUsbConnected();
    usbStateChanged = (connected != lastUsbConnected);
    lastUsbConnected = connected;
    return;
  }

  inputMgr.update();
  const bool connected = isUsbConnected();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const {
  if (deviceIsMurphyM4()) {
    return buttonIndex <= BTN_POWER && (murphyCurrentState & (1 << buttonIndex));
  }
  return inputMgr.isPressed(buttonIndex);
}

bool HalGPIO::wasPressed(uint8_t buttonIndex) const {
  if (deviceIsMurphyM4()) {
    return buttonIndex <= BTN_POWER && (murphyPressedEvents & (1 << buttonIndex));
  }
  return inputMgr.wasPressed(buttonIndex);
}

bool HalGPIO::wasAnyPressed() const {
  if (deviceIsMurphyM4()) {
    return murphyPressedEvents > 0;
  }
  return inputMgr.wasAnyPressed();
}

bool HalGPIO::wasReleased(uint8_t buttonIndex) const {
  if (deviceIsMurphyM4()) {
    return buttonIndex <= BTN_POWER && (murphyReleasedEvents & (1 << buttonIndex));
  }
  return inputMgr.wasReleased(buttonIndex);
}

bool HalGPIO::wasAnyReleased() const {
  if (deviceIsMurphyM4()) {
    return murphyReleasedEvents > 0 || murphyFrontlightEvent || murphyScreenshotEvent || murphySleepEvent;
  }
  return inputMgr.wasAnyReleased();
}

bool HalGPIO::wasFrontlightButtonReleased() const {
  return deviceIsMurphyM4() && murphyFrontlightEvent;
}

bool HalGPIO::wasScreenshotButtonReleased() const {
  return deviceIsMurphyM4() && murphyScreenshotEvent;
}

bool HalGPIO::wasSleepButtonReleased() const {
  return deviceIsMurphyM4() && murphySleepEvent;
}

unsigned long HalGPIO::getHeldTime() const {
  if (deviceIsMurphyM4()) {
    if (murphyRawState != 0) {
      return millis() - murphyPressStart;
    }
    return murphyPressFinish - murphyPressStart;
  }
  return inputMgr.getHeldTime();
}

unsigned long HalGPIO::getPowerButtonHeldTime() const {
  if (deviceIsMurphyM4()) {
    if (isPressed(BTN_POWER)) {
      return millis() - murphyPowerPressStart;
    }
    return murphyPowerPressFinish - murphyPowerPressStart;
  }
  return inputMgr.getPowerButtonHeldTime();
}

void HalGPIO::startDeepSleep() {
  if (deviceIsMurphyM4()) {
    LOG_INF("GPIO", "Deep sleep skipped on Murphy M4: power wake GPIO is not identified yet");
    return;
  }

  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (inputMgr.isPressed(BTN_POWER)) {
    delay(50);
    inputMgr.update();
  }
  // Arm the wakeup trigger *after* the button is released
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
#else
  esp_sleep_enable_ext1_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
#endif
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

void HalGPIO::verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) {
  if (shortPressAllowed) {
    // Fast path - no duration check needed
    return;
  }
  // TODO: Intermittent edge case remains: a single tap followed by another single tap
  // can still power on the device. Tighten wake debounce/state handling here.

  // Calibrate: subtract boot time already elapsed, assuming button held since boot
  const uint16_t calibration = millis();
  const uint16_t calibratedDuration = (calibration < requiredDurationMs) ? (requiredDurationMs - calibration) : 1;

  const auto start = millis();
  inputMgr.update();
  // inputMgr.isPressed() may take up to ~500ms to return correct state
  while (!inputMgr.isPressed(BTN_POWER) && millis() - start < 1000) {
    delay(10);
    inputMgr.update();
  }
  if (inputMgr.isPressed(BTN_POWER)) {
    do {
      delay(10);
      inputMgr.update();
    } while (inputMgr.isPressed(BTN_POWER) && inputMgr.getPowerButtonHeldTime() < calibratedDuration);
    if (inputMgr.getPowerButtonHeldTime() < calibratedDuration) {
      startDeepSleep();
    }
  } else {
    startDeepSleep();
  }
}

bool HalGPIO::isUsbConnected() const {
  if (deviceIsMurphyM4()) {
    // Murphy exposes an active-low charger-status signal rather than a raw USB-present signal.
    return digitalRead(MURPHY_CHARGE_STATUS_PIN) == LOW;
  }
  if (deviceIsX3()) {
    // X3: infer USB/charging via BQ27220 Current() register (0x0C, signed mA).
    // Positive current means charging.
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
      int16_t currentMa = 0;
      if (X3GPIO::readBQ27220CurrentMA(&currentMa)) {
        return currentMa > 0;
      }
      delay(2);
    }
    return false;
  }
  // U0RXD/GPIO20 reads HIGH when USB is connected
  return digitalRead(UART0_RXD) == HIGH;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  if (deviceIsMurphyM4()) {
    return WakeupReason::Other;
  }

  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  const bool usbConnected = isUsbConnected();

  if ((wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) ||
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP && usbConnected)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}
