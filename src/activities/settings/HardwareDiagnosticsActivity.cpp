#include "HardwareDiagnosticsActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalEnvSensor.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchNavigator.h"
#include "util/TouchUi.h"

namespace {
constexpr int FRONTLIGHT_DUTY_LEVELS[] = {0, 16, 32, 64, 96, 128, 192, 255};

uint16_t diagnosticBatteryPercent(const uint16_t millivolts) {
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

int diagnosticValueX(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;
  const char* labels[] = {
      "Sample time", "GPIO43", "GPIO9", "Temp/RH", "Heap", "PSRAM", "GPIO47 cool", "GPIO48 warm",
  };

  int maxLabelWidth = 0;
  for (const char* label : labels) {
    maxLabelWidth = std::max(maxLabelWidth, renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD));
  }

  return x + maxLabelWidth + 24;
}

void drawDiagnosticRow(const GfxRenderer& renderer, const int y, const int valueX, const char* label,
                       const char* value) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;
  const int width = renderer.getScreenWidth() - metrics.contentSidePadding * 2;
  const int valueWidth = std::max(0, x + width - valueX);
  const std::string displayValue = renderer.truncatedText(UI_12_FONT_ID, value, valueWidth);

  renderer.drawText(UI_12_FONT_ID, x, y, label, true, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, valueX, y, displayValue.c_str(), true);
}

void formatMemory(char* out, const size_t outSize, const uint32_t bytes) {
  if (bytes >= 1024 * 1024) {
    snprintf(out, outSize, "%.1f M", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return;
  }
  snprintf(out, outSize, "%lu K", static_cast<unsigned long>((bytes + 512) / 1024));
}
}  // namespace

void HardwareDiagnosticsActivity::onEnter() {
  Activity::onEnter();
  halFrontlight.begin();
  halFrontlight.set(SETTINGS.frontlightCoolDuty, SETTINGS.frontlightWarmDuty);
  frontlightDirty = false;
  refreshReadings();
  requestUpdate();
}

void HardwareDiagnosticsActivity::onExit() {
  if (frontlightDirty) {
    SETTINGS.saveToFile();
    frontlightDirty = false;
  }
  Activity::onExit();
}

void HardwareDiagnosticsActivity::refreshReadings() {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  pinMode(MURPHY_CHARGE_STATUS_PIN, INPUT_PULLUP);
  pinMode(MURPHY_BATTERY_ADC_PIN, INPUT);
  chargeRaw = digitalRead(MURPHY_CHARGE_STATUS_PIN);
  charging = chargeRaw == LOW ? 1 : 0;
  batteryRaw = analogRead(MURPHY_BATTERY_ADC_PIN);
  batterySenseMv = analogReadMilliVolts(MURPHY_BATTERY_ADC_PIN);
  batterySystemMv = std::min(batterySenseMv * 2, 5000);
  batteryPercent = diagnosticBatteryPercent(static_cast<uint16_t>(batterySystemMv));
  envAvailable = halEnvSensor.isAvailable();
  HalEnvSensor::Reading envReading;
  envReadOk = envAvailable && halEnvSensor.read(envReading);
  if (envReadOk) {
    envTemperatureC = envReading.temperatureC;
    envHumidityPercent = envReading.humidityPercent;
  }
  sampleMs = millis();
  LOG_INF("DIAG",
          "Murphy hardware diagnostics GPIO43=%d charging=%d GPIO9 raw=%d senseMv=%d batteryMv=%d pct=%d "
          "env=%d envOk=%d tempC=%.1f rh=%.1f frontlight47=%d frontlight48=%d",
          chargeRaw, charging, batteryRaw, batterySenseMv, batterySystemMv, batteryPercent, envAvailable ? 1 : 0,
          envReadOk ? 1 : 0, envTemperatureC, envHumidityPercent, SETTINGS.frontlightCoolDuty, SETTINGS.frontlightWarmDuty);
#else
  chargeRaw = -1;
  charging = -1;
  batteryRaw = -1;
  batterySenseMv = -1;
  batterySystemMv = -1;
  batteryPercent = -1;
  envAvailable = false;
  envReadOk = false;
  sampleMs = millis();
#endif
}

Rect HardwareDiagnosticsActivity::refreshButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, renderer.getScreenHeight() - metrics.listRowHeight, renderer.getScreenWidth(), metrics.listRowHeight};
}

Rect HardwareDiagnosticsActivity::frontlight47Rect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 18 +
                (renderer.getLineHeight(UI_12_FONT_ID) + 14) * 6 - 8;
  return Rect{0, y, renderer.getScreenWidth(), renderer.getLineHeight(UI_12_FONT_ID) + 20};
}

