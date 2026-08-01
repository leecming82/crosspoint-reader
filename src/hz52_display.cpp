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
// buffer is 115,200 bytes.
//
// MODE_PACKING_8PPB shares CrossPoint's polarity (0 = black, 1 = white) but NOT its bit
// order within a byte. CrossPoint packs MSB = leftmost pixel; epdiy's 8ppB LUT walks the
// byte the other way (lut_8ppB_start_at_white[0x01] alters the low output slot and [0x80]
// the high one, and the panel shifts that run out in the opposite sense). Handing the
// buffer over unconverted mirrors every 8-pixel run.
//
// That is invisible on solid fills, which is why the border and rules looked crisp, but it
// wrecks glyphs -- and because the panel is transposed (phyX is *logical y*), an 8-pixel
// run is vertical on screen, so glyph rows appeared displaced up/down in 8-pixel groups.
// Converting here keeps CrossPoint's MSB-first convention intact everywhere else.
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

// Scratch holding the bit-reversed copy handed to epdiy. A second buffer rather than an
// in-place flip so panelBuffer stays in CrossPoint's convention and is safe to re-push or
// read back; 115 KB of PSRAM against ~8 MB free.
uint8_t* pushBuffer = nullptr;

// Flash-resident reverse-bits-in-a-byte table: static const, so it stays out of DRAM.
constexpr uint8_t reverseByte(uint8_t v) {
  uint8_t r = 0;
  for (int i = 0; i < 8; i++) r = static_cast<uint8_t>((r << 1) | ((v >> i) & 1));
  return r;
}
struct ReverseTable {
  uint8_t v[256];
};
constexpr ReverseTable makeReverseTable() {
  ReverseTable t{};
  for (int i = 0; i < 256; i++) t.v[i] = reverseByte(static_cast<uint8_t>(i));
  return t;
}
constexpr ReverseTable REVERSE_BITS = makeReverseTable();

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
  pushBuffer = static_cast<uint8_t*>(heap_caps_malloc(panelBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!pushBuffer) {
    heap_caps_free(panelBuffer);
    panelBuffer = nullptr;
    LOG_ERR("EPD", "OOM: %u bytes for push scratch", static_cast<unsigned>(panelBufferBytes));
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

  // Convert MSB-first (CrossPoint) to the order epdiy's 8ppB LUT expects. See the file
  // header: without this every 8-pixel run is mirrored, which the transpose turns into
  // glyph rows displaced vertically in 8-pixel groups.
  for (size_t i = 0; i < panelBufferBytes; i++) {
    pushBuffer[i] = REVERSE_BITS.v[panelBuffer[i]];
  }

  epd_poweron();
  const uint32_t start = millis();
  const enum EpdDrawError err = epd_draw_base(
      epd_full_screen(), pushBuffer, epd_full_screen(),
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

#endif
