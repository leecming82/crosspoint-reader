#include "ClockOffsetActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchNavigator.h"
#include "util/TouchUi.h"

namespace {
constexpr uint8_t MAX_POS_HOURS = 14;
constexpr uint8_t MAX_NEG_HOURS = 12;
constexpr uint8_t MINUTE_STEPS = 4;  // 0, 15, 30, 45
constexpr uint8_t MINUTES_PER_QUARTER = 15;
constexpr uint8_t BIAS_QUARTER_HOURS = 48;  // 0 stored = UTC-12, 48 stored = UTC+0
#ifdef CROSSPOINT_BOARD_MURPHY_M4
constexpr int FIELD_PADDING_X = 18;
constexpr int FIELD_HEIGHT = 54;
constexpr int LABEL_GAP = 20;
constexpr int FIELD_GAP = 18;
constexpr int COLON_GAP = 8;
constexpr int ADJUST_BUTTON_HEIGHT = 72;
constexpr int ADJUST_BUTTON_BOTTOM_MARGIN = 18;
#else
constexpr int FIELD_PADDING_X = 6;
constexpr int FIELD_HEIGHT_EXTRA = 2;
constexpr int LABEL_GAP = 16;
constexpr int FIELD_GAP = 12;
constexpr int COLON_GAP = 5;
constexpr int ADJUST_BUTTON_HEIGHT = 54;
constexpr int ADJUST_BUTTON_BOTTOM_MARGIN = 16;
#endif

struct OffsetFieldLayout {
  Rect sign;
  Rect hours;
  Rect minutes;
};

// Convert a (sign, hours, quarter) triple into the biased storage value.
// Returns a value in [0, 104].
uint8_t encodeOffset(uint8_t sign, uint8_t hours, uint8_t quarter) {
  int signedQuarter = static_cast<int>(hours) * 4 + static_cast<int>(quarter);
  if (sign == 1) signedQuarter = -signedQuarter;
  int biased = signedQuarter + BIAS_QUARTER_HOURS;
  if (biased < 0) biased = 0;
  if (biased > 104) biased = 104;
  return static_cast<uint8_t>(biased);
}

// Decompose the biased storage value into (sign, hours, quarter).
void decodeOffset(uint8_t biased, uint8_t& sign, uint8_t& hours, uint8_t& quarter) {
  if (biased > 104) biased = BIAS_QUARTER_HOURS;
  int signedQuarter = static_cast<int>(biased) - BIAS_QUARTER_HOURS;
  if (signedQuarter < 0) {
    sign = 1;
    signedQuarter = -signedQuarter;
  } else {
    sign = 0;
  }
  hours = static_cast<uint8_t>(signedQuarter / 4);
  quarter = static_cast<uint8_t>(signedQuarter % 4);
}

OffsetFieldLayout offsetFieldLayout(const GfxRenderer& renderer) {
  auto widthOf = [&](const char* s) { return renderer.getTextWidth(UI_12_FONT_ID, s, EpdFontFamily::BOLD); };
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const int fieldHeight = FIELD_HEIGHT;
#else
  const int fieldHeight = lineHeight + FIELD_HEIGHT_EXTRA;
#endif
  const int centreY = renderer.getScreenHeight() / 2 - 40;
  const int fieldY = centreY - (fieldHeight - lineHeight) / 2;
  const int labelWidth = widthOf("UTC");
  const int signBoxW = std::max(widthOf("+"), widthOf("-")) + FIELD_PADDING_X * 2;
  const int hoursBoxW = std::max(widthOf("14"), widthOf("12")) + FIELD_PADDING_X * 2;
  const int colonWidth = widthOf(":");
  const int minutesBoxW = std::max({widthOf("00"), widthOf("15"), widthOf("30"), widthOf("45")}) + FIELD_PADDING_X * 2;
  const int totalWidth =
      labelWidth + LABEL_GAP + signBoxW + FIELD_GAP + hoursBoxW + COLON_GAP + colonWidth + COLON_GAP + minutesBoxW;

  int x = (renderer.getScreenWidth() - totalWidth) / 2 + labelWidth + LABEL_GAP;
  const Rect signRect{x, fieldY, signBoxW, fieldHeight};
  x += signBoxW + FIELD_GAP;
  const Rect hoursRect{x, fieldY, hoursBoxW, fieldHeight};
  x += hoursBoxW + COLON_GAP + colonWidth + COLON_GAP;
  const Rect minutesRect{x, fieldY, minutesBoxW, fieldHeight};
  return OffsetFieldLayout{signRect, hoursRect, minutesRect};
}

Rect minusButtonRect(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int gap = metrics.contentSidePadding;
  const int buttonWidth = (renderer.getScreenWidth() - metrics.contentSidePadding * 2 - gap) / 2;
  const int buttonY = renderer.getScreenHeight() - ADJUST_BUTTON_HEIGHT - ADJUST_BUTTON_BOTTOM_MARGIN;
  return Rect{metrics.contentSidePadding, buttonY, buttonWidth, ADJUST_BUTTON_HEIGHT};
}

Rect plusButtonRect(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect minusRect = minusButtonRect(renderer);
  return Rect{minusRect.x + minusRect.width + metrics.contentSidePadding, minusRect.y, minusRect.width,
              minusRect.height};
}
}  // namespace

