# HZ5.2 Japanese Port Plan

Date: 2026-07-27 (last worked 2026-08-02)

---

## RESUME HERE (2026-08-02)

**Done:** milestones 1–6. The device boots to the normal CrossPoint UI, renders through
`HalDisplay`/`GfxRenderer`, navigates with its three buttons, and reads a tategaki Japanese EPUB
end to end through the TTF reader. **Nothing is pushed** — the submodule branch `hz52-sd-cs`
(commit `3e50170`) must be pushed before the parent, or the parent references a commit nobody can
fetch.

**Ghosting is resolved and page turns no longer flash.** It took four independent fixes plus a
waveform merge; the full account is in [Panel refresh](#panel-refresh) under milestone 4. Measured
on device:

| | mode | cost |
| --- | --- | --- |
| page turn | GL16, merged halves, whole panel | ~0.40 s |
| interval refresh (`refreshFrequency`, default 15) | GC16, whole panel | ~0.74 s |
| boot / full clear | `epd_clear()` | ~1.62 s |

A little residue still accumulates between interval refreshes. That is expected rather than a
remaining bug: GL16 never drives `W→W` or `B→B`, so the background gets no periodic reset —
which is exactly what the GC16 interval pass is for, and why the setting still earns its place.

**NEXT TASK — EPUB images render in misplaced, overlapping bands.**

On a page whose content is a full-page scanned image, the image draws twice at different
origins: one copy roughly where it belongs, and a second offset left and down, overlapping it.
Text on the same page is correct. Photographed on device 2026-08-02, reading a Japanese EPUB
(the `供述調書` page, footer reads `表紙  1/1  2%`).

Prior art, and why this is a *follow-on* rather than a fresh bug: `57b3e7d6` fixed images
rendering "in misplaced horizontal bands" on this panel. The cause then was that
`DirectPixelWriter` (`lib/Epub/Epub/converters/DirectPixelWriter.h`) reimplements the
orientation transform independently of `GfxRenderer::rotateCoordinates`, precomputing a linear
form so the per-pixel loop avoids a call — and it still hardcoded X4's Portrait convention.
Both copies now carry `kPanelInvertsPortraitAxis`. **That there are two independent copies of
this transform is the standing hazard**; confirm nothing has grown a third before assuming the
fault is elsewhere.

Where to look, and one lead already narrowed:

- `DirectPixelWriter` is used by `ImageBlock.cpp`, `JpegToFramebufferConverter.cpp`,
  `PngToFramebufferConverter.cpp` and `ImageRotationUtils.h`. Only `ImageBlock` was exercised by
  the earlier fix.
- `GfxRenderer` has a **strip target** for raw writers that bypass `drawPixel`
  ([GfxRenderer.h:199](../lib/GfxRenderer/GfxRenderer.h#L199)) — writers subtract a physical-row
  origin and clip to a band. Misplaced bands is exactly its failure signature, but it looks
  inactive here: `EpubReaderActivity.cpp:1924` gates it on
  `renderTextAntiAliasing && renderer.supportsStripGrayscale()`, and HZ5.2 has
  `displayGrayscaleBits = 1` (so AA is off) and `supportsStripGrayscale()` returning false.
  Verify that on device rather than trusting the read.
- Two draws at different origins also fits the page being rendered more than once without an
  intervening clear. `ImageBlock::render` deliberately skips the font-prewarm scan pass for this
  reason ([ImageBlock.cpp:149](../lib/Epub/Epub/blocks/ImageBlock.cpp#L149)); check whether the
  BW pass runs twice.

Reproduce by opening that EPUB and paging to the image. Note the panel transpose means a
stride/origin error shows up rotated, so reason in *physical* coordinates when reading the code.

**Then, in rough order:**

1. **Sweep the waveform frame count**, if 0.40 s ever stops feeling fast enough. Frame count is
   the only thing that costs time on this hardware (~24.4 ms each), and
   `scripts/gen_hz52_waveform.py --gl-frames N` regenerates the table: 15 is today's 0.40 s,
   12 → ~0.32 s, 10 → ~0.27 s. Unlike the merge this is genuine downsampling, so it does trade
   quality — sweep down until transitions visibly stop completing.
2. **Page preparation, not the panel.** `prewarm` (TTF glyph rasterization) adds 157–1043 ms on
   top of the panel time, so a page turn totals ~0.6–1.4 s. It cannot be overlapped *with* the
   refresh — epdiy pins a max-priority feed task to both cores for the duration (see
   [Core affinity during refresh](#core-affinity-during-refresh)) — but it can be moved into the
   idle time between page turns, which is far more plentiful. The glyph sidecar is also only at
   30–36% of its cache limit, so raising that cuts misses outright.
3. **Resolve the PMIC power-good decode** — see the investigation section. Cheap, never done, and
   it either retires a suspect or reopens one.

**Also open (lower priority):** UI is legible but small at 283 ppi (milestone 7); many screens
still assume touch and are hard to use with three buttons (milestone 8); `default`/X4 env does not
build on this branch (`FontSelectionActivity`, `ReaderFontSizeActivity` need non-TTF fallbacks).

---

Status: **milestones 1–2 complete.** Hardware fully characterised (pin map, panel identity, VCOM, buttons — all recovered read-only over JTAG), and a board profile now builds and boots on the device in diagnostics-only mode. The display and storage paths are deliberately not initialised yet; that is milestones 3–4.

This document tracks porting this Japanese CrossPoint branch to a garage-built 5.2 inch ESP32-S3 e-reader, referred to as **HZ5.2**. It follows the structure of [esp32-device-migration-comparison.md](esp32-device-migration-comparison.md) (the Murphy M4 port plan), which is the closest precedent.

As with M4, the Japanese reader remains the source of truth: ruby/furigana, EPUB writing-mode metadata, vertical tategaki layout, dictionary cursor geometry, SD-backed dictionaries, the TTF reader font path, and bounded-memory caches must keep their current semantics. Murphy M4 is the compatibility baseline that must not regress; per [ttf-migration-plan.md](ttf-migration-plan.md), X3/X4 are already out of scope for this branch.

## The Device

ESP32-S3 with octal PSRAM, driving a **1280×720 5.2 inch ED052TC4** e-paper panel over an **8-bit parallel bus** using **epdiy** — not SPI. Three buttons, no touch, no frontlight. SD over SPI. A TPS65185-class PMIC provides the panel rails and software-settable VCOM.

| Item | Value |
| --- | --- |
| SoC | ESP32-S3, dual-core, TAP IdCode `0x120034e5` |
| PSRAM | **Octal, 8 MB** (`8388608` B, max single alloc `8257524` B) — confirmed on-device |
| Flash | 16 MB, DIO, 80 MHz |
| Panel | ED052TC4, `1280x720`, ~283 ppi |
| Display bus | 8-bit parallel via LCD_CAM, 22 MHz pixel clock |
| Driver stack | epdiy (`epd_lcd_init`, `epd_prep`, waveform LUT in SRAM) |
| Greyscale | 16-level (4bpp) via a borrowed ED047 waveform; 8-level from the panel default |
| VCOM | **−2.70 V**, software-settable over I²C |
| Storage | SD over SPI, FAT32 |
| Input | 3 buttons + hardware reset. No touch |
| Frontlight | None |
| RTC | None found — time via NTP |
| USB | Native USB-Serial-JTAG, `303a:1001`. **Hardware JTAG enabled, not eFuse-locked** |
| Unit MAC | `58:E6:C5:5A:45:54` |

### GPIO map

Recovered 2026-07-27 by JTAG register read against running stock firmware. Signal indices resolved from ESP-IDF's `soc/gpio_sig_map.h`.

| Subsystem | Signal | GPIO |
| --- | --- | --- |
| Panel data | `LCD_DATA_OUT8` … `OUT15` | `18`, `8`, `16`, `17`, `7`, `15`, `5`, `6` |
| Panel clock | `LCD_PCLK` | `4` |
| Panel control | `LCD_H_ENABLE` | `41` |
| Panel control | `LCD_H_SYNC` | `42` |
| Panel control | `RMT_SIG_OUT1` — CKV by strong inference | `48` |
| Panel control — `STV` | software GPIO, driven HIGH | `45` |
| **TPS65185 `WAKEUP`** | software GPIO | **`14`** |
| Panel / PMIC control (roles unresolved) | software GPIO, driven LOW at idle | `9`, `10`, `11`, `12` |
| **SD chip-select** | software GPIO | **`44`** |
| **SD power gate** (active HIGH) | software GPIO | **`46`** |
| I²C | `I2CEXT0_SCL` / `SDA` | `40` / `39` |
| SPI (SD) | `FSPICLK` / `FSPIQ` / `FSPID` | `3` / `2` / `43` |
| Button — isolated | input + pull-up, active-low | `38` |
| Button — upper of pair | input + pull-up, active-low | `0` (boot strap) |
| Button — lower of pair | input + pull-up, active-low | `21` |
| Unidentified digital input | input + pull-up, idle HIGH | `47` |
| Battery | analog signature (`FUN_IE=0`, no pulls) | `1` |
| Flash / octal PSRAM | — | `26`–`32`, `33`–`37` |
| USB D− / D+ | — | `19`, `20` |

**Physical layout:** one reset button on the **top** edge (hardware only — this is the vendor's `按顶部按钮重启`, "press the top button to restart"). Three buttons on the **right** edge, arranged **one, a gap, then two** → `38` isolated, `0` upper of pair, `21` lower of pair.

### Relationship to upstream epdiy boards

**HZ5.2 is an epdiy V7 / lilygo-S3 derivative.** Comparing against `src/board/epd_board_v7.c` and `src/board/lilygo_board_s3.c`, six signals match upstream exactly — `CKH=4`, `STH=41`, `LEH=42`, `CKV=48`, `STV=45`, and I²C on `40`/`39` — and the data-pin *set* `{5,6,7,8,15,16,17,18}` is identical. Start from those board files.

Two deliberate divergences account for everything else:

1. **No PCA9555 I/O expander.** Upstream V7 puts `OE`, `MODE`, `PWRUP`, `VCOM_CTRL`, `WAKEUP`, `PWRGOOD` and `INT` behind an I²C expander. HZ5.2 wires them **directly to GPIOs** — which is exactly the software-driven pads `9`,`10`,`11`,`12`,`14`,`44`,`46` we could not otherwise account for. Any port must drive these as plain GPIOs, not through `pca9555.c`.
2. **Upstream's upper data byte is repurposed.** Because HZ5.2 is 8-bit, V7's `D8..D15` = `{9,10,11,12,13,14,21,47}` is free. The vendor reused it — for the control lines above, for `GPIO21` (a button), and `GPIO47`. Likewise `GPIO38`, upstream's I²C interrupt, is HZ5.2's isolated button.

Note the **data bit ordering differs** from upstream's sequential `data[0]=D0…` assignment: our measured signal→pad mapping is the ground truth and the bus config must be written to match it, not copied from lilygo.

Four consequences that shape the port:

1. **Data lines sit on bus bits 8–15, not 0–7.** epdiy drives the upper byte of the S3's 16-bit LCD bus. Any bus configuration we write must match or the panel receives the wrong byte.
2. **SD is SPI-attached, not SD_MMC.** No `SDHOST_*` signals appear anywhere; there is a clean `FSPI` bus. The vendor's `vfs_fat_sdmmc` strings don't contradict this — ESP-IDF's SD-over-SPI path uses the same structures. Board profile needs `sdUsesSdMmc = false`. The SD chip-select is `44` or `46` — the pads driven HIGH once `45` is accounted for as `STV`, since an idle active-low CS reads HIGH.
3. **`GPIO0` is both the boot strap and the upper button of the pair** — a button users will press constantly if the pair carries page navigation. Holding it during reset enters download mode. Recoverable and even useful, but the mapping must never make an ordinary press destructive. M4 hit the same constraint.
4. **Deep sleep removes the device from USB entirely.** Observed directly — the unit vanished from `ioreg` mid-session and returned on button wake with `Reset cause (5) — Deep-sleep core reset`. Before any probing session set `系统设置` → `休眠时间` to `180分钟后休眠` or `不休眠 极耗电`.

### VCOM

VCOM is the panel's **common electrode voltage**. Each pixel's microcapsule sits between two electrodes: the per-pixel one driven by the TFT source driver, and a single continuous transparent electrode covering the whole panel — that is VCOM. The pigment particles move according to the field *between* them, so effective drive on any pixel is (pixel voltage − VCOM).

It is a per-panel analog property, fixed by manufacturing variation in cell gap and pigment, and printed on the panel's flex cable. At the correct value white is whitest, black is blackest, and drive is symmetric. Wrong value gives washed-out contrast and uneven greys immediately — and worse, leaves a residual **DC component**, so accumulated charge causes permanent image retention and shortens panel life over months. It is the one display setting where a bad default degrades hardware rather than merely looking wrong.

On SPI e-paper modules VCOM is fixed on-module and we have never had to touch it. On a bare panel driven through a TPS65185-class PMIC it is a programmable DAC over I²C that **firmware must write during display init, before the first refresh**.

**This unit: `VCOM = −2.70 V`.** The stock UI shows it under `系统设置` → `VCOM 电压设置` as `2.70` — magnitude only; the rail is negative. Use `displayVcomDefaultMv = -2700` as the HZ5.2 profile default. That magnitude sits above the typical −1 V…−2.5 V band, which argues it is a genuine per-panel calibration rather than a vendor default.

**Do not change it in the stock UI.** That screen is writable and the stored value has no other copy on the device.

## How HZ5.2 Compares

| Axis | X4 (original baseline) | Murphy M4 (port complete) | HZ5.2 |
| --- | --- | --- | --- |
| SoC | ESP32-C3 | ESP32-S3 + 8 MB PSRAM | ESP32-S3 + 8 MB octal PSRAM |
| Panel | `800x480` 4.26" | `800x480` 4.26" | **`1280x720` 5.2"** |
| Pixel density | ~219 ppi | ~219 ppi | **~283 ppi** |
| Display bus | SPI, SSD1677 | SPI, SSD1677 | **8-bit parallel, LCD_CAM** |
| Driver | in-tree `EInkDisplay` | in-tree `EInkDisplay` | **epdiy** |
| Greyscale | 1bpp + 2-bit planes | 1bpp + 2-bit planes | **4bpp native (16 level)** |
| Framebuffer | 48,000 B static DRAM | 48,000 B static DRAM | **~900 KB PSRAM** |
| VCOM | fixed | fixed | **software, −2.70 V** |
| Storage | SdFat / SPI | SD_MMC 4-bit | **SD over SPI** |
| Touch | none | FT6336U | none |
| Frontlight | none | 2-channel warm/cool | none |
| Buttons | 6 + power | 3 | **3** |

HZ5.2 is not an incremental variant. M4 was a favourable port because it shared X4's display family and could reuse the SSD1677 driver wholesale. HZ5.2 shares none of that.

## What This Means for CrossPoint

### In our favour

1. **A board capability profile exists.** [BoardProfile.h:5](../lib/hal/BoardProfile.h#L5) has `enum class BoardModel { X4, X3, MurphyM4 }` and a 40-field `BoardCapabilityProfile` already covering greyscale bits, button count, touch, frontlight, PSRAM. Adding `HZ52` is mechanical.
2. **This branch is already S3 + PSRAM only.** X3/X4 are out of scope, so we no longer need every change to also fit a C3's 380 KB.
3. **Cache keys already include the viewport.** [Section.cpp:24](../lib/Epub/Epub/Section.cpp#L24) is at version 46 and the header at [Section.cpp:268-292](../lib/Epub/Epub/Section.cpp#L268-L292) includes `viewportWidth`/`viewportHeight`. A `1280x720` viewport invalidates and regenerates automatically.
4. **Runtime geometry is already plumbed.** `GfxRenderer` holds panel dimensions as members ([GfxRenderer.h:69-72](../lib/GfxRenderer/GfxRenderer.h#L69-L72)); the X3-vs-X4 split already forced this.
5. **The M4 touch conversion left button fallbacks intact**, so HZ5.2 inherits working button paths for all 47 activities.
6. **PSRAM allocation patterns exist** in [TtfReaderMetrics.cpp](../src/TtfReaderMetrics.cpp).

### Against us

1. **`EInkDisplay` is a 1839-line SSD1677 SPI driver with compile-time geometry.** [EInkDisplay.h:27-35](../open-x4-sdk/libs/display/EInkDisplay/include/EInkDisplay.h#L27-L35) hardcodes `DISPLAY_WIDTH = 800` and `MAX_BUFFER_SIZE = 52272`, and [EInkDisplay.h:130](../open-x4-sdk/libs/display/EInkDisplay/include/EInkDisplay.h#L130) is a **statically allocated DRAM array**. `1280x720` at 1bpp is 115,200 bytes. Essentially none of that driver transfers — it is command opcodes, controller LUT upload, and BUSY polling, all functions of hardware HZ5.2 does not have.
2. **Display pins are `#define`s selected by board macro** ([HalGPIO.h:9-24](../lib/hal/HalGPIO.h#L9-L24)). An 8-bit parallel bus plus control and PMIC I²C has no meaningful mapping onto a six-pin SPI shape.
3. **The greyscale model is structurally different.** Ours is a 1bpp framebuffer plus a two-pass 2-bit side channel ([GfxRenderer.h:51](../lib/GfxRenderer/GfxRenderer.h#L51)) mapping onto SSD1677's BW/RED RAM. epdiy's model is a single 4bpp framebuffer through `epd_hl_update_screen()`. `HalDisplay`'s whole greyscale surface ([HalDisplay.h:50-61](../lib/hal/HalDisplay.h#L50-L61)) plus the tiled-strip machinery exists to avoid a second buffer on a 48 KB-framebuffer C3 — a problem we no longer have.
4. **229 `CROSSPOINT_BOARD_MURPHY_M4` sites across ~55 files.** A third board turns many two-way `#ifdef`s into three-way decisions. Migrating the UX-relevant ones to capability queries is worth doing *as part of* this port.
5. **Six logical buttons, three physical.** [MappedInputManager.h:8](../src/MappedInputManager.h#L8) defines `Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward` with 211 call sites across `src/` — `Back` 66, `Confirm` 57, `Right` 20, `Left` 19, `Up` 17, `Down` 16.
6. **UI fonts are fixed-pixel bitmaps** — see [Area 3](#area-3-ui-density-at-283-ppi).

## The Three High-Touch Areas

### Area 1: Display HAL

The largest piece, and genuinely new code rather than a port.

**Wrap epdiy rather than writing our own i80 driver.** Upstream ships the `ED052TC4` definition in `src/displays.c` matching this panel on every timing parameter (`1280x720`, bus width 8, bus speed 22), plus the waveform tables, the S3 output path (`src/output_lcd/`), board configs, LUT modes, and PMIC/VCOM control. Writing an i80 e-paper driver from scratch means owning rail sequencing and waveform generation on hardware that can be damaged by getting it wrong.

**One upstream field must be overridden.** Per `src/epd_display.h`, `EpdDisplayType` is a bit-flag enum: `GENERIC = 0x00`, `HORIZONTAL_MIRRORED = 1<<0`, `ED097TC2 = 1<<1`, `UPSEQ_MC2 = 1<<2`. Upstream's `ED052TC4` uses `HORIZONTAL_MIRRORED` (`1`); both vendor firmware builds ship **`2`** (`ED097TC2`). Because these are independent bits, that is two changes — mirroring off, ED097TC2 sequencing on — suggesting a panel revision or wiring difference from whatever upstream was developed against. Build against upstream verbatim and you may get a mirrored image while everything else looks correct. Resolve empirically in milestone 4 with a **deliberately asymmetric test pattern** across `0`, `1`, `2`, `1|2`.

**Waveforms.** Upstream's `ED052TC4` defaults to `epdiy_ED097TC2`; `epdiy_ED047TC1`/`ED047TC2` also exist upstream. The vendor's 16-level mode is a runtime swap onto a borrowed ED047 table — **reproducible from stock upstream waveform data**, no vendor-extracted tables needed. Note both vendor builds carry a single temperature interval (20–30 °C), so temperature compensation outside that band appears to be absent by design; worth confirming, since the board has a temperature sensor.

**An epdiy update occupies the CPU for its full duration**, not just the pixel upload. With SSD1677 we shift planes in (~11 ms each) then idle on BUSY while the controller drives the panel. With no controller, we generate every waveform frame ourselves, feeding DMA that must not starve (`line buffer underrun occurred!`). epdiy expects a core largely to itself. Our task structure — the render task, [RenderLock](../src/activities/RenderLock.h), per-activity FreeRTOS tasks — was shaped on a single-core C3 where BUSY time was free. Core affinity needs deciding during milestone 4.

Framebuffer budget:

| Buffer | Bytes |
| --- | --- |
| X4 today (1bpp `800x480`) | `48,000` static DRAM |
| HZ5.2 1bpp `1280x720` | `115,200` |
| HZ5.2 4bpp front | `460,800` |
| HZ5.2 4bpp front + back | **`921,600`** |
| Waveform LUT | TBC — must be internal SRAM |

epdiy's high-level API keeps **front** (what you draw) and **back** (what it believes is on the glass) and diffs them per pixel. The back buffer is not redundancy — it replaces the controller RAM a bare panel doesn't have. PSRAM is therefore mandatory, and `psramCacheBudgetBytes` must be computed *after* the display's ~900 KB. PSRAM is also not retained across deep sleep, which is why the vendor compresses its framebuffer to `/fs/front_buffer.z`; our `HalDisplay::begin(bool seamless)` contract ([HalDisplay.h:26](../lib/hal/HalDisplay.h#L26)) survives but its implementation does not.

Render model, in increasing cost:

- **(a) 1bpp source of truth, expand on push.** Keep `GfxRenderer` as-is, expand 1bpp → 4bpp into epdiy's framebuffer on `displayBuffer()`. Cheapest path to "it renders". No AA gain. Good milestone 4 target.
- **(b) Native 4bpp rendering.** Unlocks 16-level text AA and real greyscale images, and lets us delete the LSB/MSB plane code, the chunked `storeBwBuffer` dance, and the tiled-strip path. Large diff; must not regress ruby/vertical/dictionary geometry. Our `Color` enum ([GfxRenderer.h:45-47](../lib/GfxRenderer/GfxRenderer.h#L45-L47)) is *already* documented as 16 levels, currently realised through Bayer dithering — 4bpp is the fidelity it was written for.
- **(c) Hybrid.** 1bpp for UI screens, 4bpp for reader pages and images.

Ship (a) to get a booting device, then move the reader path to (b), arriving at (c) by construction. Do not attempt (b) before the panel is proven.

### Area 2: Three-button UX

211 call sites across 47 activities assume six directional buttons. Three physical buttons is an interaction-model change, not a remap.

`3 buttons × {short, long}` is numerically six events, but long-press is not a peer of short-press — it is slower, less discoverable, and already spoken for. A more honest budget:

| Event | Proposed action |
| --- | --- |
| `GPIO0` short (upper of pair) | `Up` / `PageBack` |
| `GPIO21` short (lower of pair) | `Down` / `PageForward` |
| `GPIO38` short (isolated) | `Confirm` |
| `GPIO38` long | `Back` |
| `GPIO0` / `GPIO21` long | context: jump-back / jump-forward |
| `GPIO0`+`GPIO21` chord | candidate for menu / global overlay |

The physical **1 + gap + 2** grouping supports this: the isolated button reads as the odd-one-out (selection), the adjacent pair as fast repeatable navigation.

**The stock firmware's own answer is worth copying.** It does not fake `Left`/`Right` — it replaces them with explicit jump menus (`向前跳页` / `向后跳页` / `从头开始`, jump 1/5/10/30/50 pages, `跳到开头` / `跳到末尾`). Our `Left`/`Right` uses are mostly value-stepping in settings and chapter/percent navigation, exactly where a jump submenu substitutes well.

**BLE HID page-turner remotes** are the pressure valve. Stock scans for `KEY`-suffixed devices, bonds, stores in NVS, and auto-reconnects on wake. On a 3-button device an optional remote is not a gimmick.

Work items:

- Add an `inputButtonCount == 3` capability path and per-board button policy rather than a third `#ifdef` arm.
- Audit all 39 `Left`/`Right` sites: mechanically replaceable by long-press, better served by a jump menu, or genuinely needs a fourth direction.
- [ButtonRemapActivity](../src/activities/settings/ButtonRemapActivity.cpp) assumes 4 remappable front buttons — hide it, don't adapt it.
- `Labels mapLabels(back, confirm, previous, next)` ([MappedInputManager.h:34](../src/MappedInputManager.h#L34)) returns a 4-slot struct for on-screen hints; three buttons needs a different layout that also conveys long-press.
- [KeyboardEntryActivity](../src/activities/util/KeyboardEntryActivity.cpp) was already "clunky" on M4 *with touch*. With three buttons and no touch it is the worst screen on the device. Wi-Fi password entry is the blocking case — consider the vendor's approach, a SoftAP provisioning page, which we already have infrastructure for in [CrossPointWebServerActivity](../src/activities/network/CrossPointWebServerActivity.cpp).

### Area 3: UI density at 283 ppi

Every UI font in this codebase is a fixed-pixel bitmap. [fontIds.h](../src/fontIds.h) defines `UI_10_FONT_ID`, `UI_12_FONT_ID`, `SMALL_FONT_ID`; CJK UI fallbacks are baked at three pixel sizes ([cjk_ui_font_17.h](../lib/GfxRenderer/cjk_ui_font_17.h), `_21`, `_29`) with a build-time gate in `scripts/check_cjk_ui_font.py`.

At 283 ppi versus 219, the same bitmap glyph is **1.29× physically smaller** — a 10 px label renders at ~78% of its intended physical height. Simultaneously the viewport grows from `800x480` to `1280x720`, 2.4× the pixels, so every hardcoded offset, row height, items-per-page calculation, status bar height, and margin across 47 activities is both too small physically and laid out for a smaller canvas.

The vendor's font ladder starting at **20** px and running to 60 is the tell — that is roughly their minimum readable size at this density.

Fix: either new CJK UI bitmap sizes generated for ~283 ppi (and a fourth entry in the `check_cjk_ui_font.py` contract), or — more in keeping with [ttf-migration-plan.md](ttf-migration-plan.md) — a density-aware UI font path sized from a board `ppi` capability field rather than a hardcoded constant. The latter is more work but stops the next device reopening the same wound.

## Milestones

**1. Hardware characterisation — done (2026-07-27).**

Recovered the full pin map, panel identity, VCOM calibration, and button layout **read-only over JTAG**, without writing anything to the device. See [The Device](#the-device) and [Appendix: GPIO Recovery Method](#appendix-gpio-recovery-method).

Notable: static extraction of the pin map from the vendor binaries does **not** work — epdiy's board pins are compile-time `#define`s that become instruction immediates, not `rodata` arrays. Pad assignment exists only as live register state. The full 16 MB flash backup was attempted and abandoned (stub-loader stream corruption at 3.4%); judged unnecessary, since the vendor's merged image is a flashable rollback for bootloader + partition table + app, and VCOM — the only irreplaceable value — was read directly from the stock UI.

Remaining minor items: identify `GPIO47`; resolve the individual roles of the eight software-GPIO control lines; scan the I²C bus.

**2. Board profile — done (2026-07-27). Flashed and verified on hardware.**

Added `BoardModel::HZ52` to [BoardProfile.h](../lib/hal/BoardProfile.h), an `HZ52_PROFILE` descriptor to [BoardProfile.cpp](../lib/hal/BoardProfile.cpp), `deviceIsHz52()` to [HalGPIO.h](../lib/hal/HalGPIO.h), board forcing in `detectDeviceTypeWithFingerprint()` ([HalGPIO.cpp](../lib/hal/HalGPIO.cpp)) and `activeBoardProfile()` ([HalStorage.cpp](../lib/hal/HalStorage.cpp)), plus `partitions_hz52.csv` (dual-OTA, same proven layout as Murphy) and `env:hz52` in [platformio.ini](../platformio.ini).

Profile starts deliberately conservative: `psramCacheBudgetBytes` 0, `displayGrayscaleBits` 1, `displayPartialRefresh` false, touch/frontlight/RTC/battery-gauge/charger false, `inputButtonCount` 3, `sdUsesSdMmc` false. Promote only as evidence lands.

Board forcing is important for safety on this board specifically: the X3 fingerprint probe drives C3 pins that are the *parallel display bus* on HZ5.2, so `detectDeviceTypeWithFingerprint()` must short-circuit before it can run.

**Guard refactor.** The TTF reader symbols (`TTF_READER_METRICS`, `TtfFontCandidate`, the `buildTtf*Setting()` builders) were gated on `CROSSPOINT_BOARD_MURPHY_M4`, which made them unavailable to any other PSRAM board. Converted the *declaration* guards in `TtfReaderMetrics.{h,cpp}`, `TtfFontScanner.{h,cpp}`, `ReaderFontProvider.cpp` and `SettingsList.h` to `CROSSPOINT_TTF_READER_DIRECT_FREETYPE` — a feature both S3 environments already define. Semantically a no-op for Murphy. Activity-level UX guards (touch targets, layout, hints) were deliberately **left** on the board macro, since HZ5.2 must not inherit Murphy's touch behaviour.

Both `hz52` and `murphy_m4` build. A temporary bring-up guard kept `setup()` in diagnostics-only mode until the display and SD pins landed, because the build otherwise fell through to the X4 pin block and would have driven panel control lines as SPI — lifted in milestone 5.

Rollback path if ever needed: flash `test/firmware-5inch-01-droid-sans-fallback.bin` at `0x0`, a merged bootloader + partition table + app image. It will not restore the vendor's NVS, though VCOM is recorded here.

**Pre-existing, not caused by this work:** `pio run -e default` fails, and did before these changes. [SettingsList.h](../src/SettingsList.h) calls `buildTtfFontFamilySetting()`/`buildTtfFontSizeSetting()`/`buildTtfFontWeightSetting()` unconditionally while declaring them only under a guard no C3 environment defines. Consistent with [ttf-migration-plan.md](ttf-migration-plan.md) scoping X3/X4 out of this branch. Left alone rather than silently repaired.

**3. Boot, diagnostics, storage — done (2026-07-27). Verified on hardware.**

SD mounts in ~840 ms, root listing works, and a create / read-back / delete round-trip passes.

| Function | GPIO |
| --- | --- |
| Peripheral power enable (active HIGH) | **46** |
| SD chip-select | **44** |
| SCLK / MISO / MOSI (FSPI) | 3 / 2 / 43 |

**The blocker was never SPI — `GPIO46` gates the card.** It is held HIGH persistently by stock firmware (in
both idle and SD-active JTAG snapshots, unlike the 9..14 block which toggles with activity). With it low the
card does not answer on *any* chip-select, so every probe failed identically and looked like a bus fault.

What led there was an I²C scan returning **zero devices** on a bus already confirmed to exist and be pulled up
— two subsystems dark at once suggested shared power rather than SPI. **That reasoning is only partly borne
out:** asserting `GPIO46` fixes SD but I²C still enumerates nothing, so it is not simply a rail feeding the
whole peripheral section.

`GPIO46`'s physical nature is therefore **unproven**: load-switch enable, socket power, level-shifter/buffer
enable, or a reset are all consistent with the evidence. A power gate is plausible by family precedent —
Murphy M4 has `sdEnablePin = 10` active-low, which is why `BoardCapabilityProfile` already carries
`sdEnablePin`/`sdEnableActiveLow`. To settle it: measure the card's VDD, or toggle the pad and check whether
the card requires a full re-initialisation afterwards (true of a power gate, not of a buffer enable). Worth
populating the existing profile fields rather than keeping the pin hardcoded, once the polarity/semantics are
confirmed.

**Bus width: 1-lane SPI, as on X3/X4 — not Murphy's 4-bit SD_MMC.** Stock routes `FSPICLK`/`FSPIQ`/`FSPID` and
**no `SDHOST_*` signal appears anywhere**, where Murphy shows `SDHOST_CCLK_OUT`, `SDHOST_CCMD` and
`SDHOST_CDATA_20..23`. SPI mode uses only CS/SCLK/DI/DO, so DAT1–DAT2 are unused and there is no evidence they
are wired to GPIOs at all. Expect roughly 5 MB/s at 40 MHz versus ~20 MB/s for M4, which shows up mainly in
EPUB cache generation rather than page turns.

Implementation: the **existing SdFat path**, unchanged in shape. HZ5.2 reaches `SDCardManager` like X3/X4 and
simply passes its own chip-select — `SDCardManager::begin()` is parameterised as `begin(uint8_t csPin = 12)`
in the submodule, so every existing caller is unaffected. The peripheral rail is asserted in
`HalGPIO::begin()`, where a board-level power enable belongs. `murphy_m4` builds unchanged.

An IDF-native backend (`spi_bus_initialize` + `esp_vfs_fat_sdspi_mount`, VFS wrapped in an `fs::FS` subclass)
was written first, on the mistaken belief that Arduino's `SPIClass` could not route MOSI. Once that turned out
to be a misread register address it was reverted, and the SdFat path measured **better on every axis**: SD
ready at 727 ms vs 837 ms, 64 KB less flash, 3.7 KB more free heap, and SdFat's 40 MHz clock instead of the
conservative 400 kHz the IDF path was left at. Total HZ5.2 storage cost is now ~15 lines in `HalStorage.cpp`
plus the submodule default-argument.

Two earlier traps, both fixed and worth knowing for any third S3 board:

1. **`SPIClass::begin()` silently ignores its pin arguments if the bus is already started** — it returns `true`
   early. (Moot for HZ5.2 now, which uses IDF directly and never touches the Arduino SPI object; Arduino's
   `SPI` *is* `FSPI` == `SPI2_HOST`, so leaving it alone avoids two drivers owning one peripheral.)
2. **`InputManager` must be bypassed**, exactly as on Murphy. It assumes `POWER_BUTTON_PIN = 3` and an ADC
   ladder on `GPIO1`/`GPIO2`; here those are SD SCLK, the battery divider and SD MISO. Running after
   `SPI.begin()` it re-routed `GPIO3` away from `FSPICLK`, leaving the clock dead so the first transfer spun
   forever in `spiTransferByteNL()`. Diagnosed by halting over JTAG and resolving the PC with `addr2line`.

**Corrections to earlier entries in this document**, recorded because both cost real time:

- A claim that MOSI was unrouted was an **address error**: `GPIO_FUNCn_OUT_SEL` is `0x60004554 + 4n`, so pad 43
  is `0x60004600`, but `0x6000460c` (pad **46**) was read. MOSI was correct throughout.
- `GPIO9` was identified as chip-select from a watchpoint trace showing bit 9 toggling. That was **wrong** —
  chip-select is `GPIO44`. Panel refreshes were active during that capture, so the toggling bit was display
  control. The watchpoint technique itself was sound; the interpretation was not.

Methods that did **not** work, so they are not repeated: sampling GPIO levels (chip-select pulses far faster
than JTAG halt sampling); scanning for `FSPICS0` routing (stock drives CS in software, so no pad is ever routed
to signal 110); a static search for a `spi_bus_config_t` fingerprint `{43,2,3,-1,-1}` (built on the stack from
immediates, not in `.rodata`); and watchpointing at boot (stock defers SD init).

**I²C resolved: `GPIO14` is the TPS65185 `WAKEUP` line.** Sweeping the control block one pad at a time and
scanning after each gave a clean single positive — nothing responds with `9`, `10`, `11` or `12` asserted, and
asserting `GPIO14` alone brings up exactly one device at **`0x68`**, the TPS65185's documented address. The
part will not ACK on I²C until `WAKEUP` is high. This unblocks writing VCOM before the first refresh, which was
a hard prerequisite for milestone 4.

**`GPIO46` confirmed a true power gate, not a buffer enable.** Toggling it low then high while the card was
mounted and re-reading the same file: 14058 bytes before, **0 bytes after** — the card lost state entirely, so
it is real power switching. Now modelled with the existing `sdEnablePin = 46` / `sdEnableActiveLow = false`
profile fields rather than a hardcoded constant, and asserted from the profile in `HalGPIO::begin()`.

Two consequences: SD can be powered down in deep sleep for a genuine saving, **provided the card is
re-initialised on wake** — a resumed mount will not survive the gate cycling. And `GPIO9`/`10`/`11`/`12` remain
unidentified, but are now known *not* to gate either SD or I²C, which narrows them to panel control
(`PWRUP`, `VCOM_CTRL`, `OE`, `MODE`).

**4. Display bring-up (epdiy) — done (2026-08-02). Renders through `HalDisplay`/`GfxRenderer`.**

The panel powers, clears and draws, and page turns are clean and flash-free. `GfxRenderer` stays 1bpp;
`Hz52Display::push()` expands that surface into epdiy's 4bpp framebuffer, which is what allows a real
frame-indexed waveform to run (see [Panel refresh](#panel-refresh)). Page turns cost ~0.40 s, the
interval refresh ~0.74 s.

`HalDisplay` now routes every method to `Hz52Display` under `CROSSPOINT_BOARD_HZ52`, so `GfxRenderer` draws
into the framebuffer `HalDisplay` hands it and activities need no board-specific code. Text and shapes render
correctly in logical portrait. Two things were needed beyond the driver itself:

- **Orientation.** The panel is landscape-native (`1280x720`) and the transform is a pure transpose
  (`phyX = logical y`, `phyY = logical x`), measured with an asymmetric origin pattern rather than assumed.
  `GfxRenderer` gained `kPanelInvertsPortraitAxis` (false for HZ5.2) parameterising the Portrait and
  PortraitInverted mappings. `display_type` proved inert on the LCD path.
- **Bit order.** See [The 8PPB bit-order trap](#the-8ppb-bit-order-trap).

**No custom board definition was needed.** HZ5.2 is an epdiy V7 derivative with the PCA9555 expander
removed, which upstream already ships as `epd_board_v7_raw` ("a small v7 board without IO expander targeted
only to 8-bit einks"). Every pin matches what we measured: data `5,6,7,15,16,17,18,8`; `CKH=4` `STH=41`
`LEH=42` `STV=45` `CKV=48`; `OE=9` `MODE=10` `PWRUP=11` `VCOM_CTRL=12` `WAKEUP=14` `PWRGOOD=47` `INT=13`;
I²C `40`/`39`. This independently confirms the last unknown pads — and our own findings corroborate it, since
GPIO14 alone gating the PMIC is exactly `WAKEUP`, and GPIO47 as input-with-pull-up is exactly open-drain
`PWRGOOD`.

**`display_type` turns out to be irrelevant on our code path — use upstream's `ED052TC4` unmodified.** The
field is consulted in only three places: `highlevel.c:58` (`is_mirrored = display_type & HORIZONTAL_MIRRORED`),
the **I2S** render path, and `DISPLAY_UPSEQ_MC2` handling in some board files. We drive `epd_draw_base()` on
the **LCD** path, which reads none of them, so overriding the field to the vendor's `2` changed nothing when
tried. The long-standing "mirroring risk" flagged earlier in this document is therefore **not** a
`display_type` question at all; orientation is entirely ours to apply.

**Measured panel mapping** (origin-"L" pattern with arms of 1280 px and 720 px, plus a direction notch — chosen
after an "F" glyph proved impossible to read reliably from photographs):

```text
framebuffer x  ->  physical vertical,   increasing downward
framebuffer y  ->  physical horizontal, increasing rightward
framebuffer (0,0) -> physical top-left
```

That is a **transpose**: `physical_x = fb_y`, `physical_y = fb_x`. A transpose is a reflection, not a rotation
— a true 90° rotation would carry a sign flip on one axis and none is present. Logical portrait content
(720 × 1280) is therefore written with the axes swapped.

**Confirmed on hardware.** A portrait "page" of seven left-aligned bars with deliberately ragged right-hand
lengths (`88, 72, 90, 55, 84, 68, 40` %) renders with every bar in the right order and a footer marker
bottom-left — i.e. it reads as text lines running down a portrait screen. The mapping is applied at *draw*
time, not by transposing a buffer afterwards: a post-hoc transpose would cost ~920,000 bit operations per
refresh, and `GfxRenderer` already applies orientation transforms at draw time on X3/X4, so this folds into
the existing mechanism rather than adding one.

**Memory: `epd_hl_*` costs 1.84 MB of PSRAM** for its front/back/difference set, against 115,200 bytes for
a 1bpp `epd_draw_base()` buffer. The low-level path was chosen first for that 16× saving and later abandoned
anyway: 1bpp has nowhere to record a pixel's current state, so no real waveform can run. Against ~7.4 MB free
the memory was never the binding constraint. See [Panel refresh](#panel-refresh).

Both early conclusions in this section were later overturned — the 8PPB packing (wrong bit order, and moot
once the driver went 4bpp) and the pixel clock ("largely moot in practice", which it was not: raising it
halved every refresh). Timings here are pre-fix and superseded by the table in
[RESUME HERE](#resume-here-2026-08-02). Ambient reads 31 °C from the PMIC.

**Four epdiy API traps**, each of which cost a flash cycle and none obvious from the docs:

1. **`EPD_BUILTIN_WAVEFORM` is `#define NULL`.** The high-level API substitutes the display's default;
   `epd_draw_base()` does not — `render.c:104` returns `EPD_DRAW_NO_PHASES_AVAILABLE` on NULL. Pass
   `epd_get_display()->default_waveform`.
2. **`MODE_PACKING_8PPB` requires `PREVIOUSLY_WHITE` or `PREVIOUSLY_BLACK`.** A 1bpp buffer carries no "from"
   state, so `find_lut_functions()` (`lut.c:478`) finds no LUT and returns `EPD_DRAW_LOOKUP_NOT_IMPLEMENTED`.
3. **Read temperature only after `epd_poweron()`.** `epd_ambient_temperature()` reads a TPS65185 register and
   the PMIC does not ACK until `WAKEUP` is asserted; reading first trips an `ESP_ERROR_CHECK` in
   `tps65185.c` and aborts the firmware.
4. **Never touch I²C with Arduino `Wire`.** epdiy owns that bus via IDF; claiming it first makes
   `epd_board_init()` fail its i2c assert and abort.

Also: `epd_clear()` is mandatory on first boot after other firmware. `epd_hl_set_all_white()` only sets
epdiy's *belief* about the glass, so a differential update leaves the previous firmware's image in place —
which is exactly what happened on the first successful refresh.

**Driver shape.** `src/hz52_display.{h,cpp}` now exposes a `HalDisplay`-shaped surface — `begin()`,
`frameBuffer()`, `clear()`, `push()` — over one 115,200-byte PSRAM buffer allocated once for the life of the
device, never per refresh. It tracks whether the panel is uniform and self-clears when `PREVIOUSLY_WHITE`
would otherwise be invalid, since that flag is a correctness requirement rather than an optimisation.

### The 8PPB bit-order trap

Historical: the first driver used epdiy's `MODE_PACKING_8PPB` with a 1bpp buffer, on the claim that
it is byte-for-byte CrossPoint's format. Five of six properties match — 1bpp, 8 pixels per byte,
`0` = black, row-major, `width / 8` stride — but **bit order within a byte is reversed**
(`EInkDisplay` puts the first pixel in the MSB, epdiy in the LSB).

It hid because reversing a byte whose bits are all equal is a no-op, so filled rectangles and rules
looked perfect; only glyph strokes, 1–2 px wide and therefore nearly all mixed bytes, were mirrored.
The push path now expands 1bpp into 4bpp through a `constexpr` table, so the packing question is
moot, but the shape of the mistake is worth remembering.

**Method note.** Three hypotheses reasoned from source were all wrong (font decompression, 2-bit
misread, `drawLine` fast path). Two measurements settled it: dumping the glyph bitmap both ways
proved the font data and decode correct, then hand-blitting a glyph through `drawPixel` put the
fault *below* the renderer. No amount of reading `drawText` would have shown that. Prefer the
measurement — a lesson this port then had to learn twice more, in [Panel refresh](#panel-refresh).

### Core affinity during refresh

`render.c:310` creates one `epd_prep` feed task **per core** (`NUM_RENDER_THREADS = 2`, pinned via
`xTaskCreatePinnedToCore(..., i)`) at `configMAX_PRIORITIES - 1`, the highest priority in the
system. A refresh therefore occupies **both** cores at maximum priority. The tasks block on their
line queues when idle, so this costs nothing between refreshes — but during one, nothing else runs
meaningfully.

Consequences to design around:

- **The overlap trick is unavailable.** On the SSD1677 boards you can start a refresh and prepare
  the next page while the controller works. Here there is no core to do it on. Any page-preparation
  optimisation has to run in the *idle* time between refreshes, not during them — which is fine,
  since a reader spends far longer idle than refreshing, but it is a different mechanism.
- **Watchdog headroom.** The idle task is starved on both cores for the duration. Worst case is
  `epd_clear()` at 1.6 s against a 5 s default.

`HalDisplay` routes every method to `Hz52Display` under `CROSSPOINT_BOARD_HZ52`. Its
SSD1677-shaped greyscale surface (`copyGrayscale*`, `writeGrayscalePlaneStrip`) is stubbed inert
here — that scheme has no analogue on this panel, so 16-level greyscale (milestone 9) is a rewrite
of that path rather than a wiring-up.

### Panel refresh

**(2026-08-02) — resolved.**

Page turns left a shadow of the previous page wherever it had ink — on tategaki pages, a boxy
residue per character cell. Four independent things were wrong. Several were fixed together, so the
individual contributions cannot be cleanly attributed; what follows is the mechanism of each.

**1. The pixel clock was halved.** epdiy drops 22 → 11 MHz when it sees a 32-byte data cache line
(`lcd_driver.c: check_cache_configuration`), which is what Arduino's prebuilt libs ship. `[env:hz52]`
now sets `custom_sdkconfig`, so pioarduino rebuilds those libs from source with
`CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE=64`; the guard stops firing, and the clock-halving log strings
are dead-stripped from our binary exactly as they are from the vendor's. This turned out **not** to
be a ghosting cause — everything still ghosted afterwards — but it halved every refresh, which is
what made the real fixes affordable. See [Raising the pixel clock](#raising-the-pixel-clock-to-22-mhz)
for what it entailed.

**Do not** force the clock with a build flag. Patching out the halving was tried and boot-loops the
device with visible artifacting; the guard is load-bearing. Make its premise false instead.

**2. Nothing was ever driving the whole panel.** `epd_hl_update_area` diffs front against back and
hands `dirty_lines`/`dirty_columns` to `epd_draw_base`, so pixels whose value did not change are
never driven *whatever mode is requested*. A "full refresh" therefore repainted the glyphs that had
moved and nothing else. This is the single most important finding, and it explains why every
experiment in the table below failed: they all varied the drive applied to a diff that already
excluded the pixels holding the residue.

**3. The dirty masks were producing the seams.** With the panel finally being driven properly, a
faint grid of light outlines appeared on character-cell boundaries. That was not residue: the masks
are per-row *and* per-column, so what got driven was the intersection of changed rows and changed
columns, and the boundaries between driven and undriven bands read as seams. `drawFullScreen()`
passes NULL for both, driving every line and column. It costs nothing — frame time is fixed by the
line clock, not by how many pixels are dirty.

**4. The waveform's phase weighting was being flattened.** `EpdWaveformPhases.phase_times` is read
in exactly one file, `output_i2s/render_i2s.c`. The LCD render path this board uses gives every
phase one panel scan at fixed line timing, so every phase gets an equal ~24.4 ms. ED097TC2's GC16
first half is `15,8,8,8,8,8,10,10,10,10,20,20,50,100,200` — its final settle is meant to be 41% of
the half-waveform and was getting 6.7%, so transitions landed short of the rail.
`scripts/gen_hz52_waveform.py` resamples the designed timeline onto equal frames (repeating a phase
being the only way to hold it longer), giving that settle 8 frames of 30 instead of 1. Registered
via `epd_hl_waveform()` — data only, no change to the draw path.

**Mode choice.** With the above fixed, decoding the LUTs for pure B/W content shows GC16 and GL16
differ in exactly one class of pixel:

| | W→W | W→B | B→W | B→B |
| --- | --- | --- | --- | --- |
| GC16 | darken + lighten | darken | lighten | — |
| GL16 | — | darken | lighten | — |

GC16 takes the unchanged white background through a full darken-then-lighten cycle — that is the
visible flash. GL16 leaves it alone, so page turns use GL16 and show no flash, while changed pixels
still get the full 15 phases each way. GC16 remains the interval refresh precisely because GL16
never drives `W→W` or `B→B`, so the background would otherwise never be reset at all.

**5. Half of GL16's frames were idle.** GL16's 30 phases are two 15-phase halves: the first drives
`W→B`, the second `B→W`. So each pixel does nothing for half of them — a `W→B` pixel is idle during
15–29, a `B→W` pixel during 0–14. For greyscale that sequencing is required, but binary content only
reaches transitions that live in one half, and the panel drives every pixel independently. The
generator superimposes the halves instead of concatenating them: identical per-pixel drive in 15
frames. **Page turns 0.74 s → 0.40 s.**

This is correct *only* while the content is binary, which is load-bearing rather than incidental.
Over all 256 `(from, to)` transitions, real GL16 drives **224 in both halves** — a mid-grey to
mid-grey pixel genuinely needs drive-to-rail then drive-to-target, in sequence. Only the 30 starting
from a rail are single-half, and those are exactly what binary content reaches. The merged table
covers only those 30, so a greyscale pixel would get *no drive at all* rather than a degraded one.
`--no-merge` emits the real 30-phase GL16 for when milestone 9 lands, and the generator refuses to
merge if it ever finds the halves contesting a transition.

An earlier note here claimed the merge would be valid for greyscale too, on the grounds that the
two tables never contest an entry. That was a misreading: they never contest because they *barely
overlap*, leaving 226 transitions untouched by both — sparse coverage, not disjoint coverage.

DU is not usable at any point: 5 flat phases, a one-directional push with no reset stage. Nothing
shorter exists either. epdiy's mode enum lists `GC16_FAST`(3), `A2`(4), `GL16_FAST`(6) and `DU4`(7),
but no bundled table implements any of them — epdiy hand-synthesizes waveforms from per-panel
frame-time tables rather than parsing vendor `.wbf` data (`scripts/epdiy_waveform_gen.py`), and has
no ED052TC4 entry at all. Its own ED052TC4 display definition pairs the panel with
`epdiy_ED097TC2`, exactly as we do.

**Why X3/X4/M4 never hit any of this.** They are not comparable hardware. Those boards have an
SSD1677-class controller holding per-transition LUT banks — `LUTWW`, `LUTBW`, `LUTWB`, `LUTBB`,
`LUTC` — selected per pixel from (old state, new state). `LUTWW`/`LUTBB` mean **even unchanged
pixels get driven**, which is what prevents both drift and seams. Those LUTs are also OEM-extracted
for that exact panel (see `EInkDisplay.cpp`: "Values mirror the OEM V5.6.21 X3 firmware LUT bank").
Here the SoC drives the panel directly, epdiy diffs in software, and unchanged pixels get nothing.

#### Hypotheses eliminated along the way

Each by measurement rather than reasoning. Every one varied the *drive*; none could have worked,
because the diff excluded the pixels that mattered.

| Hypothesis | Test | Result |
| --- | --- | --- |
| Too few drive frames | `MODE_DU` (5) → `*_TO_GL16` (15) | 3× slower, **no visual change** |
| 1bpp cannot run a waveform | rewrote to 4bpp/`epd_hl` | real waveforms now, ghosting unchanged |
| Wrong waveform family | swapped `ED097TC2` → `ED047TC2` | no change |
| Wrong temperature band | PMIC reads 32 °C (die, not glass); applied −10 °C | no change |
| DU inherently ghosts | switched page turns to `MODE_GL16` | no change — but masks were still on |
| Wrong drive voltage | read TPS65185 registers | VCOM −2.70 V as set; `PG=0xFA` — but see below |
| Waveform phases mis-timed | rebuilt Arduino at a 64-byte cache line, 11 → 22 MHz | no change; page turns 265 → 152 ms |

The GL16 row is worth noting: that test predates both fixes GL16 needed. The masks were still on, so
it painted bands rather than the whole screen, and `phase_times` was still being flattened, so no
transition completed. GL16 is what page turns use now.

#### Two things that do not work — do not retry

**A white-only flash instead of a black one.** `epd_push_pixels(area, t, 1)` drives the whole panel
toward white *unconditionally*, so unlike an `epd_hl` paint it does reach every pixel — it looked
like the way to get a reset that fades to white rather than flashing black. It produces a mottled
grey screen with the previous image showing through, which is worse than what it replaces.

The reason is in `epd_clear_area_cycles`: each cycle is **10 dark + 10 lighten + 2 neutral** frames
(`color` 0 = `DARK_BYTE`, 1 = `CLEAR_BYTE`, default = `0x00`, i.e. no drive). The dark half is what
resets, and the neutral frames let the pixels settle. Lighten frames alone drag particles partway
and leave them un-settled. Same physics that makes DU ghost: a one-directional push cannot reset
e-ink. **The dark stage is not cosmetic, and there is no white-only equivalent.**

**Absolute full-screen painting via `epd_draw_base`.** `MODE_EPDIY_WHITE_TO_GL16` (15 phases, no
inversion stage) is only reachable with `MODE_PACKING_2PPB`, which in turn requires a
`PREVIOUSLY_WHITE`/`PREVIOUSLY_BLACK` flag — without one, `find_lut_functions` returns
`EPD_DRAW_LOOKUP_NOT_IMPLEMENTED` (0x2). Supplying it makes the call execute and **hangs the device
hard on the LCD render method**: no serial, unrecoverable over USB-JTAG.

Recovery, if it happens anyway: press the physical reset button, then catch the device in the
bootloader with `esptool --after no-reset` and flash the app directly
(`write-flash 0x10000 .pio/build/hz52/firmware.bin`). `pio run -t upload` cannot connect, because
its own reset lets the hung firmware run again.

#### The drive-voltage row is not as settled as it reads

`PG=0xFA` was recorded as "all rails good", but our own decode of that byte prints `vddh=0 vneg=0`:

```text
[3815] [INF] [EPD] PMIC power-good: all=1 vb=1 vddh=0 vpos=1 vneg=0
```

`hz52_display.cpp: logPmicState` assumes "bit7 is the summary flag, the low nibble reports each
rail" and reads bits 3/2/1/0. One of two things is true, with very different consequences:

- the bit-layout assumption is wrong and the log line is cosmetically misdecoded, or
- **VDDH and VNEG genuinely are not power-good.** VNEG is the negative rail; a weak VNEG
  under-drives the toward-white transition and leaves residue exactly where ink was.

Worth checking against the TPS65185 datasheet's PG register definition. Ghosting is resolved without
it, so this is no longer urgent — but the row should not be treated as ruled out.

#### Vendor firmware static analysis

Closed off the remaining "they must have something we don't" theories:

- **The waveform tables are byte-identical to upstream epdiy.** Both are compiled in — `ED097TC2`
  GC16 LUT at `0x452844`, `ED047TC2` GC16 at `0x45737C`. There is no bespoke ED052TC4 table to find.
- **Same board definition**: `epd_board_v7_raw` (3/3 log strings present), `pca9555.c` absent.
- **Stock's normal mode is a greyscale refresh, not DU** — its menu reads
  `切换到8级灰刷新 (默认波形)` ("switch to 8-level grey refresh, default waveform"), with
  `(ED047波形)` as the 16-level option. We now do the same thing for a different reason: GL16 rather
  than DU, chosen for the flash rather than the residue.
- **Stock ran the panel at 22 MHz while we ran at 11.** Its descriptor carries `bus_speed = 22`, and
  all three of epdiy's clock-halving log strings were *absent* from both vendor builds while present
  in ours. That branch gates on a compile-time constant, so the compiler strips it when the cache
  line is 64 B — the binary was telling us stock built with
  `CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE=64`. Since closed.

### Raising the pixel clock to 22 MHz

Detail for item 1 above. Measured on device: page turns 261–281 ms → 140–155 ms, full clear
3033 ms → 1589 ms, and epdiy's three clock-halving warnings gone from the boot log — the branch
dead-stripped from the binary, the same signature the vendor builds show.

The cheap route worked: `custom_sdkconfig` in `[env:hz52]` is enough. It does trigger a from-source
rebuild of the Arduino IDF libs (`arduino.py: check_reinstall_frwrk` → `call_compile_libs` →
`espidf.py`), so `framework = arduino, espidf` was not needed. But "two-line fix" it was not — a
from-source Arduino build diverges from the prebuilt one in four ways, each of which had to be handled:

1. **Embedded data files.** `esp_insights` and `esp_rainmaker` embed server certificates via
   `target_add_binary_data()`. PlatformIO drives the build with SCons rather than ninja and reimplements
   that for exactly one case, the mbedtls bundle (`espidf.py: generate_mbedtls_bundle`); everything else
   gets a source-less `.S` in the code model and the build dies. `scripts/embed_idf_data_files.py` reads
   the recipes back out of the CMake-generated `build.ninja` and registers equivalent SCons builders.
   Dropping the two components instead would have been simpler, but the Arduino wrapper libraries for
   them are only removable via `CONFIG_ARDUINO_SELECTIVE_COMPILATION`, which silently also drops five
   libraries that have no `CONFIG_ARDUINO_SELECTIVE_*` symbol to re-enable them (`USB`, `HTTPUpdate`,
   `ESP_NOW`, `ESP_I2S`, `ESP_HostedOTA`) — leaving hz52 with a different Arduino surface to the
   murphy_m4 baseline, and muddying the one variable this change exists to isolate.
2. **`-Wl,--wrap=log_printf`.** `esp_diagnostics` defines `__wrap_log_printf` only under
   `CONFIG_LIB_BUILDER_COMPILE`, a symbol that lives in Espressif's `esp32-arduino-lib-builder` and has
   no Kconfig entry in the shipped framework, so kconfig drops it from a from-source build. The matching
   `--wrap` flag lives in the prebuilt `flags/ld_flags`, which the rebuild does *not* regenerate
   (`espidf.py: idf_lib_copy` copies archives and `sdkconfig.h`, not `flags/`). The wrapper is only an
   ESP Insights capture hook that forwards to `__real_log_printf`, so `[env:hz52]` unflags the wrap.
3. **`-mdisable-hardware-atomics`.** pioarduino greps the raw text of `custom_sdkconfig` for
   `PSRAM`/`CONFIG_SPIRAM=y` and, finding neither, strips this flag (`arduino.py: has_psram_config`).
   The ESP32-S3 cannot do hardware atomics against PSRAM addresses, so that would silently miscompile
   every atomic on this board. `[env:hz52]` therefore restates `CONFIG_SPIRAM=y` — redundant as config,
   load-bearing as text.
4. **Stale `sdkconfig.<env>`.** kconfig prefers an existing `sdkconfig.hz52` over `sdkconfig.defaults`,
   so an edit to `custom_sdkconfig` can appear to do nothing. Both files are generated and gitignored;
   delete `sdkconfig.hz52` when changing the option.

The Arduino framework package is *shared state*: building `murphy_m4` afterwards reinstalls it back to
the prebuilt libs, and returning to `hz52` rebuilds from source. Both directions were verified — M4
builds against pristine Feb-dated libs with a 32-byte line, hz52 round-trips back to 64. Alternating
between the two envs costs one framework rebuild each way.

**5. Physical button input — done (2026-08-01).** Three buttons (`38` confirm/back-on-hold, `0` up, `21` down) drive the normal activity stack. `GPIO21` is the wake source; `GPIO0` is also the boot strap, so it is read but never held at reset.

Two blockers had to clear together before the bring-up guard in `setup()` could be lifted, since both abort or strand the boot rather than degrade:

1. **`HalEnvSensor::begin()` calls `Wire.begin(ENV_SDA, ENV_SCL)` unconditionally** ([HalEnvSensor.cpp:28](../lib/hal/HalEnvSensor.cpp#L28)). epdiy owns SDA=39/SCL=40 through IDF for VCOM and panel temperature; claiming it with Arduino `Wire` makes `epd_board_init()` fail its i2c assert and abort. Needs gating on `hasEnvironmentalSensor`. `HalClock` is already safe — it gates on `deviceIsX3()`/`deviceIsMurphyM4()`.
2. **Input is not wired.** `HalGPIO::begin()` bypasses `InputManager` on this board (it assumes `POWER_BUTTON_PIN=3` and an ADC ladder on GPIO1/2), so the three buttons reach no activity; the UI would render but not navigate.

**6. Japanese EPUB smoke test — passing (2026-08-02), not stress-tested.** A tategaki EPUB renders end to end through the TTF reader: section cache deserialises, ruby and vertical layout look right, progress saves and resumes. The FD-pool trouble seen on M4 (`max_files` raised to 12) has not appeared, but nothing has deliberately probed for it.

**7. UI density pass — not started.** [Area 3](#area-3-ui-density-at-283-ppi). Board `ppi` capability, UI font sizing, sweep of hardcoded layout constants.

**8. Three-button UX conversion — not started.** Jump menus replacing `Left`/`Right`, hint layout, `ButtonRemapActivity` hidden, keyboard-entry strategy decided.

Two symptoms reported from device use on 2026-08-02, both belonging here rather than to milestone 5:

- **No way out of the file browser.** There is no dedicated Back button on this board —
  `hz52LongPressButton` maps only the isolated front button (`GPIO38`) to `BTN_BACK`, as a long
  press, while a short press on the same button is Confirm. Establish whether the browser
  ignores `BTN_BACK` (a real bug) or whether the gesture is simply undiscoverable (a hint-layout
  problem, which is this milestone's actual subject).
- **No way to reach ruby placement settings** from the reader with three buttons. Unverified
  whether the setting is unreachable or absent.

**9. 16-level greyscale — not started.** Native 4bpp reader rendering. Two prerequisites, both
recorded where they bite: `HalDisplay`'s greyscale surface is SSD1677-shaped and inert here, and the
GL16 waveform is currently **merged for binary content only** — regenerate with
`scripts/gen_hz52_waveform.py --no-merge` before raising `displayGrayscaleBits`, or every
mid-grey pixel gets no drive at all. Page turns would go ~0.40 s → ~0.74 s.

**10. Fully functional Japanese EPUB — not started.** Horizontal and vertical, ruby/furigana through parse/cache/layout/render, dictionary cursor geometry, SD fonts, bookmarks, TOC, footnotes, percent/chapter nav, orientation, progress save/resume.

**11. Power, battery, sleep — not started.** Battery divider on `GPIO1`, power-off path (the vendor has one — `关机`), deep-sleep framebuffer persistence, wake sources, no-RTC clock strategy, PMIC temperature.

**12. BLE HID remote — not started.** Promoted from optional accelerator to a real deliverable, because it is the pressure valve for the three-button budget.

**13. Optional accelerators — not started.** PSRAM cache budget tuning once stability is proven, screenshots, web/OPDS/KOReader parity.

## Still Open

- **What `GPIO47` is.** Confirmed *not* a button. Input with pull-up, reads **HIGH while USB-connected**.

  Note that M4's charger-status line (`GPIO43`) read *LOW* while charging, so simple charger status is a poor fit unless the polarity is inverted or the battery was already full. **Leading candidate: TPS65185 `PWRGOOD` or `INT`.** `GPIO47` is upstream V7's `D15`, freed by HZ5.2's 8-bit bus. Having dropped the PCA9555 expander that normally carries `PWRGOOD`/`INT`, the vendor needed direct GPIOs for them — and both are open-drain inputs requiring a pull-up, exactly this pad's signature. SD card-detect is the alternative.

  **Do not try to characterise this by unplugging USB.** JTAG rides on the same USB connection, so disconnecting removes the only channel for reading the pin — the test is self-defeating. M4's charger line was characterised by custom firmware logging it, not by JTAG. Tests that keep USB attached, best first: sample across a page refresh (`PWRGOOD` asserts when the panel rails come up); eject the SD card and re-read (card-detect); hold a magnet near the bezel (hall/lid). Otherwise defer to milestone 3, when our own firmware can log it.
- **Individual roles of the remaining software-GPIO control lines** (`9`,`10`,`11`,`12` LOW). Partly resolved: `14` = TPS65185 `WAKEUP`, `44` = SD chip-select, `46` = SD power gate (a real gate, not a buffer enable — the card loses state when toggled), `45` = `STV` from the upstream match. The rest are the ex-PCA9555 set — `OE`, `MODE`, `PWRUP`, `VCOM_CTRL` — and the upstream `epd_board_v7_raw` assignment is the working hypothesis. Note a watchpoint trace taken *during* panel refreshes wrongly implicated `GPIO9` as chip-select; sample when only the subsystem of interest is active.
- ~~**Which I²C devices sit on `SCL=40`/`SDA=39`.**~~ **Resolved (milestone 3):** the **TPS65185 at `0x68`**, and nothing else. The bus reads empty until `GPIO14` (`WAKEUP`) is asserted — found by sweeping the control block one pad at a time, since a scan on a correctly pulled-up bus returning zero devices is a *power* symptom, not a wiring one. No RTC found, consistent with `hasRtc = false`.
- A safe **PSRAM cache budget**. Size is settled: 8 MB octal, leaving ~7.4 MB after the display's ~900 KB. Budget stays 0 until measured under a real Japanese book with a large SD font loaded.
- **Battery divider ratio**, and whether a charger-status line exists.
- **Deep-sleep wake pin.** Both vendor builds reference `rtc_gpio_*`, so it is an RTC-capable pad (`GPIO0`–`GPIO21`) — likely `GPIO0` or `GPIO21`.
- **Power-off / power-latch mechanism** behind `关机`.
- **Waveform LUT size** and internal-SRAM cost; whether 16-level via the ED047 waveform is safe for sustained use.
- ~~**`display_type`**: upstream `ED052TC4` says `1`, vendor ships `2`.~~ **Resolved (milestone 4):** inert on our code path. Orientation is applied entirely by `GfxRenderer`; use upstream `ED052TC4` unmodified.
- **Whether an external RTC exists** (no evidence found, but absence of strings is not proof).
- **Download-mode entry sequence.** JTAG posture is settled: enabled.

Per the M4 plan's closing warning, which applies with more force here: **do not brute-force GPIOs on an e-paper panel or its PMIC.** Wrong rail assumptions on a parallel panel with a programmable-VCOM PMIC can damage hardware. Ask the vendor, read the epdiy board configs, then probe with a specific hypothesis.

One email to the vendor asking which epdiy board config they used would likely settle the eight control lines and the PMIC part in a single reply — the largest remaining item.

## Capability Model Additions

Fields to add to [BoardCapabilityProfile](../lib/hal/BoardProfile.h#L8-L47). Prefer these over a third `#ifdef` arm, and migrate existing `CROSSPOINT_BOARD_MURPHY_M4` UX sites to capability queries as part of this work.

| Capability | Purpose |
| --- | --- |
| `displayBus` | `SPI_SSD1677` vs `PARALLEL_EPDIY` — selects the whole display backend |
| `displayPpi` | Drives UI font sizing and layout scaling ([Area 3](#area-3-ui-density-at-283-ppi)) |
| `displayFramebufferBpp` | 1 vs 4; selects the render target model |
| `displayFramebufferInPsram` | Framebuffer is not static DRAM; affects sleep persistence |
| `displayVcomControllable` / `displayVcomDefaultMv` | Software VCOM exists and has a persisted value (`-2700` here) |
| `displayHasPanelTempSensor` | Waveform temperature compensation |
| `requiresFramebufferPersistAcrossSleep` | PSRAM framebuffer does not survive deep sleep |
| `inputLongPressIsPrimary` | Three-button boards promote long-press to a first-class action |
| `inputSupportsBleHid` | BLE page-turner remote is a supported input provider |
| `hasHardwarePowerOff` | HZ5.2 appears to have one; M4 does not |

Existing fields for HZ5.2: `socFamily` = `ESP32S3`, `hasPsram` = true (octal, size TBC), `psramCacheBudgetBytes` = 0 until measured, `displayWidth`/`displayHeight` = `1280`/`720`, `displayGrayscaleBits` = 1 then 4, `displaySingleBufferRequired` = false, `inputButtonCount` = 3, `inputHasTouch` = false, `touchController` = `"none"`, `hasFrontlight` = false, `hasRtc` = false, `sdRequired` = true, **`sdUsesSdMmc` = false**. The `sdClkPin`/`sdCmdPin`/`sdD0..D3Pin` fields do not apply; SPI equivalents are needed.

## Cache And Test Checklist

Additions to the M4 checklist, which still applies in full:

- Viewport `1280x720` must produce a distinct section cache from `800x480`. Guaranteed by [Section.cpp:268-292](../lib/Epub/Epub/Section.cpp#L268-L292), but verify — a shared SD card between an M4 and an HZ5.2 is realistic.
- Rerun all Japanese EPUB tests after the 4bpp render-model change, not just after cache-key changes. Ruby positioning, vertical geometry, and dictionary cursor hit-boxes all read glyph bitmaps.
- Verify PSRAM cache budgets leave headroom after the display's ~900 KB, under a real Japanese book with a large SD font loaded.
- Verify every `Left`/`Right`-dependent flow has a three-button equivalent, screen by screen.
- Verify UI text is physically legible at 283 ppi on real hardware. This one cannot be checked by inspection.
- Verify long-run 16-level refresh does not ghost or stress the panel.
- Verify the sleep/wake framebuffer path: sleep, wake, confirm panel content matches with no full-refresh flash.

## Appendix: GPIO Recovery Method

**Validated on Murphy M4 (ESP32-S3) and Xteink X3 (ESP32-C3) against known ground truth before being used on HZ5.2.** Read-only, no flash writes, minutes per board.

Two results show it works blind rather than confirming what it was fed: on X3 it flagged pads 1 and 2 as analog before any cross-check — they turned out to be `BUTTON_ADC_PIN_1`/`_2`, since X3/X4 read buttons through a resistor-ladder ADC — and it identified pad 12 as an idle active-low chip select purely from "software-driven output, currently HIGH", which `SDCardManager.cpp` confirms is `SD_CS = 12`.

### Prerequisites

```bash
pio pkg install --global --tool "platformio/tool-openocd-esp32"
```

Upstream OpenOCD 0.12.0 may lack S3 target configs; use Espressif's fork. Confirm the device exposes USB-Serial-JTAG:

```bash
ioreg -p IOUSB -l -w 0 | grep -iE '"idProduct"|"USB Product Name"'
```

Expect `4097` (`0x1001`) and `USB JTAG_serial debug unit`. On Apple Silicon, **`could not find or open device` from OpenOCD despite the board appearing in `ioreg` means an unapproved USB accessory**, not a JTAG problem — approve the macOS prompt. If the device is absent from `ioreg` entirely it is in deep sleep; press a button. (macOS has no `timeout`; `ioreg` is faster than `system_profiler`.)

### Capture

```bash
OCD=~/.platformio/packages/tool-openocd-esp32
cd $OCD/share/openocd/scripts
$OCD/bin/openocd \
  -c "set ESP_FLASH_SIZE 0" \
  -f board/esp32s3-builtin.cfg \
  -c "init" -c "halt" \
  -c "mdw 0x60004000 512" \
  -c "mdw 0x60009000 64" \
  -c "resume" -c "esp32s3.cpu0 curstate" -c "exit"
```

`set ESP_FLASH_SIZE 0` *before* sourcing the board config disables OpenOCD's flash driver outright, so no flash bank is ever created — a safety belt against accidental writes. Confirm `curstate` reports `running`.

Do **not** issue `resume` before `halt`: on HZ5.2 `init` does not halt the cores (unlike M4), so a leading `resume` errors out and aborts the script. That also means HZ5.2 snapshots reflect genuinely-running, fully-initialised firmware.

Use `board/esp32c3-builtin.cfg` for X3.

### Registers (ESP32-S3)

| Register | Address | Meaning |
| --- | --- | --- |
| `GPIO_OUT` / `GPIO_OUT1` | `0x60004004` / `0x60004010` | driven level, pads 0–31 / 32–48 |
| `GPIO_ENABLE` / `GPIO_ENABLE1` | `0x60004020` / `0x6000402C` | output enable |
| `GPIO_IN` / `GPIO_IN1` | `0x6000403C` / `0x60004040` | live pad level |
| `GPIO_FUNCn_OUT_SEL_CFG` | `0x60004554 + 4n` | peripheral output signal routed to pad `n` |
| `GPIO_FUNCn_IN_SEL_CFG` | `0x60004154 + 4n` | indexed by **signal**: bits 4:0 source pad, bit 6/7 matrix-routed |
| `IO_MUX_GPIOn` | `0x60009004 + 4n` | `FUN_SEL` 14:12, `DRV` 11:10, `FUN_IE` 9, `FUN_WPU` 8, `FUN_WPD` 7 |

### Decode rules

- **`OUT_SEL == <GPIO magic>`** → pad driven from `GPIO_OUT`, i.e. software GPIO — chip-selects, DC/RST, enables. **The magic value is chip-specific**: 9-bit field on S3 with GPIO = `256`; 8-bit on C3 with GPIO = `128` (there both `0x080` and `0x280` decode to 128, the extra bit being `OEN_SEL`). Mask `& 0x1ff` on S3, `& 0xff` on C3.
- **`OUT_SEL < magic`** → peripheral output signal index, which *names the pad's function*.
- **`OUT_SEL` alone misses input-only pins.** Invert `GPIO_FUNCn_IN_SEL_CFG` too — on X3 that was the only way `SPI_MISO = 7` was recoverable.
- **Consecutive signal indices across scattered pads = a bus.** This alone found HZ5.2's 8-wide panel data bus and M4's 4-bit SD map.
- **`FUN_IE = 0` with no pulls** → analog pad (ADC).
- **`FUN_IE = 1`, `FUN_WPU = 1`, no output enable** → button or status input.
- `GPIO_OUT` level on a software-GPIO pad reveals polarity in use.

### Signal names

**Do not hand-derive the table — ESP-IDF ships it**, and PlatformIO already has it locally:

```text
~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/include/soc/esp32s3/include/soc/gpio_sig_map.h
~/.platformio/packages/framework-arduinoespressif32-libs/esp32c3/include/soc/esp32c3/include/soc/gpio_sig_map.h
```

Parse with `#define\s+(\w+?)_IDX\s+(\d+)`. This corrects errors hand-inversion makes — HZ5.2's signal `82` resolves as `RMT_SIG_OUT1`, not the LEDC channel it superficially resembles.

### What registers cannot tell you

The registers state a pad's *peripheral block* and *electrical role*. They never state which physical device sits on the far end. On M4 the "touch" and "frontlight" labels came from the port doc, not silicon — and pads `0`,`1`,`2` (buttons) had signatures identical to pad `43` (charger status). Distinguishing those requires a press-delta test (re-read `GPIO_IN` while each button is held, as used here to map `38`/`0`/`21`) or an I²C address scan.

## Appendix: Vendor Firmware Analysis

Two vendor builds were analysed before hardware arrived. They remain useful for the stock UX reference and for facts not visible in registers, but the device itself is now the authority.

| Item | Newer | Older |
| --- | --- | --- |
| File | `test/firmware-5inch-01-droid-sans-fallback.bin` | `test/5.2_v3_droid.bin` |
| Size | `7,245,824` | `6,356,480` |
| SHA-256 | `b260291c…65` | `b0858e9b…3d` |
| Compile time | `Jul 19 2026` | `Jun 14 2026` |
| ESP-IDF | `v5.5.4` | `v5.3.4-dirty` |
| App offset | `0x10000` | `0x20000` |

Both are **merged flash images** (bootloader + partition table + app, flashable at `0x0`) — so either is a firmware rollback path. Neither contains NVS.

Partition layouts differ between builds — newer: `nvs` 24K, `app0` 15424K, no OTA/SPIFFS/coredump; older: `nvs` **64K**, `otadata` 8K, `app0` 14336K, `spiffs` 1024K. **Read the live table off the unit; never assume either.** The vendor has no in-place update mechanism; the 15 MB app exists because they embed a 3.76 MB font.

Key facts derived:

- **Display descriptor** recovered from DROM at `0x4517D8`: `1280×720`, bus width 8, bus speed 22 MHz, `display_type = 2`. Byte-identical across both builds, so it is a settled decision rather than an experiment. This is what identified the panel as `ED052TC4` and surfaced the `display_type` discrepancy with upstream.
- **Fonts:** FreeType at runtime (`font_ft`) with variable-axis support, plus **Droid Sans Fallback v2.55b** embedded in flash — 51,064 glyphs, 3.76 MB, byte-identical across both builds, and carrying **`vhea`/`vmtx` vertical metrics**. A second independent data point for [ttf-migration-plan.md](ttf-migration-plan.md)'s runtime-TTF direction.
- **Formats: the vendor *removed* EPUB support between the two builds.** The older build carries the full machinery — `mz_zip_reader_init_file`/`_file_stat`/`_extract_to_mem`, `META-INF`, `container.xml`, `rootfile`, `opf`, `manifest`, `spine`, and the `.epub` extension. **None of it survives in the newer build**, which references only `.txt`, `.im52` and `.im60`. Residual `SELECTING_EPUB` / `Creating epub list` / `EPUB folder TOC` strings are vestigial naming inherited from atomic14, not working code.

  Current shipping firmware therefore reads **TXT only** (block-paged), plus IMX pre-rendered raw images (`/fs/cover/%d.im52`, `.im60` — the vendor ships 5.2" and 6.0" variants) and a built-in help reader. Log strings (`EPUB folder TOC: %zu items`, `长按返回EPUB文件夹章节列表`, `没有找到章节`, `到达章节末尾，切换到下一章`) indicate a **folder-per-book** model: a directory on the SD card is a book and the `.txt` files inside it are chapters. Loose `.txt` files at the root appear not to be listed.

  This is a meaningful gap CrossPoint closes rather than inherits — our EPUB engine, Japanese layout, ruby, and vertical text are exactly what this hardware currently lacks.
- **Sleep:** compresses the framebuffer to `/fs/front_buffer.z` and reinflates on wake, because PSRAM does not survive deep sleep.
- **BLE:** Bluedroid HID **host**, scanning for `KEY`-suffixed page-turner remotes, bonding, storing in NVS, auto-reconnecting on wake.
- **Wi-Fi:** SoftAP `elink` at `192.168.4.1` for provisioning, then STA with an embedded HTML file manager. NTP from `ntp.aliyun.com`.
- **Battery:** older build uses legacy `adc1_*` (newer migrated to `adc_oneshot`), which independently narrowed the battery input to ADC1 — `GPIO1`–`GPIO10`. The register read then found the analog pad at `GPIO1`.
- **No touch, no frontlight strings** of any kind, in either build.

**Stock UI inventory**, useful as a three-button UX reference: main menu (book list, `文件管理` file manager, `WIFI传书` transfer, `旋转屏幕` rotate, `系统设置` settings, `关机` power off); system settings (Bluetooth, sleep timeout 3/5/10/30/180 min or never, sleep screen default/image/clock, help, VCOM); page settings (font size 20–60, weight normal/semi-bold/bold, margins 5–50, line height 10–50%); and — most relevant to [Area 2](#area-2-three-button-ux) — explicit jump menus in place of directional navigation.

**Provenance:** the firmware is derived from **atomic14's `esp32-epub-reader`** (UI states `SELECTING_EPUB`/`READING_*`, `EpdiyFrameBufferRenderer`, `Creating epub list`), heavily extended. It is **not** a CrossPoint derivative — unlike Murphy's firmware, which leaked our source paths — so there is no shared architecture to mine.

## References

- **epdiy** (`github.com/vroland/epdiy`) — the primary dependency. Ships the `ED052TC4` definition (`src/displays.c`), waveform tables, S3 output path (`src/output_lcd/`), board configs (`src/board/`), LUT modes, and PMIC/VCOM control. **Confirm its license before vendoring**, and verify it builds against pioarduino `platform-espressif32` 55.03.37 / IDF 5.5.x — it is IDF-native and we are Arduino-as-component.
- **Murphy M4 port** ([esp32-device-migration-comparison.md](esp32-device-migration-comparison.md)) — the process template.
- **atomic14 `esp32-epub-reader`** — explains what the vendor firmware does and why its UX looks the way it does. Not a reader-engine reference.

Philosophy check, per [CLAUDE.md](../CLAUDE.md): this is a dedicated e-reader port, not a feature expansion. Every item should be justified by "the Japanese reader works well on this hardware" — anything that is not (IMX-style formats, dashboards, BLE beyond page-turn input) stays out of scope.
