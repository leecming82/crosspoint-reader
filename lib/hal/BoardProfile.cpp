#include "BoardProfile.h"

namespace {

constexpr BoardCapabilityProfile X4_PROFILE = {
    .model = BoardModel::X4,
    .id = "x4",
    .label = "X4",
    .socFamily = SocFamily::ESP32C3,
    .hasPsram = false,
    .psramCacheBudgetBytes = 0,
    .displayWidth = 800,
    .displayHeight = 480,
    .visibleWidth = 800,
    .visibleHeight = 480,
    .displayGrayscaleBits = 2,
    .displayPartialRefresh = true,
    .displaySingleBufferRequired = true,
    .inputButtonCount = 7,
    .inputHasTouch = false,
    .touchController = "none",
    .hasFrontlight = false,
    .frontlightChannels = 0,
    .hasRtc = false,
    .hasBatteryGauge = false,
    .hasChargerControl = true,
    .hasTiltSensor = false,
    .hasEnvironmentalSensor = false,
    .sdRequired = true,
    .sdUsesSdMmc = false,
    .sdMmc4Bit = false,
    .sdEnablePin = -1,
    .sdEnableActiveLow = false,
    .sdClkPin = -1,
    .sdCmdPin = -1,
    .sdD0Pin = -1,
    .sdD1Pin = -1,
    .sdD2Pin = -1,
    .sdD3Pin = -1,
};

constexpr BoardCapabilityProfile X3_PROFILE = {
    .model = BoardModel::X3,
    .id = "x3",
    .label = "X3",
    .socFamily = SocFamily::ESP32C3,
    .hasPsram = false,
    .psramCacheBudgetBytes = 0,
    .displayWidth = 528,
    .displayHeight = 880,
    .visibleWidth = 528,
    .visibleHeight = 880,
    .displayGrayscaleBits = 2,
    .displayPartialRefresh = true,
    .displaySingleBufferRequired = true,
    .inputButtonCount = 7,
    .inputHasTouch = false,
    .touchController = "none",
    .hasFrontlight = false,
    .frontlightChannels = 0,
    .hasRtc = true,
    .hasBatteryGauge = true,
    .hasChargerControl = false,
    .hasTiltSensor = true,
    .hasEnvironmentalSensor = false,
    .sdRequired = true,
    .sdUsesSdMmc = false,
    .sdMmc4Bit = false,
    .sdEnablePin = -1,
    .sdEnableActiveLow = false,
    .sdClkPin = -1,
    .sdCmdPin = -1,
    .sdD0Pin = -1,
    .sdD1Pin = -1,
    .sdD2Pin = -1,
    .sdD3Pin = -1,
};

constexpr BoardCapabilityProfile MURPHY_M4_PROFILE = {
    .model = BoardModel::MurphyM4,
    .id = "murphy_m4",
    .label = "Murphy M4",
    .socFamily = SocFamily::ESP32S3,
    .hasPsram = true,
    .psramCacheBudgetBytes = 0,
    .displayWidth = 800,
    .displayHeight = 480,
    .visibleWidth = 800,
    .visibleHeight = 480,
    .displayGrayscaleBits = 2,
    .displayPartialRefresh = true,
    .displaySingleBufferRequired = true,
    .inputButtonCount = 3,
    .inputHasTouch = true,
    .touchController = "FT6336U@0x2E",
    .hasFrontlight = true,
    .frontlightChannels = 2,
    .hasRtc = true,
    .hasBatteryGauge = true,
    .hasChargerControl = true,
    .hasTiltSensor = false,
    .hasEnvironmentalSensor = true,
    .sdRequired = true,
    .sdUsesSdMmc = true,
    .sdMmc4Bit = true,
    .sdEnablePin = 10,
    .sdEnableActiveLow = true,
    .sdClkPin = 16,
    .sdCmdPin = 15,
    .sdD0Pin = 17,
    .sdD1Pin = 18,
    .sdD2Pin = 11,
    .sdD3Pin = 14,
};

// HZ5.2: 1280x720 ED052TC4 panel driven over an 8-bit parallel bus by epdiy, not SPI.
// Capabilities start deliberately conservative and are promoted only as hardware
// evidence lands (same policy as the Murphy bring-up):
//   - psramCacheBudgetBytes stays 0 until the epdiy framebuffer pair (~900 KB of
//     PSRAM for front+back at 4bpp) is allocated and the remainder measured.
//   - displayGrayscaleBits stays 1 until the 4bpp render path exists; the panel
//     supports 16 levels via a borrowed ED047 waveform.
//   - displayPartialRefresh stays false until proven on our own driver.
// SD is SPI-attached (FSPI CLK=3, MISO=2, MOSI=43), so every sdMmc* field is
// inapplicable and left at the X4-style defaults.
// See docs/hz52-device-migration-comparison.md for the full pin map.
constexpr BoardCapabilityProfile HZ52_PROFILE = {
    .model = BoardModel::HZ52,
    .id = "hz52",
    .label = "HZ5.2",
    .socFamily = SocFamily::ESP32S3,
    .hasPsram = true,
    .psramCacheBudgetBytes = 0,
    .displayWidth = 1280,
    .displayHeight = 720,
    .visibleWidth = 1280,
    .visibleHeight = 720,
    .displayGrayscaleBits = 1,
    .displayPartialRefresh = false,
    .displaySingleBufferRequired = false,
    .inputButtonCount = 3,
    .inputHasTouch = false,
    .touchController = "none",
    .hasFrontlight = false,
    .frontlightChannels = 0,
    .hasRtc = false,
    .hasBatteryGauge = false,
    .hasChargerControl = false,
    .hasTiltSensor = false,
    .hasEnvironmentalSensor = false,
    .sdRequired = true,
    .sdUsesSdMmc = false,
    .sdMmc4Bit = false,
    // GPIO46 is a real power gate, not a buffer enable: toggling it while mounted makes
    // the card lose state entirely. Chip-select is GPIO44; the SD_MMC pin fields below
    // do not apply because this board runs SD over SPI, one lane, like X3/X4.
    .sdEnablePin = 46,
    .sdEnableActiveLow = false,
    .sdClkPin = -1,
    .sdCmdPin = -1,
    .sdD0Pin = -1,
    .sdD1Pin = -1,
    .sdD2Pin = -1,
    .sdD3Pin = -1,
};

}  // namespace

const BoardCapabilityProfile& boardProfileFor(BoardModel model) {
  switch (model) {
    case BoardModel::X3:
      return X3_PROFILE;
    case BoardModel::MurphyM4:
      return MURPHY_M4_PROFILE;
    case BoardModel::HZ52:
      return HZ52_PROFILE;
    case BoardModel::X4:
    default:
      return X4_PROFILE;
  }
}

const char* boardModelName(BoardModel model) { return boardProfileFor(model).label; }

const char* socFamilyName(SocFamily family) {
  switch (family) {
    case SocFamily::ESP32S3:
      return "ESP32-S3";
    case SocFamily::ESP32C3:
    default:
      return "ESP32-C3";
  }
}
