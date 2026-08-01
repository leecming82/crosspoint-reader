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
// Uses epdiy's high-level API (epd_hl_*), which keeps front/back framebuffers and diffs
// them per pixel to drive a real waveform.
//
// The first implementation used the low-level epd_draw_base() with MODE_PACKING_8PPB and a
// 1bpp buffer -- 115 KB instead of 1.84 MB. That worked, but 1bpp has nowhere to record a
// pixel's *current* state, so epdiy has to be told PREVIOUSLY_WHITE/BLACK as a single
// blanket assumption for the whole screen, and its 8ppB LUT builder ignores the frame index
// entirely (lut.c: build_8ppB_lut_256b_from_white memcpy's one fixed table). The result was
// a constant DC push rather than a waveform: no reset phase, so ghosting was structural.
// Raising the frame count (MODE_DU 5 -> *_TO_GL16 15) tripled the duration and changed
// nothing visually, which is the signature of that problem.
//
// At 4bpp (MODE_PACKING_2PPB) each pixel carries 16 levels and epdiy's difference buffer
// holds the packed (from, to) pair, so build_2ppB_lut_64k_static_from() can index
// phases->luts + 16*4*frame -- a genuine frame-varying waveform with the reset phases that
// actually clear particle history. That is what the earlier SSD1677 boards got for free
// from their display controller's own RAM, and what a controller-less panel has to hold
// itself.
#ifdef CROSSPOINT_BOARD_HZ52

#include "hz52_display.h"

#include <Arduino.h>
#include <Logging.h>
#include <epd_highlevel.h>
#include <epdiy.h>

// Internal epdiy headers: the only route to the PMIC, since epdiy owns the I2C bus through
// IDF and Arduino Wire must never touch it. These are plain C headers with no extern "C"
// guard of their own, unlike epdiy.h, so wrap them or the symbols mangle and fail to link.
extern "C" {
#include "board/epd_board_i2c.h"
#include "board/tps65185.h"
}
#include <esp_heap_caps.h>

#include <cstring>

