#pragma once

// Minimal HZ5.2 panel driver used during bring-up (milestone 4). This will fold into
// HalDisplay behind the board profile once the render path is settled; keeping it
// separate for now avoids disturbing the SSD1677 path that X3/X4/Murphy depend on.
#ifdef CROSSPOINT_BOARD_HZ52

#include <cstddef>
#include <cstdint>

namespace Hz52Display {

bool begin();

// Panel-native 1bpp framebuffer: 1280x720, 160-byte stride, 0 = black, 1 = white.
// Same polarity and packing as EInkDisplay's, so GfxRenderer output can be handed over
// without conversion.
uint8_t* frameBuffer();
size_t frameBufferSize();
int panelWidth();
int panelHeight();

// Full flash clear (3 cycles). Required before the first push after other firmware owned
// the glass, and the heaviest de-ghost.
void clear();



// Push the framebuffer to the panel.
//
// deghost selects MODE_GC16 (a flashing full-reset update) instead of MODE_DU for this
// paint. It is a mode on the paint rather than a separate call because epd_hl updates the
// *difference* between its front and back buffers and returns early when there is none --
// a de-ghost issued before the new frame was expanded found an empty diff and did nothing.
bool push(bool deghost = false);

// Logical portrait surface (720 x 1280). Applies the measured panel transpose.
int logicalWidth();
int logicalHeight();
void setLogicalPixel(int lx, int ly, bool black);
void fillLogicalRect(int lx, int ly, int w, int h, bool black);

}  // namespace Hz52Display

#endif
