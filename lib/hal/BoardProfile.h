#pragma once

#include <Arduino.h>

enum class BoardModel : uint8_t { X4, X3, MurphyM4 };
enum class SocFamily : uint8_t { ESP32C3, ESP32S3 };

struct BoardCapabilityProfile {
  BoardModel model;
  const char* id;
  const char* label;
  SocFamily socFamily;

  bool hasPsram;
  uint32_t psramCacheBudgetBytes;

  uint16_t displayWidth;
  uint16_t displayHeight;
  uint16_t visibleWidth;
  uint16_t visibleHeight;
  uint8_t displayGrayscaleBits;
  bool displayPartialRefresh;
  bool displaySingleBufferRequired;

  uint8_t inputButtonCount;
  bool inputHasTouch;
  const char* touchController;

  bool hasFrontlight;
  uint8_t frontlightChannels;
  bool hasRtc;
  bool hasBatteryGauge;
  bool hasChargerControl;
  bool hasTiltSensor;
  bool hasEnvironmentalSensor;
  bool sdRequired;
  bool sdUsesSdMmc;
  bool sdMmc4Bit;
  int8_t sdEnablePin;
  bool sdEnableActiveLow;
  int8_t sdClkPin;
  int8_t sdCmdPin;
  int8_t sdD0Pin;
  int8_t sdD1Pin;
  int8_t sdD2Pin;
  int8_t sdD3Pin;
};

const BoardCapabilityProfile& boardProfileFor(BoardModel model);
const char* boardModelName(BoardModel model);
const char* socFamilyName(SocFamily family);
