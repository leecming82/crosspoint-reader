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

// Two-cycle de-ghost for the reader's periodic HALF_REFRESH. Cheaper than clear(); still
// leaves the panel uniformly white, so the next push repaints from a known state.
void deghost();

// Push the framebuffer to the panel.
bool push();

// Logical portrait surface (720 x 1280). Applies the measured panel transpose.
int logicalWidth();
int logicalHeight();
void setLogicalPixel(int lx, int ly, bool black);
void fillLogicalRect(int lx, int ly, int w, int h, bool black);

}  // namespace Hz52Display

#endif
