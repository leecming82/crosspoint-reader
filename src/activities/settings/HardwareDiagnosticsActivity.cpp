#include "HardwareDiagnosticsActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchNavigator.h"
#include "util/TouchUi.h"

namespace {
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

void drawDiagnosticRow(const GfxRenderer& renderer, const int y, const char* label, const char* value) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;
  const int width = renderer.getScreenWidth() - metrics.contentSidePadding * 2;
  const int labelWidth = std::min(190, width / 2);

  renderer.drawText(UI_12_FONT_ID, x, y, label, true, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x + labelWidth, y, value, true);
}
}  // namespace

void HardwareDiagnosticsActivity::onEnter() {
  Activity::onEnter();
  refreshReadings();
  requestUpdate();
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
  sampleMs = millis();
  LOG_INF("DIAG", "Murphy hardware diagnostics GPIO43=%d charging=%d GPIO9 raw=%d senseMv=%d batteryMv=%d pct=%d",
          chargeRaw, charging, batteryRaw, batterySenseMv, batterySystemMv, batteryPercent);
#else
  chargeRaw = -1;
  charging = -1;
  batteryRaw = -1;
  batterySenseMv = -1;
  batterySystemMv = -1;
  batteryPercent = -1;
  sampleMs = millis();
#endif
}

Rect HardwareDiagnosticsActivity::refreshButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, renderer.getScreenHeight() - metrics.listRowHeight, renderer.getScreenWidth(), metrics.listRowHeight};
}

void HardwareDiagnosticsActivity::loop() {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
    finish();
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

  char chargeRawValue[24];
  char chargingValue[24];
  char batteryValue[48];
  char batterySenseMvValue[32];
  char batterySystemMvValue[32];
  char batteryPercentValue[32];
  char sampledValue[32];
  char heapValue[48];
  char psramValue[48];

  snprintf(chargeRawValue, sizeof(chargeRawValue), "%d", chargeRaw);
  snprintf(chargingValue, sizeof(chargingValue), "%s", charging > 0 ? "yes" : "no");
  snprintf(batteryValue, sizeof(batteryValue), "%d", batteryRaw);
  snprintf(batterySenseMvValue, sizeof(batterySenseMvValue), "%d mV", batterySenseMv);
  snprintf(batterySystemMvValue, sizeof(batterySystemMvValue), "%d mV", batterySystemMv);
  snprintf(batteryPercentValue, sizeof(batteryPercentValue), "%d%%", batteryPercent);
  snprintf(sampledValue, sizeof(sampledValue), "%lu ms", sampleMs);
  snprintf(heapValue, sizeof(heapValue), "%u free", static_cast<unsigned>(ESP.getFreeHeap()));
  snprintf(psramValue, sizeof(psramValue), "%u free", static_cast<unsigned>(ESP.getFreePsram()));

  const auto& metrics = UITheme::getInstance().getMetrics();
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 18;
  const int lineStep = renderer.getLineHeight(UI_12_FONT_ID) + 14;

  drawDiagnosticRow(renderer, y, "GPIO43", chargeRawValue);
  y += lineStep;
  drawDiagnosticRow(renderer, y, "Charging", chargingValue);
  y += lineStep;
  drawDiagnosticRow(renderer, y, "GPIO9 raw", batteryValue);
  y += lineStep;
  drawDiagnosticRow(renderer, y, "GPIO9 sense", batterySenseMvValue);
  y += lineStep;
  drawDiagnosticRow(renderer, y, "Battery", batterySystemMvValue);
  y += lineStep;
  drawDiagnosticRow(renderer, y, "Battery %", batteryPercentValue);
  y += lineStep;
  drawDiagnosticRow(renderer, y, "Sample time", sampledValue);
  y += lineStep;
  drawDiagnosticRow(renderer, y, "Heap", heapValue);
  y += lineStep;
  drawDiagnosticRow(renderer, y, "PSRAM", psramValue);

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  TouchUi::drawTouchButton(renderer, refreshButtonRect(), "Refresh");
#else
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Refresh", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

  renderer.displayBuffer();
}
