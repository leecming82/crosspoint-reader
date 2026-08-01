# HZ5.2 Japanese Port Plan

Date: 2026-07-27

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

Result: `pio run -e hz52` succeeds — 5,395,030 bytes, 75.5% of the app partition, RAM 34%. `murphy_m4` still succeeds (5,504,214 bytes).

**Pre-existing, not caused by this work:** `pio run -e default` fails, and did before these changes. [SettingsList.h](../src/SettingsList.h) calls `buildTtfFontFamilySetting()`/`buildTtfFontSizeSetting()`/`buildTtfFontWeightSetting()` unconditionally while declaring them only under a guard no C3 environment defines. Consistent with [ttf-migration-plan.md](ttf-migration-plan.md) scoping X3/X4 out of this branch. Left alone rather than silently repaired.

**Bring-up guard.** Flashing as built would have been destructive: the `hz52` build falls through to the X4 pin block in [HalGPIO.h](../lib/hal/HalGPIO.h), which maps `EPD_MOSI` to `GPIO10` (a TPS65185/panel control line), `EPD_CS` to `GPIO21` (a button to ground), `EPD_DC` to `GPIO4` (`LCD_PCLK`), and SCLK/RST/BUSY/MISO onto `LCD_DATA_OUT9/14/15/12`; `SDCardManager`'s `SD_CS = 12` is another control line on the same bus. Driving `GPIO10` risks powering the panel rails with no waveform scanning, leaving the panel under a static DC field. `setup()` now sets the existing `bootDiagnosticsOnly` flag for HZ5.2 immediately after `logBootDiagnostics()` and returns, before any storage or display init. **Lift this guard only once the epdiy display HAL and SPI SD pins land (milestones 3–4).**

**Verified on hardware.** Flashed 5,371,072 bytes at `0x10000`, hash verified. Boot log confirms the board-profile banner, `Reset reason: USB(11)`, CPU 240 MHz, and a stable diagnostic heartbeat (heap flat at 221,396 across 20 s, no watchdog or panic). This also measured **PSRAM at 8 MB**.

Rollback path if needed: flash `test/firmware-5inch-01-droid-sans-fallback.bin` at `0x0` — it is a merged bootloader + partition table + app image. Note it will not restore the vendor's NVS, though VCOM is recorded here.

`clang-format` is not installed locally, so formatting is unverified against the CI format check.

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

**4. Display bring-up (epdiy, 1bpp) — done (2026-08-01). Renders through `HalDisplay`/`GfxRenderer`.**

The panel powers, clears, and draws. A 1bpp full-screen update completes in **221 ms** (**232 ms** including
the bit-order conversion below).

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

**Memory: use the low-level API, not `epd_hl_*`.** `epd_hl_init()` allocates **1.84 MB** of PSRAM for its
front/back diff pair. `epd_draw_base()` with `MODE_PACKING_8PPB` takes a **115,200-byte** 1bpp buffer — a 16×
saving. This makes render model (a) the proven path. The packing shares CrossPoint's polarity but **not** its
bit order — see "The 8PPB bit-order trap" below, which cost the most debugging time of anything in milestone 4.

**Pixel clock is halved to 11 MHz** because Arduino's prebuilt libs use a 32-byte data cache line; epdiy
reduces the clock rather than risk coherency faults. Stock is IDF-native and gets 22 MHz. Largely moot in
practice: at 221 ms for a DU update there is little left to win, so rebuilding the Arduino libs with
`CONFIG_ESP32S3_DATA_CACHE_LINE_64B` is not worth it for now.

Timings measured: `epd_clear()` 2.9 s, `MODE_DU` 1bpp update **221 ms**, `MODE_GC16` (4bpp, high-level) 1.45 s.
Ambient reads 31 °C from the PMIC.

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

The claim carried through most of milestone 4 — that 8PPB is byte-for-byte CrossPoint's format — is **wrong**,
and it was wrong in the most expensive possible way: five of six properties match, so the buffer renders a
recognisable image and looks correct on anything solid.

| Property | `EInkDisplay` (SSD1677) | epdiy `MODE_PACKING_8PPB` |
| --- | --- | --- |
| Bits per pixel | 1 | 1 |
| Pixels per byte | 8 | 8 |
| Polarity | `0` = black, `1` = white | same |
| Row order / stride | row-major, `width / 8` | same |
| **Bit order within a byte** | **MSB = first pixel** | **LSB = first pixel** |

Evidence, from `lut_8ppB_start_at_white` in `.pio/libdeps/hz52/epdiy/src/output_common/lut.c`: `lut[0x01]`
(input LSB set) alters the *lowest* output slot and `lut[0x80]` (input MSB set) the *highest*, and the panel
shifts that run out in the opposite sense to ours. `lut[0x00] = 0x5555` (all slots `01`, drive to black) and
`lut[0xFF] = 0x0000` confirm the shared polarity.