void ClockOffsetActivity::onEnter() {
  Activity::onEnter();
  loadFromSettings();
  activeField = FIELD_HOURS;
  requestUpdate();
}

void ClockOffsetActivity::onExit() {
  saveToSettings();
  Activity::onExit();
}

void ClockOffsetActivity::loadFromSettings() {
  decodeOffset(SETTINGS.clockUtcOffsetQ, sign, hours, minutesQuarter);
  clampForSign();
}

void ClockOffsetActivity::saveToSettings() const {
  const uint8_t encoded = encodeOffset(sign, hours, minutesQuarter);
  if (encoded == SETTINGS.clockUtcOffsetQ) return;
  SETTINGS.clockUtcOffsetQ = encoded;
  SETTINGS.saveToFile();
}

void ClockOffsetActivity::clampForSign() {
  const uint8_t maxHours = (sign == 1) ? MAX_NEG_HOURS : MAX_POS_HOURS;
  if (hours > maxHours) hours = maxHours;
  // At the absolute boundary (-12:00 or +14:00) only :00 is valid.
  if (hours == maxHours && minutesQuarter != 0) {
    minutesQuarter = 0;
  }
}

void ClockOffsetActivity::adjustActiveField(int delta) {
  switch (activeField) {
    case FIELD_SIGN: {
      sign = static_cast<uint8_t>((sign + 1) % 2);
      clampForSign();
      break;
    }
    case FIELD_HOURS: {
      const uint8_t maxHours = (sign == 1) ? MAX_NEG_HOURS : MAX_POS_HOURS;
      const int next = (static_cast<int>(hours) + delta + (maxHours + 1)) % (maxHours + 1);
      hours = static_cast<uint8_t>(next);
      clampForSign();
      break;
    }
    case FIELD_MINUTES: {
      // At the boundary hour, lock minutes to :00.
      const uint8_t maxHours = (sign == 1) ? MAX_NEG_HOURS : MAX_POS_HOURS;
      if (hours == maxHours) {
        minutesQuarter = 0;
        break;
      }
      const int next = (static_cast<int>(minutesQuarter) + delta + MINUTE_STEPS) % MINUTE_STEPS;
      minutesQuarter = static_cast<uint8_t>(next);
      break;
    }
    default:
      break;
  }
}

