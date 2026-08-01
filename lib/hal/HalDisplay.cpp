#include <HalDisplay.h>
#include <HalGPIO.h>

#ifdef CROSSPOINT_BOARD_HZ52
// HZ5.2 has no display controller: the panel is an 8-bit parallel bus driven by epdiy,
// with the SoC acting as the timing controller. EInkDisplay's SSD1677 command protocol has
// no meaning here, so every method routes to Hz52Display instead. The einkDisplay member
// is still constructed (its pins are all -1 on this board) but is never driven.
#include "../../src/hz52_display.h"

#include <cstring>
#endif

// Global HalDisplay instance
HalDisplay display;

#define SD_SPI_MISO 7

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

void HalDisplay::begin(bool seamless) {
#ifdef CROSSPOINT_BOARD_HZ52
  (void)seamless;
  Hz52Display::begin();
  // The panel physically retains whatever the previous firmware left on it, and epdiy has
  // no way to know what that is, so the first paint must be a real clear.
  Hz52Display::clear();
  return;
#else
  // Set X3-specific panel mode before initializing.
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }

  einkDisplay.begin();

  if (seamless) {
    // Defuse the SDK's X3 _x3InitialFullSyncsRemaining counter (no-op on X4)
    // so the first paint isn't promoted to FULL (~770ms). Skips the wakeup-
    // gated requestResync() below for the same reason.
    einkDisplay.skipInitialResync();
    return;
  }
  // Request resync after specific wakeup events to ensure clean display state.
  const auto wakeupReason = gpio.getWakeupReason();
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
      wakeupReason == HalGPIO::WakeupReason::Other) {
    einkDisplay.requestResync();
  }
#endif
}

void HalDisplay::clearScreen(uint8_t color) const {
#ifdef CROSSPOINT_BOARD_HZ52
  memset(Hz52Display::frameBuffer(), color, Hz52Display::frameBufferSize());
#else
  einkDisplay.clearScreen(color);
#endif
}

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
#ifdef CROSSPOINT_BOARD_HZ52
  // MODE_DU is a fast two-level waveform that deliberately skips the reset phases, so
  // residue builds up and old text stays faintly visible. The reader already schedules a
  // de-ghosting pass every SETTINGS.getRefreshFrequency() pages (via
  // ReaderUtils::displayWithRefreshCycle -> HALF_REFRESH); that becomes a GC16 paint here.
  //
  // The de-ghost is a mode on the paint, not a call before it. epd_hl updates the
  // difference between its front and back buffers and returns early when there is none, so
  // issuing it before the new frame was expanded into the front buffer found an empty diff
  // and silently did nothing -- which is why the interval refresh never appeared to run.
  (void)turnOffScreen;
  Hz52Display::push(mode == FULL_REFRESH || mode == HALF_REFRESH);
  return;
#endif
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
#ifdef CROSSPOINT_BOARD_HZ52
  (void)mode;
  (void)turnOffScreen;
  Hz52Display::clear();
  Hz52Display::push();
  return;
#endif
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::deepSleep() {
#ifdef CROSSPOINT_BOARD_HZ52
  // epdiy powers the panel down after every draw; nothing further to release here.
  return;
#else
  einkDisplay.deepSleep();
#endif
}

uint8_t* HalDisplay::getFrameBuffer() const {
#ifdef CROSSPOINT_BOARD_HZ52
  return Hz52Display::frameBuffer();
#else
  return einkDisplay.getFrameBuffer();
#endif
}

#ifdef CROSSPOINT_BOARD_HZ52
// The two-plane greyscale surface below exists to squeeze 2-bit AA out of a 48 KB
// framebuffer on a C3 with no PSRAM, and maps onto SSD1677's BW/RED RAM. HZ5.2 has
// neither: 16-level greyscale here means a 4bpp buffer through epdiy. Inert until that
// path exists; the board profile reports displayGrayscaleBits = 1 so nothing should call
// these.
void HalDisplay::copyGrayscaleBuffers(const uint8_t*, const uint8_t*) {}
void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t*) {}
void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t*) {}
void HalDisplay::cleanupGrayscaleBuffers(const uint8_t*) {}
// Deliberately does NOT push. The greyscale passes clear the shared framebuffer to 0x00
// (all black) and render plane data into it before calling this; pushing there paints a
// real near-black frame onto the panel, which is what produced a black after-image that
// persisted rather than a transient artefact. The BW frame is pushed by displayBuffer()
// after restoreBwBuffer(), so dropping this present loses nothing.
void HalDisplay::displayGrayBuffer(bool) {}
void HalDisplay::writeGrayscalePlaneStrip(bool, const uint8_t*, uint16_t, uint16_t) {}
bool HalDisplay::supportsStripGrayscale() const { return false; }
#else
void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) { einkDisplay.displayGrayBuffer(turnOffScreen); }

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  einkDisplay.writeGrayscalePlaneStrip(lsbPlane ? EInkDisplay::GRAY_PLANE_LSB : EInkDisplay::GRAY_PLANE_MSB, rows,
                                       yStart, numRows);
}

bool HalDisplay::supportsStripGrayscale() const { return einkDisplay.supportsStripGrayscale(); }
#endif

uint16_t HalDisplay::getDisplayWidth() const {
#ifdef CROSSPOINT_BOARD_HZ52
  return Hz52Display::panelWidth();
#else
  return einkDisplay.getDisplayWidth();
#endif
}

uint16_t HalDisplay::getDisplayHeight() const {
#ifdef CROSSPOINT_BOARD_HZ52
  return Hz52Display::panelHeight();
#else
  return einkDisplay.getDisplayHeight();
#endif
}

uint16_t HalDisplay::getDisplayWidthBytes() const {
#ifdef CROSSPOINT_BOARD_HZ52
  return Hz52Display::panelWidth() / 8;
#else
  return einkDisplay.getDisplayWidthBytes();
#endif
}

uint32_t HalDisplay::getBufferSize() const {
#ifdef CROSSPOINT_BOARD_HZ52
  return Hz52Display::frameBufferSize();
#else
  return einkDisplay.getBufferSize();
#endif
}