namespace {

// Panel calibration read from the stock UI before it was overwritten. epdiy takes the
// magnitude in millivolts; the rail itself is negative. Must be applied before any
// refresh -- a wrong value degrades the panel over time, not just contrast.
constexpr int VCOM_MV = 2700;

// Fallback only. epdiy_ED047TC2 carries 14 temperature bands spanning 0-48 C, so the real
// panel temperature is used when the PMIC gives one; this stands in if that read fails.
constexpr int FALLBACK_TEMPERATURE_C = 25;

// epd_ambient_temperature() reads a sensor on the TPS65185 *die*, not on the glass. The
// PMIC dissipates real power generating the +-15 V/+22 V rails, so it runs well above the
// panel -- it reports ~32 C on a room-temperature device. Selecting a hotter band than
// reality picks a shorter waveform (ED047TC2 DU: 15 phases at 30-33 C vs 22 at 21-24 C),
// which under-drives every transition: particles stop short of the electrode and land grey
// rather than white, leaving a shadow wherever the previous page had ink.
//
// Under-drive leaves residue; over-drive mostly costs time. So bias cold.
constexpr int PMIC_TO_PANEL_OFFSET_C = -10;

// Cached panel temperature. Reading it hits the TPS65185 over I2C, which is not worth doing
// on every page turn, and it only matters at band granularity (3 C steps) anyway.
constexpr uint32_t TEMP_REFRESH_MS = 60000;
int cachedTemperatureC = FALLBACK_TEMPERATURE_C;
uint32_t temperatureReadAt = 0;

// Must be called with the rails already up: epd_ambient_temperature() reads a TPS65185
// register and the PMIC does not ACK until WAKEUP is asserted.
int panelTemperature() {
  const uint32_t now = millis();
  if (temperatureReadAt != 0 && (now - temperatureReadAt) < TEMP_REFRESH_MS) {
    return cachedTemperatureC;
  }
  const float raw = epd_ambient_temperature();
  // Guard against an implausible read rather than feeding it to the waveform lookup.
  if (raw > -20.0f && raw < 60.0f) {
    cachedTemperatureC = static_cast<int>(raw + 0.5f) + PMIC_TO_PANEL_OFFSET_C;
    LOG_INF("EPD", "Panel temp: pmic=%dC offset=%dC used=%dC", static_cast<int>(raw + 0.5f),
            PMIC_TO_PANEL_OFFSET_C, cachedTemperatureC);
  }
  temperatureReadAt = now;
  return cachedTemperatureC;
}

// ED052TC4 with HORIZONTAL_MIRRORED cleared. epd_hl honours that flag (highlevel.c:58)
// while the low-level path ignored it, and GfxRenderer already applies orientation, so
// leaving it set would mirror the image twice.
constexpr EpdDisplay_t HZ52_PANEL = {
    .width = 1280,
    .height = 720,
    .bus_width = 8,
    .bus_speed = 22,
    // Matches stock. Static analysis of the vendor firmware shows both epdiy waveform
    // tables compiled in *byte-identically* to upstream (ED097TC2 GC16 LUT found at
    // 0x452844, ED047TC2 GC16 at 0x45737C), and its menu offers "16-level grey (ED047
    // waveform)" as an option over the default. So there is no bespoke ED052TC4 table to
    // find, and the default is what stock actually runs.
    //
    // ED047TC2's GL16 is 38-57 phases against ED097TC2's flat 30, so this is also the
    // faster of the two. Its single temperature band spans 20-30 C, which the offset
    // applied below keeps us inside.
    .default_waveform = &epdiy_ED097TC2,
    .display_type = DISPLAY_TYPE_GENERIC,
};

// Page turns use MODE_DU.
//
// MODE_GL16 was tried and rejected on hardware: despite carrying the reset stage described
// below it left ghosting *unchanged* while costing 1.4 s (30 phases vs DU's 5) and adding a
// visible shifting artefact on every refresh. That is a useful negative result -- if a full
// reset waveform does not clear the residue, the cause is not waveform timing, and the
// remaining suspects are drive voltage (VCOM/rails) or epdiy's belief about the glass
// diverging from reality.
//
// DU is a two-level mode whose waveform is a flat one-directional push (ED097TC2 DU is
// literally { 1000,1000,1000,1000,1000 }). It moves particles that are already free but
// cannot unstick lodged ones, and never reverses, so residue accumulates in the shape of
// whatever was previously inked -- the boxy shadows behind tategaki columns.
//
// GL16 carries a reset stage. Its 30 phases are exactly WHITE_TO_GL16's 15 followed by
// BLACK_TO_GL16's 15: drive every pixel in the region toward a known rail regardless of
// target, then drive from that known state to the target. That reset is applied to
// unchanged pixels too, which is what erases history rather than just the visible diff.
// GL16 is the non-flashing variant, so it does this without a visible inversion.
constexpr enum EpdDrawMode PAGE_MODE = MODE_DU;
constexpr enum EpdDrawMode FULL_MODE = MODE_GC16;

uint8_t* panelBuffer = nullptr;  // 1bpp surface GfxRenderer draws into
size_t panelBufferBytes = 0;
bool initialised = false;
bool panelIsWhite = false;

EpdiyHighlevelState hlState;

// 1bpp -> 4bpp expansion table: one input byte (8 pixels, MSB = leftmost) becomes four
// output bytes (2 pixels each, even x in the low nibble per epd_draw_pixel). 1 = white =
// 0xF. Flash-resident, so no DRAM cost, and it replaces the old bit-reverse table -- nibble
// order is now explicit here rather than implied by epdiy's 8ppB packing.
struct ExpandTable {
  uint32_t v[256];
};
constexpr ExpandTable makeExpandTable() {
  ExpandTable t{};
  for (int b = 0; b < 256; b++) {
    uint32_t out = 0;
    for (int px = 0; px < 8; px++) {
      const bool white = (b >> (7 - px)) & 1;
      if (!white) continue;
      const int byteIdx = px / 2;                  // which of the four output bytes
      const int shift = (px % 2) ? 4 : 0;          // odd x -> high nibble
      out |= static_cast<uint32_t>(0xF) << (byteIdx * 8 + shift);
    }
    t.v[b] = out;
  }
  return t;
}
constexpr ExpandTable EXPAND = makeExpandTable();

}  // namespace