void ClockOffsetActivity::loop() {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
    finish();
    return;
  }

  const Rect minusRect = minusButtonRect(renderer);
  const Rect plusRect = plusButtonRect(renderer);
  if (TouchNavigator::wasTappedIn(mappedInput, minusRect)) {
    adjustActiveField(-1);
    requestUpdate();
    return;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, plusRect)) {
    adjustActiveField(+1);
    requestUpdate();
    return;
  }

  const OffsetFieldLayout fields = offsetFieldLayout(renderer);
  if (TouchNavigator::wasTappedIn(mappedInput, fields.sign)) {
    activeField = FIELD_SIGN;
    requestUpdate();
    return;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, fields.hours)) {
    activeField = FIELD_HOURS;
    requestUpdate();
    return;
  }
  if (TouchNavigator::wasTappedIn(mappedInput, fields.minutes)) {
    activeField = FIELD_MINUTES;
    requestUpdate();
    return;
  }
#endif

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activeField = static_cast<Field>((activeField + 1) % FIELD_COUNT);
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    adjustActiveField(+1);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    adjustActiveField(-1);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    adjustActiveField(+1);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    adjustActiveField(-1);
    requestUpdate();
  });
}

void ClockOffsetActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  TouchUi::drawHeaderWithBack(renderer, screen, tr(STR_CLOCK_UTC_OFFSET));
#else
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLOCK_UTC_OFFSET));
#endif

  const int centreY = pageHeight / 2 - 40;
  auto widthOf = [&](const char* s) { return renderer.getTextWidth(UI_12_FONT_ID, s, EpdFontFamily::BOLD); };
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const int fieldHeight = FIELD_HEIGHT;
#else
  const int fieldHeight = lineHeight + FIELD_HEIGHT_EXTRA;
#endif
  const int fieldTextY = centreY - (fieldHeight - lineHeight) / 2 + (fieldHeight - lineHeight) / 2;
  const OffsetFieldLayout fields = offsetFieldLayout(renderer);

  char signStr[2] = {sign == 1 ? '-' : '+', '\0'};
  char hoursStr[8];
  snprintf(hoursStr, sizeof(hoursStr), "%d", hours);
  char minutesStr[8];
  snprintf(minutesStr, sizeof(minutesStr), "%02d", minutesQuarter * MINUTES_PER_QUARTER);

  const int labelWidth = widthOf("UTC");
  const int labelX = fields.sign.x - LABEL_GAP - labelWidth;

  renderer.drawText(UI_12_FONT_ID, labelX, fieldTextY, "UTC", true, EpdFontFamily::BOLD);

  auto drawField = [&](const char* text, const Rect rect, const Field field) {
    const bool selected = activeField == field;
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, selected ? Color::LightGray : Color::White);
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
    if (selected) {
      renderer.drawRect(rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2, true);
    }
    const int textX = rect.x + (rect.width - widthOf(text)) / 2;
    const int textY = rect.y + (rect.height - lineHeight) / 2;
    renderer.drawText(UI_12_FONT_ID, textX, textY, text, true, EpdFontFamily::BOLD);
  };

  drawField(signStr, fields.sign, FIELD_SIGN);
  drawField(hoursStr, fields.hours, FIELD_HOURS);
  const int colonX = fields.hours.x + fields.hours.width + COLON_GAP;
  renderer.drawText(UI_12_FONT_ID, colonX, fieldTextY, ":", true, EpdFontFamily::BOLD);
  drawField(minutesStr, fields.minutes, FIELD_MINUTES);

  // Live preview of the resulting wall-clock time, so users can verify against a watch.
  if (halClock.isAvailable()) {
    char timeBuf[9];
    const uint8_t encoded = encodeOffset(sign, hours, minutesQuarter);
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), encoded, SETTINGS.clockFormat == 1)) {
      char preview[24];
      snprintf(preview, sizeof(preview), "%s %s", tr(STR_CURRENT_TIME), timeBuf);
      renderer.drawCenteredText(UI_10_FONT_ID, centreY + 60, preview);
    }
  }

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const Rect minusRect = minusButtonRect(renderer);
  const Rect plusRect = plusButtonRect(renderer);
  TouchUi::drawTouchButton(renderer, minusRect, "-");
  TouchUi::drawTouchButton(renderer, plusRect, "+");
#else
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_NEXT_FIELD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

  renderer.displayBuffer();
}
