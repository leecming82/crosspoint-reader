#include "HalPowerManager.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include <algorithm>
#include <cassert>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

namespace {
uint16_t interpolateBatteryPercent(uint16_t millivolts) {
  struct Point {
    uint16_t mv;
    uint8_t percent;
  };

  static constexpr Point curve[] = {
      {3300, 0},
      {3500, 10},
      {3800, 50},
      {4100, 90},
      {4200, 100},
  };

  if (millivolts <= curve[0].mv) {
    return curve[0].percent;
  }
  constexpr size_t curveCount = sizeof(curve) / sizeof(curve[0]);
  for (size_t i = 1; i < curveCount; ++i) {
    if (millivolts <= curve[i].mv) {
      const auto& lo = curve[i - 1];
      const auto& hi = curve[i];
      const uint32_t spanMv = hi.mv - lo.mv;
      const uint32_t spanPercent = hi.percent - lo.percent;
      return lo.percent + ((millivolts - lo.mv) * spanPercent + spanMv / 2) / spanMv;
    }
  }
  return curve[curveCount - 1].percent;
}

uint16_t readMurphyBatteryMillivolts() {
  const uint32_t sensedMv = analogReadMilliVolts(MURPHY_BATTERY_ADC_PIN);
  return static_cast<uint16_t>(std::min<uint32_t>(sensedMv * 2U, 5000U));
}

// HZ5.2 senses the cell through a divider on GPIO1 whose resistors are not known. This
// reproduces the stock firmware's conversion rather than inventing one:
//
//   cell_volts = (raw / 4095) * 3100 mV * K
//
// recovered from the battery function's literal pool in the vendor images. The pool is
// identifiable: alongside K it carries ESP-IDF's attenuation table (950/1250/1750/3100) and
// the percent ladder 3.7/3.9/4.05/4.15/4.2 with slopes 75/133.33/150.
//
// Note 3100 is the *nominal* 11 dB full scale, not this chip's calibrated one, so K absorbs
// the ADC's real gain error along with the divider ratio. The two cannot be separated from
// the binary. That is why this works from raw counts: feeding analogReadMilliVolts() into a
// K derived against the nominal scale would apply the chip's calibration twice.
//
// K is 6.6 in the Jun 2026 vendor build and 6.9 in the Jul 2026 one, so it is the vendor's
// own empirical fudge and not a schematic value. 6.6 is used here because against this
// unit's measured raw (~847 at a terminated charge, charge LED green) it yields 4.23 V,
// whereas 6.9 yields 4.43 V, which no single LiPo cell reaches. This unit shipped with a
// Jul 14 build, between the two.
//
// Trustworthy near full charge, less so as the cell drains. A multimeter across the
// terminals, or a logged discharge, would settle both K and the curve; the divider ratio
// stays listed as open in docs/hz52-device-migration-comparison.md until then.
constexpr uint32_t HZ52_ADC_FULL_SCALE_MV = 3100;  // stock's nominal 11 dB full scale
constexpr uint32_t HZ52_ADC_MAX_COUNTS = 4095;     // 12-bit
constexpr uint32_t HZ52_SENSE_SCALE_TENTHS = 66;   // stock's K (6.6), scaled by 10

uint16_t readHz52BatteryMillivolts() {
  // The divider is high-impedance enough that a single conversion has little margin if the
  // pad is ever loaded differently, so average a burst. Measured spread was under 2 counts.
  constexpr int samples = 16;
  uint32_t sum = 0;
  for (int i = 0; i < samples; ++i) {
    sum += static_cast<uint32_t>(analogRead(HZ52_BATTERY_ADC_PIN));
  }
  const uint32_t raw = sum / samples;
  const uint32_t cellMv = raw * HZ52_ADC_FULL_SCALE_MV * HZ52_SENSE_SCALE_TENTHS / (HZ52_ADC_MAX_COUNTS * 10U);
  return static_cast<uint16_t>(std::min<uint32_t>(cellMv, 5000U));
}

#ifdef CROSSPOINT_BOARD_MURPHY_M4
void startMurphyDeepSleep(HalGPIO& gpio) {
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }

  pinMode(GPIO_NUM_0, INPUT_PULLUP);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_0, ESP_EXT1_WAKEUP_ANY_LOW);

  LOG_INF("PWR", "Entering Murphy deep sleep; wake=GPIO0 active-low");
  esp_deep_sleep_start();
}
#endif
}  // namespace