Rect HardwareDiagnosticsActivity::frontlight48Rect() const {
  const Rect row = frontlight47Rect();
  return Rect{row.x, row.y + row.height, row.width, row.height};
}

void HardwareDiagnosticsActivity::cycleFrontlightCool() {
  int nextDuty = 0;
  for (const int duty : FRONTLIGHT_DUTY_LEVELS) {
    if (duty > SETTINGS.frontlightCoolDuty) {
      nextDuty = duty;
      break;
    }
  }
  SETTINGS.frontlightCoolDuty = static_cast<uint8_t>(nextDuty);
  halFrontlight.set(SETTINGS.frontlightCoolDuty, SETTINGS.frontlightWarmDuty);
  frontlightDirty = true;
  requestUpdate();
}

void HardwareDiagnosticsActivity::cycleFrontlightWarm() {
  int nextDuty = 0;
  for (const int duty : FRONTLIGHT_DUTY_LEVELS) {
    if (duty > SETTINGS.frontlightWarmDuty) {
      nextDuty = duty;
      break;
    }
  }
  SETTINGS.frontlightWarmDuty = static_cast<uint8_t>(nextDuty);
  halFrontlight.set(SETTINGS.frontlightCoolDuty, SETTINGS.frontlightWarmDuty);
  frontlightDirty = true;
  requestUpdate();
}

void HardwareDiagnosticsActivity::loop() {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
    finish();
    return;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, frontlight47Rect())) {
    cycleFrontlightCool();
    return;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, frontlight48Rect())) {
    cycleFrontlightWarm();
    return;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, refreshButtonRect())) {
    refreshReadings();
    requestUpdate();
    return;
  }
#endif

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    refreshReadings();
    requestUpdate();
    return;
  }
}

void HardwareDiagnosticsActivity::render(RenderLock&&) {
  renderer.clearScreen();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  TouchUi::drawHeaderWithBack(renderer, screen, tr(STR_HARDWARE_DIAGNOSTICS));
#else
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_HARDWARE_DIAGNOSTICS));
#endif

  char chargeValue[32];
  char batteryValue[48];
  char envValue[48];
  char sampledValue[32];
  char heapValue[48];
  char psramValue[48];
  char frontlight47Value[32];
  char frontlight48Value[32];

  snprintf(chargeValue, sizeof(chargeValue), "%d (%s)", chargeRaw, charging > 0 ? "yes" : "no");
  snprintf(batteryValue, sizeof(batteryValue), "%d (%d mV)", batteryRaw, batterySystemMv);
  if (envReadOk) {
    snprintf(envValue, sizeof(envValue), "%.1f C (%.1f%%)", envTemperatureC, envHumidityPercent);
  } else {
    snprintf(envValue, sizeof(envValue), "%s", envAvailable ? "read error" : "not found");
  }
  snprintf(sampledValue, sizeof(sampledValue), "%.1f s", static_cast<double>(sampleMs) / 1000.0);
  formatMemory(heapValue, sizeof(heapValue), ESP.getFreeHeap());
  formatMemory(psramValue, sizeof(psramValue), ESP.getFreePsram());
  snprintf(frontlight47Value, sizeof(frontlight47Value), "duty %u", SETTINGS.frontlightCoolDuty);
  snprintf(frontlight48Value, sizeof(frontlight48Value), "duty %u", SETTINGS.frontlightWarmDuty);

  const auto& layoutMetrics = UITheme::getInstance().getMetrics();
  int y = layoutMetrics.topPadding + layoutMetrics.headerHeight + layoutMetrics.verticalSpacing + 18;
  const int lineStep = renderer.getLineHeight(UI_12_FONT_ID) + 14;
  const int valueX = diagnosticValueX(renderer);

  drawDiagnosticRow(renderer, y, valueX, "Sample time", sampledValue);
  y += lineStep;
  drawDiagnosticRow(renderer, y, valueX, "GPIO43", chargeValue);
  y += lineStep;
  drawDiagnosticRow(renderer, y, valueX, "GPIO9", batteryValue);
  y += lineStep;
  drawDiagnosticRow(renderer, y, valueX, "Temp/RH", envValue);
  y += lineStep;
  drawDiagnosticRow(renderer, y, valueX, "Heap", heapValue);
  y += lineStep;
  drawDiagnosticRow(renderer, y, valueX, "PSRAM", psramValue);
  y += lineStep;
  drawDiagnosticRow(renderer, y, valueX, "GPIO47 cool", frontlight47Value);
  y += lineStep;
  drawDiagnosticRow(renderer, y, valueX, "GPIO48 warm", frontlight48Value);

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  TouchUi::drawTouchButton(renderer, refreshButtonRect(), "Refresh");
#else
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Refresh", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

  renderer.displayBuffer();
}