namespace Hz52Display {

bool begin() {
  if (initialised) return true;

  epd_init(&epd_board_v7_raw, &HZ52_PANEL, EPD_OPTIONS_DEFAULT);
  epd_set_vcom(VCOM_MV);

  // ~1.84 MB of PSRAM: front + back (460,800 each at 4bpp) + difference (921,600, one byte
  // per pixel holding the packed from/to pair). Against ~7.4 MB free.
  hlState = epd_hl_init(EPD_BUILTIN_WAVEFORM);

  // Our own 1bpp surface stays: GfxRenderer is 1bpp throughout, so it draws here and push()
  // expands into epdiy's 4bpp framebuffer.
  panelBufferBytes = static_cast<size_t>(epd_width() / 8) * epd_height();
  panelBuffer = static_cast<uint8_t*>(heap_caps_malloc(panelBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!panelBuffer) {
    LOG_ERR("EPD", "OOM: %u bytes for 1bpp surface", static_cast<unsigned>(panelBufferBytes));
    return false;
  }
  memset(panelBuffer, 0xFF, panelBufferBytes);  // 0xFF = white, same polarity as EInkDisplay

  initialised = true;
  LOG_INF("EPD", "Panel ready: %dx%d, 1bpp surface %u B + 4bpp hl state, VCOM -%d.%02d V, psramFree=%u",
          epd_width(), epd_height(), static_cast<unsigned>(panelBufferBytes), VCOM_MV / 1000, (VCOM_MV % 1000) / 10,
          static_cast<unsigned>(ESP.getFreePsram()));
  return true;
}

uint8_t* frameBuffer() { return panelBuffer; }
size_t frameBufferSize() { return panelBufferBytes; }
int panelWidth() { return epd_width(); }
int panelHeight() { return epd_height(); }

void clear() {
  if (!initialised) return;
  // Mandatory on first boot after other firmware: epdiy has no idea what is physically on
  // the glass. epd_hl_set_all_white() only updates epdiy's *belief*, so the physical clear
  // has to happen too or the first differential update keeps the old image.
  const uint32_t start = millis();
  epd_poweron();
  epd_clear();
  epd_poweroff();
  epd_hl_set_all_white(&hlState);
  panelIsWhite = true;
  LOG_INF("EPD", "Full clear in %lums", static_cast<unsigned long>(millis() - start));
}

// One-shot PMIC dump. Waveform tables, board definition and panel config all match the
// stock firmware byte-for-byte, and even a full-reset GL16 waveform left ghosting unchanged
// -- which points away from drive *timing* and toward drive *voltage*. If the rails or VCOM
// are not what the panel expects, every transition under-drives regardless of waveform.
//
// Must run with the rails up: the TPS65185 does not ACK until WAKEUP is asserted.
void logPmicState() {
  i2c_master_dev_handle_t tps = epd_board_i2c_current_tps();
  if (tps == nullptr) {
    LOG_ERR("EPD", "PMIC handle unavailable");
    return;
  }

  const uint8_t revid = tps_read_register(tps, TPS_REG_REVID);
  const uint8_t enable = tps_read_register(tps, TPS_REG_ENABLE);
  const uint8_t vadj = tps_read_register(tps, TPS_REG_VADJ);
  const uint8_t vcom1 = tps_read_register(tps, TPS_REG_VCOM1);
  const uint8_t vcom2 = tps_read_register(tps, TPS_REG_VCOM2);
  const uint8_t pg = tps_read_register(tps, TPS_REG_PG);
  const uint8_t upseq0 = tps_read_register(tps, TPS_REG_UPSEQ0);
  const uint8_t dwnseq0 = tps_read_register(tps, TPS_REG_DWNSEQ0);

  // VCOM is a 9-bit DAC in 10 mV steps: VCOM1 is the low byte, VCOM2 bit0 the MSB.
  const unsigned vcomMv = (((vcom2 & 0x01) << 8) | vcom1) * 10;

  LOG_INF("EPD", "PMIC revid=0x%02X enable=0x%02X vadj=0x%02X pg=0x%02X upseq0=0x%02X dwnseq0=0x%02X", revid, enable,
          vadj, pg, upseq0, dwnseq0);
  LOG_INF("EPD", "PMIC vcom regs=0x%02X/0x%02X -> -%u.%02u V (we asked for -%d.%02d V)", vcom1, vcom2, vcomMv / 1000,
          (vcomMv % 1000) / 10, VCOM_MV / 1000, (VCOM_MV % 1000) / 10);
  // PG bit7 is the summary flag; the low nibble reports each rail individually.
  LOG_INF("EPD", "PMIC power-good: all=%d vb=%d vddh=%d vpos=%d vneg=%d", (pg >> 7) & 1, (pg >> 3) & 1, (pg >> 2) & 1,
          (pg >> 1) & 1, pg & 1);
}

bool push(bool deghost) {
  if (!initialised) return false;

  // Expand the 1bpp surface into epdiy's 4bpp framebuffer. epdiy diffs this against its own
  // back buffer and drives only what changed, so the two-pass differential this driver used
  // to run by hand -- and the prev/passBlack/passWhite buffers it needed -- are gone.
  uint8_t* fb = epd_hl_get_framebuffer(&hlState);
  auto* out = reinterpret_cast<uint32_t*>(fb);
  for (size_t i = 0; i < panelBufferBytes; i++) {
    out[i] = EXPAND.v[panelBuffer[i]];
  }

  epd_poweron();
  static bool pmicLogged = false;
  if (!pmicLogged) {
    pmicLogged = true;
    logPmicState();
  }
  const uint32_t start = millis();
  const enum EpdDrawError err = epd_hl_update_screen(&hlState, deghost ? FULL_MODE : PAGE_MODE, panelTemperature());
  const uint32_t elapsed = millis() - start;
  epd_poweroff();

  panelIsWhite = false;
  if (err != EPD_DRAW_SUCCESS) {
    LOG_ERR("EPD", "Draw failed: err=0x%x", static_cast<int>(err));
    return false;
  }
  LOG_INF("EPD", "Drew in %lums (%s, temp=%dC)", static_cast<unsigned long>(elapsed),
          deghost ? "GC16" : (PAGE_MODE == MODE_DU ? "DU" : "GL16"), panelTemperature());
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