void HalPowerManager::begin() {
  if (gpio.deviceIsX3()) {
    // X3 uses an I2C fuel gauge for battery monitoring.
    // I2C init must come AFTER gpio.begin() so early hardware detection/probes are finished.
    Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
    Wire.setTimeOut(4);
    _batteryUseI2C = true;
  } else if (gpio.deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
  } else if (gpio.deviceIsMurphyM4()) {
    pinMode(MURPHY_BATTERY_ADC_PIN, INPUT);
  } else if (gpio.deviceIsHz52()) {
    pinMode(HZ52_BATTERY_ADC_PIN, INPUT);
    // readHz52BatteryMillivolts() converts raw counts against a 3100 mV full scale, which is
    // only the 11 dB figure. Set it explicitly rather than inheriting the core default.
    analogSetPinAttenuation(HZ52_BATTERY_ADC_PIN, ADC_11db);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

int HalPowerManager::idleCpuFrequencyMhz() const { return LOW_POWER_FREQ; }

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  if (gpio.deviceIsMurphyM4() || gpio.deviceIsHz52()) {
    // Live CPU downclocking is trouble on both ESP32-S3 boards; keep the loop idle delay but skip freq scaling.
    // M4 intermittently trips INT_WDT on the switch itself. HZ5.2 downclocks cleanly, but LOW_POWER_FREQ is 10 MHz
    // -- a figure validated on C3 hardware -- and that is too slow to feed this board's peripherals: an epdiy paint
    // issued from any path that does not hold a power lock starves into a task watchdog reset. Treated as the same
    // underlying hazard as M4's rather than chasing each starved peripheral separately.
    enabled = false;
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Note: We don't use mutex here to avoid too much overhead,
  // it's not very important if we read a slightly stale value for currentLockMode
  const LockMode mode = currentLockMode;

  if (mode == None && enabled && !isLowPower) {
    const int lowPowerFreq = idleCpuFrequencyMhz();
    LOG_DBG("PWR", "Going to low-power mode freq=%d MHz", lowPowerFreq);
    if (!setCpuFrequencyMhz(lowPowerFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", lowPowerFreq);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  if (gpio.deviceIsMurphyM4()) {
    startMurphyDeepSleep(gpio);
    return;
  }
#endif

#ifdef CROSSPOINT_BOARD_HZ52
  if (gpio.deviceIsHz52()) {
    // Must not fall through to the X4 path below. That path drives GPIO13 low and latches
    // it (X4's battery-latch MOSFET, but the TPS65185 INT line here) and arms ext1 wake on
    // InputManager::POWER_BUTTON_PIN = GPIO3, which on this board is the SD SPI clock. No
    // button can assert GPIO3, so the device slept and never woke.
    //
    // Wake needs an RTC-capable pad (GPIO0-21 on the S3). Of the three buttons only GPIO0
    // and GPIO21 qualify, and GPIO0 is the boot strap -- held low across reset the chip
    // enters download mode instead of booting. GPIO21 is therefore the only usable wake
    // source: sleep on Power, wake on the lower button. Revisit with the milestone 8
    // mapping, ideally by moving Power onto GPIO21.
    while (gpio.isPressed(HalGPIO::BTN_POWER)) {
      delay(50);
      gpio.update();
    }
#ifdef ENABLE_SERIAL_LOG
    logSerial.end();
#endif
    pinMode(HZ52_BTN_PAIR_LOWER, INPUT_PULLUP);
    esp_sleep_enable_ext1_wakeup(1ULL << HZ52_BTN_PAIR_LOWER, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
    return;
  }
#endif

  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }

#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

  // Pre-sleep routines from the original firmware
  // GPIO13 is connected to battery latch MOSFET, we need to make sure it's low during sleep
  // Note that this means the MCU will be completely powered off during sleep, including RTC
  constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;
  gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_SPIWP, 0);
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  gpio_hold_en(GPIO_SPIWP);
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
  // Arm the wakeup trigger *after* the button is released
  // Note: this is only useful for waking up on USB power. On battery, the MCU will be completely powered off, so the
  // power button is hard-wired to briefly provide power to the MCU, waking it up regardless of the wakeup source
  // configuration
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
#else
  esp_sleep_enable_ext1_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
#endif
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  if (!gpio.deviceIsX3() && !gpio.deviceIsX4() && !gpio.deviceIsMurphyM4() && !gpio.deviceIsHz52()) {
    return 0;
  }

  // Both M4 and HZ5.2 read a plain divider through the ADC; only the pin and ratio differ.
  if (gpio.deviceIsMurphyM4() || gpio.deviceIsHz52()) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    const uint16_t millivolts = gpio.deviceIsHz52() ? readHz52BatteryMillivolts() : readMurphyBatteryMillivolts();
    const uint16_t percent = interpolateBatteryPercent(millivolts);
    if (_batteryCachedPercent == 0) {
      _batteryCachedPercent = percent;
    } else {
      _batteryCachedPercent = (_batteryCachedPercent * 3 + percent + 2) / 4;
    }
    _batteryLastPollMs = now;
    return _batteryCachedPercent;
  }

  if (_batteryUseI2C) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    // Read SOC directly from I2C fuel gauge (16-bit LE register).
    // On I2C error, keep last known value to avoid UI jitter/slowdowns.
    Wire.beginTransmission(I2C_ADDR_BQ27220);
    Wire.write(BQ27220_SOC_REG);
    if (Wire.endTransmission(false) != 0) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    Wire.requestFrom(I2C_ADDR_BQ27220, (uint8_t)2);
    if (Wire.available() < 2) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    const uint16_t soc = (hi << 8) | lo;
    _batteryCachedPercent = soc > 100 ? 100 : soc;
    _batteryLastPollMs = now;
    return _batteryCachedPercent;
  }
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
