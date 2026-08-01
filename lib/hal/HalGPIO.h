#pragma once

#include <Arduino.h>
#include <InputManager.h>
#include <BoardProfile.h>

// Display SPI pins (custom pins for XteinkX4/Murphy, not hardware SPI defaults)
#if defined(CROSSPOINT_BOARD_HZ52)
// HZ5.2 has no display SPI at all: the panel is an 8-bit parallel bus driven by
// epdiy. Every EPD_* pin is therefore unmapped, and driving the inherited X4
// values here would land on the parallel data bus, a TPS65185 control line and a
// button. See docs/hz52-device-migration-comparison.md.
#define EPD_SCLK -1
#define EPD_MOSI -1
#define EPD_CS -1
#define EPD_DC -1
#define EPD_RST -1
#define EPD_BUSY -1
#define SPI_MISO -1
#elif defined(CROSSPOINT_BOARD_MURPHY_M4)
#define EPD_SCLK 4   // SPI Clock
#define EPD_MOSI 3   // SPI MOSI / panel DIN
#define EPD_CS 5     // Chip Select
#define EPD_DC 6     // Data/Command
#define EPD_RST 7    // Reset
#define EPD_BUSY 8   // Busy
#define SPI_MISO -1  // Murphy display SPI is write-only; SD wiring is separate.
#else
#define EPD_SCLK 8   // SPI Clock
#define EPD_MOSI 10  // SPI MOSI (Master Out Slave In)
#define EPD_CS 21    // Chip Select
#define EPD_DC 4     // Data/Command
#define EPD_RST 5    // Reset
#define EPD_BUSY 6   // Busy
#define SPI_MISO 7   // SPI MISO, shared between SD card and display (Master In Slave Out)
#endif

// Shared SPI bus pins. On X3/X4/Murphy the e-paper panel and the SD card sit on one
// bus, so these alias the display pins. HZ5.2 has no display SPI; its only SPI
// peripheral is the SD card, on FSPI.
#if defined(CROSSPOINT_BOARD_HZ52)
#define SPI_BUS_SCLK 3
#define SPI_BUS_MISO 2
#define SPI_BUS_MOSI 43
#else
#define SPI_BUS_SCLK EPD_SCLK
#define SPI_BUS_MISO SPI_MISO
#define SPI_BUS_MOSI EPD_MOSI
#endif

// HZ5.2 buttons, confirmed by press-delta test. Right edge, arranged one/gap/two.
#define HZ52_BTN_ISOLATED 38    // single button above the gap
#define HZ52_BTN_PAIR_UPPER 0   // upper of the pair; also the ESP32-S3 boot strap
#define HZ52_BTN_PAIR_LOWER 21  // lower of the pair
#define HZ52_BATTERY_ADC_PIN 1  // analog signature; ADC1, consistent with stock firmware
#define HZ52_SD_POWER_EN 46     // SD power gate, active HIGH (confirmed: card loses state when toggled)
#define HZ52_PMIC_WAKEUP 14     // TPS65185 WAKEUP; the PMIC will not ACK on I2C until asserted
#define HZ52_SD_CS_PIN 44       // SD chip-select, confirmed by successful mount
#define HZ52_I2C_SDA 39         // I2CEXT0_SDA, confirmed by JTAG register read
#define HZ52_I2C_SCL 40         // I2CEXT0_SCL, confirmed by JTAG register read

#define BAT_GPIO0 0  // Battery voltage

#define MURPHY_BATTERY_ADC_PIN 9       // Battery divider ADC input
#define MURPHY_CHARGE_STATUS_PIN 43    // Active-low charger status

#define UART0_RXD 20  // Used for USB connection detection

// Xteink X3 Hardware
#define X3_I2C_SDA 20
#define X3_I2C_SCL 0
#define X3_I2C_FREQ 400000

// TI BQ27220 Fuel gauge I2C
#define I2C_ADDR_BQ27220 0x55  // Fuel gauge I2C address
#define BQ27220_SOC_REG 0x2C   // StateOfCharge() command code (%)
#define BQ27220_CUR_REG 0x0C   // Current() command code (signed mA)
#define BQ27220_VOLT_REG 0x08  // Voltage() command code (mV)

// Analog DS3231 RTC I2C
#define I2C_ADDR_DS3231 0x68  // RTC I2C address
#define DS3231_SEC_REG 0x00   // Seconds command code (BCD)

// QST QMI8658 IMU I2C
#define I2C_ADDR_QMI8658 0x6B        // IMU I2C address
#define I2C_ADDR_QMI8658_ALT 0x6A    // IMU I2C fallback address
#define QMI8658_WHO_AM_I_REG 0x00    // WHO_AM_I command code
#define QMI8658_WHO_AM_I_VALUE 0x05  // WHO_AM_I expected value

class HalGPIO {
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
#endif

  bool lastUsbConnected = false;
  bool usbStateChanged = false;

  static constexpr unsigned long MURPHY_LONG_PRESS_MS = 700;

  uint8_t murphyRawState = 0;
  uint8_t murphyLastRawState = 0;
  uint8_t murphyPhysicalState = 0;
  uint8_t murphyCurrentState = 0;
  uint8_t murphyPressedEvents = 0;
  uint8_t murphyReleasedEvents = 0;
  unsigned long murphyLastDebounceTime = 0;
  unsigned long murphyPressStart = 0;
  unsigned long murphyPressFinish = 0;
  unsigned long murphyPowerPressStart = 0;
  unsigned long murphyPowerPressFinish = 0;
  bool murphyPowerLongPressActive = false;
  bool murphyLongPressHandled = false;
  bool murphyFrontlightEvent = false;
  bool murphyScreenshotEvent = false;
  bool murphySleepEvent = false;

 public:
  using DeviceType = BoardModel;

 private:
  DeviceType _deviceType = DeviceType::X4;

 public:
  HalGPIO() = default;

  // Inline device type helpers for cleaner downstream checks
  inline bool deviceIsX3() const { return _deviceType == DeviceType::X3; }
  inline bool deviceIsX4() const { return _deviceType == DeviceType::X4; }
  inline bool deviceIsMurphyM4() const { return _deviceType == DeviceType::MurphyM4; }
  inline bool deviceIsHz52() const { return _deviceType == DeviceType::HZ52; }
  inline const BoardCapabilityProfile& getBoardProfile() const { return boardProfileFor(_deviceType); }

  // Start button GPIO and setup SPI for screen and SD card
  void begin();

  // Button input methods
  void update();
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  bool wasFrontlightButtonReleased() const;
  bool wasScreenshotButtonReleased() const;
  bool wasSleepButtonReleased() const;
  unsigned long getHeldTime() const;
  unsigned long getPowerButtonHeldTime() const;

  // Setup wake up GPIO and enter deep sleep
  void startDeepSleep();

  // Verify power button was held long enough after wakeup.
  // If verification fails, enters deep sleep and does not return.
  // Should only be called when wakeup reason is PowerButton.
  void verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed);

  // Check if USB is connected
  bool isUsbConnected() const;

  // Returns true once per edge (plug or unplug) since the last update()
  bool wasUsbStateChanged() const;

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };

  WakeupReason getWakeupReason() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};

extern HalGPIO gpio;
