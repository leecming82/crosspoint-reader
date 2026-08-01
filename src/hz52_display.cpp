// HZ5.2 panel driver (milestone 4).
//
// The board is an epdiy V7 derivative with the PCA9555 expander removed, which upstream
// ships as `epd_board_v7_raw`. Every pin matches what we measured off the vendor firmware
// over JTAG, so no custom board definition is required:
//
//   data D0..D7 = 5,6,7,15,16,17,18,8   CKH=4  STH=41  LEH=42  STV=45  CKV=48
//   OE=9  MODE=10  PWRUP=11  VCOM_CTRL=12  WAKEUP=14  PWRGOOD=47  INT=13
//   I2C SCL=40 SDA=39 (TPS65185 @ 0x68)
//
// Deliberately uses the low-level epd_draw_base() rather than the epd_hl_* API: the
// high-level state allocates 1.84 MB of PSRAM for its front/back diff pair, while a 1bpp
// buffer is 115,200 bytes. MODE_PACKING_8PPB is "0 = black, 1 = white, MSB leftmost",
// which is exactly CrossPoint's existing framebuffer convention, so GfxRenderer's output
// can eventually be handed over with no conversion at all.
#ifdef CROSSPOINT_BOARD_HZ52

#include "hz52_display.h"

#include <Arduino.h>
#include <Logging.h>
#include <epdiy.h>
#include <esp_heap_caps.h>

#include <cstring>

namespace {

// Panel calibration read from the stock UI before it was overwritten. epdiy takes the
// magnitude in millivolts; the rail itself is negative. Must be applied before any
// refresh -- a wrong value degrades the panel over time, not just contrast.
constexpr int VCOM_MV = 2700;

// The default waveform's temperature bands do not cover the ~31 C this board reports, and
// a value outside them makes epd_draw_base() return EPD_DRAW_NO_PHASES_AVAILABLE. epdiy's
// own docs say the default waveforms ignore this and it should be room temperature.
constexpr int DRAW_TEMPERATURE_C = 25;

uint8_t* panelBuffer = nullptr;
size_t panelBufferBytes = 0;
bool initialised = false;
bool panelIsWhite = false;

}  // namespace

namespace Hz52Display {

bool begin() {
  if (initialised) return true;

  epd_init(&epd_board_v7_raw, &ED052TC4, EPD_OPTIONS_DEFAULT);
  epd_set_vcom(VCOM_MV);

  // One allocation for the life of the device; never reallocated per refresh.
  panelBufferBytes = static_cast<size_t>(epd_width() / 8) * epd_height();
  panelBuffer = static_cast<uint8_t*>(heap_caps_malloc(panelBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!panelBuffer) {
    LOG_ERR("EPD", "OOM: %u bytes for panel framebuffer", static_cast<unsigned>(panelBufferBytes));
    return false;
  }
  memset(panelBuffer, 0xFF, panelBufferBytes);  // 0xFF = white, same polarity as EInkDisplay

  initialised = true;
  LOG_INF("EPD", "Panel ready: %dx%d, %u byte 1bpp buffer, VCOM -%d.%02d V", epd_width(), epd_height(),
          static_cast<unsigned>(panelBufferBytes), VCOM_MV / 1000, (VCOM_MV % 1000) / 10);
  return true;
}

uint8_t* frameBuffer() { return panelBuffer; }
size_t frameBufferSize() { return panelBufferBytes; }
int panelWidth() { return epd_width(); }
int panelHeight() { return epd_height(); }

void clear() {
  if (!initialised) return;
  // Mandatory on first boot after other firmware: epdiy has no idea what is physically on
  // the glass, and a differential update would leave the previous image in place.
  epd_poweron();
  epd_clear();
  epd_poweroff();
  panelIsWhite = true;
}

bool push() {
  if (!initialised) return false;

  // MODE_PACKING_8PPB needs PREVIOUSLY_WHITE/BLACK: a 1bpp buffer carries no "from" state,
  // so without it find_lut_functions() has no LUT and the draw fails with
  // EPD_DRAW_LOOKUP_NOT_IMPLEMENTED. Only valid while the panel really is uniform.
  if (!panelIsWhite) clear();

  epd_poweron();
  const uint32_t start = millis();
  const enum EpdDrawError err = epd_draw_base(
      epd_full_screen(), panelBuffer, epd_full_screen(),
      static_cast<enum EpdDrawMode>(MODE_DU | MODE_PACKING_8PPB | PREVIOUSLY_WHITE), DRAW_TEMPERATURE_C, NULL, NULL,
      epd_get_display()->default_waveform);
  const uint32_t elapsed = millis() - start;
  epd_poweroff();

  panelIsWhite = false;
  if (err != EPD_DRAW_SUCCESS) {
    LOG_ERR("EPD", "Draw failed: err=0x%x", static_cast<int>(err));
    return false;
  }
  LOG_INF("EPD", "Drew full screen in %lums", static_cast<unsigned long>(elapsed));
  return true;
}

void setLogicalPixel(int lx, int ly, bool black) {
  // Measured mapping (origin-"L" test): framebuffer x is physical vertical increasing
  // downward, framebuffer y is physical horizontal increasing rightward, origin top-left.
  // So logical portrait (720 wide x 1280 tall) transposes onto the panel.
  if (lx < 0 || ly < 0 || lx >= epd_height() || ly >= epd_width()) return;
  const int fx = ly;
  const int fy = lx;
  uint8_t* byte = &panelBuffer[static_cast<size_t>(fy) * (epd_width() / 8) + (fx >> 3)];
  const uint8_t mask = static_cast<uint8_t>(0x80 >> (fx & 7));
  if (black) {
    *byte &= static_cast<uint8_t>(~mask);
  } else {
    *byte |= mask;
  }
}

void fillLogicalRect(int lx, int ly, int w, int h, bool black) {
  for (int y = ly; y < ly + h; ++y) {
    for (int x = lx; x < lx + w; ++x) setLogicalPixel(x, y, black);
  }
}

int logicalWidth() { return epd_height(); }   // 720
int logicalHeight() { return epd_width(); }   // 1280

}  // namespace Hz52Display

// Bring-up demo: draw a portrait "page" of decreasing-width bars, so correct orientation
// is obvious at a glance -- it should read as left-aligned text lines running down the
// portrait screen, not sideways.
void hz52DisplayDemo() {
  if (!Hz52Display::begin()) return;

  const int lw = Hz52Display::logicalWidth();
  const int lh = Hz52Display::logicalHeight();
  memset(Hz52Display::frameBuffer(), 0xFF, Hz52Display::frameBufferSize());

  Hz52Display::fillLogicalRect(0, 0, lw, 8, true);            // top rule, full width
  Hz52Display::fillLogicalRect(0, lh - 8, lw, 8, true);       // bottom rule
  static const int widths[] = {88, 72, 90, 55, 84, 68, 40};   // ragged right, like text
  int y = 80;
  for (const int pct : widths) {
    Hz52Display::fillLogicalRect(40, y, (lw - 80) * pct / 100, 26, true);
    y += 60;
  }
  Hz52Display::fillLogicalRect(40, lh - 90, 120, 26, true);   // footer marker, bottom-left

  LOG_INF("EPD", "Demo page: logical %dx%d portrait", lw, lh);
  Hz52Display::clear();
  Hz52Display::push();
}

#endif