**Why it hid.** Reversing a byte whose 8 bits are all equal is a no-op, so filled rectangles, rules and
borders are unaffected apart from a ≤7 px nudge on their edge bytes. Every early test was solid shapes, so
the wrong assumption looked confirmed. Glyph strokes are 1–2 px wide, so nearly every byte is a mixed pattern
and nearly every one was mirrored.

**Why it did not look like a packing bug.** Because the panel is transposed (`phyX` is *logical y*), the
8-pixel byte group runs **vertically** on screen. Mirroring inside a byte flips 8 screen rows top-to-bottom,
so horizontal glyph strokes were displaced up and down in 8-row bands — presenting as a layout or font fault,
not a bit-order one. On a non-transposed panel the same bug would smear horizontally, which is far more
recognisable.

**Fix:** convert at the epdiy boundary in `Hz52Display::push()` via a 256-entry `constexpr` reverse table
(flash-resident, no DRAM cost) into a second PSRAM scratch buffer. ~11 ms per full push against a 220 ms draw.
Deliberately *not* in `GfxRenderer::drawPixel` — that is the hottest path in the renderer and shared by every
board, and bit order is a property of this panel's interface, not of CrossPoint's framebuffer. One convention
holds everywhere upstream; only the push adapts.

**Method note.** Three hypotheses reasoned from source were all wrong (font decompression, 2-bit misread,
`drawLine` fast path). Two measurements settled it: dumping the glyph bitmap both ways proved the font data
and decode correct, then hand-blitting a glyph through `drawPixel` put the fault *below* the renderer, which
no amount of reading `drawText` would have shown. Prefer the measurement.

**Core affinity resolved — and there is no choice to make.** `render.c:316` creates an `epd_prep` feed task
**pinned to every core** (`xTaskCreatePinnedToCore(..., i)`) at `configMAX_PRIORITIES - 1`, the highest priority
in the system, marked `IRAM_ATTR`. A refresh therefore occupies **both** cores at max priority; there is no
"give epdiy one core and keep the other" option. Consequences to design around:

- Nothing else runs meaningfully during a refresh — not Wi-Fi, not SD, not page pre-render. The overlap trick
  the SSD1677 boards get for free (start refresh, prepare the next page while the controller works) is
  unavailable here.
- The idle task is starved on both cores for the duration, so the task watchdog is a real consideration. Our
  measured worst case is `epd_clear()` at 2.9 s against a 5 s default — closer than is comfortable, and worth
  an explicit feed or a shorter clear.

Remaining for this milestone: fold `Hz52Display` behind the board profile into `HalDisplay` so activities
render through the normal path. Deliberately **not** attempted as part of the tidy-up: `HalDisplay` hardcodes
`EInkDisplay`'s geometry (`DISPLAY_WIDTH = 800`, `MAX_BUFFER_SIZE = 52272` static arrays) and exposes an
SSD1677-shaped grayscale surface (`copyGrayscale*`, `writeGrayscalePlaneStrip`) that has no analogue here.
Rewiring that touches every board and belongs with the render-path work, not with a cleanup commit.

**5. Physical button input — next.** Pins are known (`38`, `0`, `21`); build the 3-button + long-press event model behind a capability check. Document `GPIO0`'s boot-strap role.

Two blockers must clear together before the bring-up guard in `setup()` can be lifted and the activity stack
allowed to run — both abort or strand the boot rather than degrade:

1. **`HalEnvSensor::begin()` calls `Wire.begin(ENV_SDA, ENV_SCL)` unconditionally** ([HalEnvSensor.cpp:28](../lib/hal/HalEnvSensor.cpp#L28)). epdiy owns SDA=39/SCL=40 through IDF for VCOM and panel temperature; claiming it with Arduino `Wire` makes `epd_board_init()` fail its i2c assert and abort. Needs gating on `hasEnvironmentalSensor`. `HalClock` is already safe — it gates on `deviceIsX3()`/`deviceIsMurphyM4()`.
2. **Input is not wired.** `HalGPIO::begin()` bypasses `InputManager` on this board (it assumes `POWER_BUTTON_PIN=3` and an ADC ladder on GPIO1/2), so the three buttons reach no activity; the UI would render but not navigate.

**6. Japanese EPUB smoke test — not started.** Expect FD-pool issues as on M4 (`max_files` had to go to 12).

**7. UI density pass — not started.** [Area 3](#area-3-ui-density-at-283-ppi). Board `ppi` capability, UI font sizing, sweep of hardcoded layout constants.

**8. Three-button UX conversion — not started.** Jump menus replacing `Left`/`Right`, hint layout, `ButtonRemapActivity` hidden, keyboard-entry strategy decided.

**9. 16-level greyscale — not started.** Native 4bpp reader rendering (model (b)). Validate the ED047-waveform mode does not ghost or stress the panel over long runs.

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
