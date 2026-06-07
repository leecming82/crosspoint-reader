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
    .hasChargerControl = false,
    .hasTiltSensor = false,
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
    .hasChargerControl = true,
    .hasTiltSensor = true,
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
    .hasFrontlight = false,
    .frontlightChannels = 0,
    .hasRtc = false,
    .hasBatteryGauge = false,
    .hasChargerControl = false,
    .hasTiltSensor = false,
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

}  // namespace

const BoardCapabilityProfile& boardProfileFor(BoardModel model) {
  switch (model) {
    case BoardModel::X3:
      return X3_PROFILE;
    case BoardModel::MurphyM4:
      return MURPHY_M4_PROFILE;
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
