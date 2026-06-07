# Murphy M4 Japanese Port Plan

Date: 2026-06-07

This document tracks the port of this Japanese CrossPoint branch to the Murphy M4 e-reader. The Japanese reader remains the source of truth: ruby/furigana, EPUB writing-mode metadata, vertical tategaki layout, dictionary cursor geometry, SD-backed dictionaries, `.cpfont` fonts, and bounded-memory caches must keep their current semantics.

The received Murphy M4 is now concrete enough to plan around: ESP32-S3, 16 MB flash, native USB serial/JTAG download mode, a 4.26 inch `800x480`/`EPD426` display class, FT6336U touch evidence, warm/cool frontlight evidence, and a device-specific partition table that differs from the public Murphy Cloud manifest. Murphy/Corogoo also appears to have a separate 3.7 inch M4 line, so the shared public repo name `3.7-inch-ink-screen-reader` is treated as a distribution breadcrumb, not target-hardware identity.

## Milestones

1. Evidence and recovery baseline — done

   Captured serial/download-mode evidence, full flash, partition table, OTA data, bootloader, SPIFFS, coredump, and app descriptors before writing custom firmware. This gives us a rollback path and revealed PSRAM, active app slot, partition layout, security posture, and stock firmware lineage.

   Exit criteria: full flash backup is saved and restorable in principle; boot and partition metadata are decoded; the current stock firmware version and app slot are known.

   Result: artifacts are saved under `test/murphy-m4-baseline/`. The full 16 MB rollback image is `murphy-m4-full-flash-20260607.bin`, SHA-256 `b68c4d8a911d57c97ac238990c6f08d00efb27c352d97076ba52601af68e2a1e`. `app0` is selected and valid; `app1` is blank. Stock app metadata reports `arduino-lib-builder`, `esp-idf: v4.4.7 38eeba213a`, compile time `Mar  5 2024 12:12:53`, with MoFei strings and version-like strings `1.2.13`/`1.2.11`. Stock firmware mining found an embedded `EPD426-v1` update URL and public EPD426 OTA sibling, confirming 4.26 inch lineage but not exposing source-level GPIO definitions. Passive normal firmware serial logs were quiet at 115200; ROM download-mode logs were captured.

2. Murphy M4 board profile — done

   Add an ESP32-S3 Murphy M4 PlatformIO environment and board capability descriptor without changing the C3/X3/X4 baseline. Start conservatively: mark touch, frontlight, PSRAM, RTC, charger, gauge, and sensors as unknown or disabled until proven by hardware or logs.

   Exit criteria: the Murphy environment compiles, has the correct flash/partition posture for this unit, emits a board-profile banner over serial, and leaves existing C3 environments behaviorally unchanged.

   Result: added `env:murphy_m4`, `partitions_murphy_m4.csv`, and a `BoardCapabilityProfile` descriptor. The Murphy profile targets ESP32-S3, 16 MB flash, captured app offsets, `800x480` viewport, confirmed 8 MB PSRAM with zero cache budget until allocation is tested, touch disabled with `FT6336U-unverified`, frontlight disabled, RTC/battery/charger/sensors disabled, and SD required. Startup emits a board-profile banner through the existing serial logging path. `pio run -e murphy_m4` and `pio run -e default` pass.

3. Boot, diagnostics, and storage — done

   Bring up serial diagnostics, basic task/activity startup, NVS/SPIFFS handling, and SD/storage detection before display work. The firmware should be diagnosable even if the e-paper panel stays blank.

   Exit criteria: custom firmware boots, logs reset reason and memory state, can mount or report storage state, and can recover enough diagnostics to continue hardware bring-up safely even when serial/display/SD are unavailable.

   Result: custom Murphy firmware boots after a manual reset and reaches setup. Because USB serial and SD logging were not reliable on the bring-up unit, Murphy diagnostic builds now mirror `LOG_*` output into a bounded 64 KB scratch log at the start of the blank `app1` slot (`0x6e0000`). The log captures reset reason, wakeup cause, CPU/SDK, heap/internal heap, PSRAM, storage init result, and diagnostic heartbeats. Storage still needs the real Murphy SD pin map, but failures are now observable without serial, SD, or display output.

4. Display bring-up and app integration — in progress

   Reuse the X4 e-paper driver model on the Murphy M4 4.26 inch panel, changing only board integration where hardware evidence requires it. Keep physical scan dimensions, logical reader orientation, and viewport metrics separate.

   Exit criteria: test patterns and simple app screens render reliably without obvious panel stress; rotation/orientation is understood; busy timing is bounded; normal BW page turns use the X4 refresh commands; grayscale/AA behavior has an acceptable cleanup policy.

   Current result: the Murphy M4 display is an `800x480` GDEQ0426T82/SSD1677-style panel that matches the X4 display model. Confirmed differences are board-level: `SCK=4`, `MOSI=3`, `CS=5`, `DC=6`, `RST=7`, `BUSY=8`, no display MISO. RAM addressing also matches the working X4 path: X is pixel-addressed `0..799`, and Y is reversed `479..0`.

   Confirmed driver model: use the X4 init, RAM window/counter setup, BW RAM `0x24`, RED RAM `0x26`, and refresh commands. Normal BW page turns do not load custom factory LUTs; they write the two RAM planes and invoke the controller update path (`FAST`, `HALF`, or `FULL`). Cold/wake updates need the X4 stronger activation path before later condensed updates behave correctly.

   Grayscale/AA finding: the default X4 grayscale/AA path works on M4. A plain FAST page immediately after AA can leave visible residue, while the X4/base post-AA cleanup behavior using a stronger HALF-style refresh clears it. Keep that X4 post-AA policy for now and revisit only after real reader pages show a problem.

   Timing note: the SPI-backed HAL writes each 48 KB RAM plane in about `11 ms`; visible refresh time is dominated by panel BUSY. Full-screen black/white probe timings are useful for safety checks, but they are not the same as real text page-turn quality because normal page turns use differential RAM state and the controller's refresh policy.

   App integration status: `murphy_m4` now builds with the M4 display pins, X4 display model, 2-bit grayscale capability enabled, and partial refresh enabled. The regular app can render the `SD card error` screen; the remaining display-side check is that the normal app no longer falls into refresh loops while storage remains unresolved.

5. SD card and storage bring-up

   Identify the Murphy SD wiring and storage mode without relying on full app startup. Keep app1 flash logging as the primary diagnostic path until storage mounts reliably.

   Exit criteria: SD/card storage mounts under custom firmware; root listing and a small read/write sanity test succeed; the confirmed pin map and bus mode are reflected in the board profile or storage HAL; failure paths do not trigger display refresh loops.

   Current result: minimal `murphy_m4_probe` firmware now avoids the full app and avoids e-ink refreshes while probing storage. It tried the mined SD_MMC candidates `CLK=16`, `CMD=15`, `D0=17`, plus 4-bit variants using `18`, `11`, and `14`, with GPIO10/GPIO21 power-enable candidates. No tested SD_MMC combination mounted yet. GPIO10 passively idles high and should not be driven low; GPIO21 stayed low even when selected as an enable candidate. Artifacts include `test/murphy-m4-baseline/murphy_m4_minimal_display_sd_probe_flash_log_readback.bin` and `test/murphy-m4-baseline/murphy_m4_minimal_display_sd_power_hiz_probe_flash_log_readback.bin`.

6. Physical button input

   Map Enter, Next, and Lock/Back to existing logical actions while treating Reset as hardware-only. Keep all primary navigation reachable by buttons before adding touch.

   Exit criteria: file browser, menus, page turns, selection, back/lock behavior, and recovery into download mode remain usable without touch.

7. Japanese reader validation

   Validate the existing reader on the Murphy viewport before adding Murphy-only features. This is the first real product milestone, because display and input are only useful if Japanese EPUB semantics survive.

   Exit criteria: horizontal and vertical EPUBs render correctly; ruby/furigana survives parse, cache, layout, and render; dictionary cursor geometry follows logical text order; SD fonts and cache keys include board/layout metrics; bookmarks and page progression remain metadata-driven.

8. Touch input

   Add FT6336U as an optional input provider that emits activity-level taps and logical actions. Touch should improve navigation and dictionary cursor placement, not replace the button workflow.

   Exit criteria: list row taps, reader tap zones, menu taps, and dictionary cursor placement work with calibrated coordinates; all primary flows still have button equivalents; touch calibration lives in the board profile.

9. Two-channel frontlight

   Add warm/cool frontlight as a board capability with off-by-default settings and explicit shutdown behavior. Brightness and color temperature should be stored separately from reader layout and hidden on boards without frontlight.

   Exit criteria: brightness and color temperature can be changed safely, persisted, restored at boot, turned off before sleep/update/shutdown, and disabled cleanly on non-frontlight boards.

10. Power, battery, and sensors

   Identify and implement battery, charger, wake sources, RTC, buzzer, BMI270, and SHT40/AHT20 only where hardware evidence exists. Power UI should degrade gracefully when a chip is absent or not yet understood.

   Exit criteria: sleep/off paths release display, storage, radios, touch, and frontlight; charging/battery data is shown only when reliable; wake sources are explicit; unknown sensors remain disabled.

   Note: Murphy Cloud firmware confirms temperature/humidity support via an SHT40 path plus AHT20-style probe/fallback on shared I2C plumbing. Battery telemetry strings exist, but the low-level battery sense or gauge path still needs to be identified before enabling battery UI.

11. Optional accelerators and Murphy features

   Add PSRAM-backed caches, screenshots, USB MSC, BLE remote/ring, dashboard widgets, and Murphy package ideas only after the Japanese reader works without them. These should be capability-gated accelerators or affordances, not dependencies.

   Exit criteria: no-PSRAM behavior still passes; optional caches are evictable and rebuildable; USB/BLE/dashboard features do not change reader cache semantics or Japanese layout behavior.

## Confirmed Murphy M4 Hardware Facts

The received unit entered ESP32-S3 download mode using the public Murphy sequence: hold `Lock/Back`, briefly press `Reset`, keep holding `Lock/Back` for about one second, then release it.

Read-only probes from Fedora Linux found:

| Item | Result |
| --- | --- |
| USB VID:PID | `303a:1001` |
| USB description | `USB JTAG/serial debug unit` |
| Linux serial path | `/dev/ttyACM0` |
| Chip | ESP32-S3 revision v0.2 |
| Features | WiFi, BLE, 8 MB embedded PSRAM |
| Crystal | 40 MHz |
| Factory MAC / USB serial | `48:ca:43:a4:b1:8c` |
| Flash manufacturer/device | `c8:4018` |
| Flash size | 16 MB |
| Flash eFuse type | Quad, 4 data lines |
| Secure boot | Disabled |
| Flash encryption | Disabled |
| Download mode | Enabled |
| USB serial/JTAG | Enabled |
| Hardware JTAG disable | False |

Full flash backup:

| Item | Result |
| --- | --- |
| Artifact | `test/murphy-m4-baseline/murphy-m4-full-flash-20260607.bin` |
| Size | `16777216` bytes |
| SHA-256 | `b68c4d8a911d57c97ac238990c6f08d00efb27c352d97076ba52601af68e2a1e` |
| Active app slot | `app0` |
| Inactive app slot | `app1`, blank/`0xff` |
| Stock app project | `arduino-lib-builder` |
| Stock app version | `esp-idf: v4.4.7 38eeba213a` |
| Stock app compile time | `Mar  5 2024 12:12:53` |

Actual partition table read from this unit at `0x8000`:

| Name | Type | Subtype | Offset | Size |
| --- | --- | --- | --- | --- |
| `nvs` | data | nvs | `0x009000` | `20K` |
| `otadata` | data | ota | `0x00e000` | `8K` |
| `app0` | app | ota_0 | `0x010000` | `6976K` |
| `app1` | app | ota_1 | `0x6e0000` | `6976K` |
| `spiffs` | data | spiffs | `0xdb0000` | `2M` |
| `coredump` | data | coredump | `0xff0000` | `64K` |

Important implication: this unit uses `app0` at `0x10000`, not the public Murphy Cloud `0x20000` app offset. Always read the target device's partition table before flashing, and never write an app image at `0x0`.

## GPIO Findings

| Subsystem | Signal | GPIO | Status | Evidence / Notes |
| --- | --- | --- | --- | --- |
| E-paper display | SCK / CLK | `4` | Confirmed | Mined from Murphy `1.2.4` display-constructor tuple, repeated in `1.2.8`/`1.2.11`; visibly validated by full-frame bar/white probes. |
| E-paper display | MOSI / DIN | `3` | Confirmed | Same display tuple and full-frame validation. |
| E-paper display | CS | `5` | Confirmed | Same display tuple and full-frame validation. |
| E-paper display | DC | `6` | Confirmed | Same display tuple and full-frame validation. |
| E-paper display | RST | `7` | Confirmed | Same display tuple; electrical probe showed `BUSY=8` responding while `RST=7` was held low/released; full-frame validation. |
| E-paper display | BUSY | `8` | Confirmed | Same display tuple; electrical reset response and measured BUSY-high intervals during LUT/update probes. |
| E-paper display | MISO | None / `-1` | Confirmed for display SPI | Murphy Cloud `1.3.0` SPI setup showed no display MISO; probes use write-only SPI. |
| E-paper display | RAM addressing | X `0..799`, Y `479..0` | Confirmed behavior | Not GPIO, but critical display wiring behavior: X4-style pixel-addressed X plus reversed Y is required for correct full-frame content. |
| SD / storage | CLK | `16` | Likely | Stock-firmware mining found an SD_MMC-style tuple around `16,15,17,18,11,14`; not yet mounted on hardware. |
| SD / storage | CMD | `15` | Likely | Same SD_MMC tuple; not yet hardware-validated. |
| SD / storage | D0 | `17` | Likely | Same SD_MMC tuple; not yet hardware-validated. |
| SD / storage | D1 / D2 / D3 or drive pins | `18`, `11`, `14` | Candidate | Same tuple, but exact SD bus width and role split still need validation. |
| Touch | I2C SDA | `13` | Likely | Firmware/string mining points to FT6336U on shared I2C plumbing; pin tuple inferred from cross-version analysis, not yet hardware-validated. |
| Touch | I2C SCL | `12` | Likely | Same touch/I2C evidence; not yet hardware-validated. |
| Frontlight | Cool / warm channels | `47`, `48` | Likely on newer firmware | Murphy Cloud `1.3.0` frontlight evidence anchors `GPIO47`/`GPIO48`; polarity/frequency/channel order still unknown. |
| Frontlight | Cool / warm channels | `38`, `39` | Historical/conflicting candidate | Older `1.2.4` code has a `Frontlight ready: cool=GPIO%d warm=GPIO%d` neighborhood using `GPIO38`/`GPIO39`; reconcile before enabling. |
| Buzzer | PWM / LEDC | `46` | Firmware clue | Public changelog mentions buzzer on `GPIO46` / LEDC channel 2; not yet hardware-validated. |
| Temp/humidity | I2C sensor path | TBD | Confirmed feature, pins unresolved | Murphy Cloud firmware confirms SHT40 plus AHT20-style fallback, likely on shared I2C, but low-level pins/address behavior still need probing. |
| Battery / charger | Sense or gauge path | TBD | Unknown | Battery telemetry strings exist, but no reliable GPIO/ADC/I2C gauge mapping yet. |

## Still Unknown

These are the key facts that still need hardware evidence:

- PSRAM speed and stable allocation budget.
- Display controller variant, rail sequencing details, partial-refresh limits, and grayscale behavior. Full-frame content is now visibly validated with `SCK=4`, `MOSI=3`, `CS=5`, `DC=6`, `RST=7`, `BUSY=8`, X4-style pixel-addressed X `0..799`, reversed Y `479..0`, the X4 factory quality LUT, and `0x22/0xf4`.
- Touch I2C pins, interrupt pin, reset pin, FT6336U address, coordinate transform, and calibration offsets.
- Warm/cool frontlight PWM pins, frequency, polarity, driver topology, safe duty range, and shutdown requirements.
- SD wiring and whether stock firmware uses external SD, internal SPIFFS, USB MSC, or a mix.
- Battery gauge, charger IC, USB/charging detection, RTC, wake-capable GPIOs, and hardware power-off behavior.
- Whether the stock firmware exposes diagnostics/API data that reveal board capabilities.

## Next Read-Only Probes

- Capture normal boot serial logs from reset without forcing download mode. Boot logs may print PSRAM detection, board profile names, display/touch/frontlight init messages, I2C probe results, and active partition metadata.
- Read a full flash backup and decode bootloader, OTA data, app descriptors, coredump area, NVS, and SPIFFS offline. SPIFFS may contain settings, calibration, diagnostics, or cached hardware facts absent from the OTA image.
- If stock firmware can join Wi-Fi or enter a USB/transfer mode, query local diagnostics and settings APIs before replacing it. Embedded strings suggest diagnostics, screenshot, settings, font, OTA, and EPUB capability endpoints.
- Exercise stock frontlight and touch settings, then compare saved settings or diagnostics. This may reveal warm/cool ranges, touch calibration, refresh settings, and whether settings live in NVS or SPIFFS.

Avoid GPIO brute-force probing on the e-paper or frontlight until a full backup exists and display power sequencing is understood. Wrong rail or PWM assumptions can stress the panel or frontlight hardware.

## Reference Summary

### Current Japanese Branch

Use this branch as the semantic base. Do not replace its Japanese EPUB parser, writing-mode handling, ruby/furigana layout, tategaki renderer, dictionary cursor geometry, SD-backed dictionaries, `.cpfont` font path, or bounded-memory cache model.

The existing ESP32-C3 X3/X4 devices remain the compatibility baseline. Murphy can add capabilities, but touch, frontlight, PSRAM, RTC, and a large display must not become required for the Japanese reader to work.

### X4 Display Clues

The current X4 path targets an `800x480` GDEQ0426T82-style e-paper display with SSD1677 SPI behavior, BW/RED RAM differential refresh, and custom LUTs. Murphy firmware strings and changelog entries repeatedly point to an `800x480` GDEQ display class and window partial refresh.

Binary mining of the Murphy app found an exact match for this branch's short X4 differential grayscale LUT fingerprint, but not for the checked X4 factory fast/quality LUT fingerprints. This is strong evidence for a shared display family, not proof of identical panel module, GPIO map, init sequence, or waveform policy.

### Murphy Firmware Evidence

The local Murphy OTA image `test/murphy-26-0604-1.2.23.bin` is an ESP32-S3 app image, not a full flash dump. It contains project strings `Murphy version: 1.2.23`, `PandaR-ESP32-1.2.23`, `Murphy-Reader`, build date `2026-03-31 11:43:43`, ESP-IDF `v5.5.4`, and a private build identifier `14a0af9`.

Strings indicate a CrossPoint-derived private fork with source paths such as `/lib/hal/HalGPIO.cpp`, `/lib/hal/HalPowerManager.cpp`, `/lib/hal/HalStorage.cpp`, `/lib/GfxRenderer/GfxRenderer.cpp`, `/src/activities/ActivityManager.cpp`, and `CrossPoint/.mofei/wifi.bin.bak`.

Useful firmware clues:

- Storage root changed from `/.crosspoint` to `/.mofei`.
- Mofei HAL surfaces mention display framebuffer, input, touch, SD_MMC, light sleep, diagnostics, and power handling.
- Display strings include `Mofei frameBuffer`, `Mofei Gray4 Quality`, `Mofei busy timeout`, `fast lut`, `du lut`, `gray4 quality lut`, and `window partial gdeq`.
- Touch strings mention `MofeiTouch`, `FT6336U`, touch validation/configuration, and touch queues.
- Frontlight strings and public changelog point to separate warm/cool channels and a two-row brightness/color-temperature UI.
- Changelog mentions buzzer on GPIO46 / LEDC channel 2, BMI270 probing, and SHT40/AHT20 temperature/humidity support.
- BLE strings cover remote/ring input, HID, file transfer, Find My behavior, and ANCS iPhone notifications.
- Local API strings include diagnostics, settings reset, screenshot, OTA start, font install/reload/delete, OPDS, recent books, EPUB package capabilities, and study reports.
- Reader package strings include `MURPHY_EPUB_DERIVED`, `/derived.murphypkg`, `/book.bin`, `/sections`, and cover/image/text chunk names.

Do not treat the OTA image as source. It gives strings, metadata, and architectural clues, but not reliable GPIO maps, display init tables, charger behavior, or panel timing.

### Public Murphy Cloud Evidence

The public Murphy site and guide expose user-facing device facts: four side controls labeled Enter, Next, Lock/Back, and Reset; long-press Enter enters USB transfer mode; Lock/Back plus Reset enters flashing mode; Settings includes diagnostics, update checks, BLE, frontlight cool/warm, sleep wallpaper, sunlight fading fix, refresh frequency, text anti-aliasing, orientation, OPDS, and KOReader Sync.

The public Cloud manifest uses a different layout from the received M4: bootloader at `0x0`, partitions at `0x8000`, boot app at `0xf000`, and app at `0x20000`, with OTA slots at `0x20000` and `0x620000`. Treat public firmware metadata as one channel, not as proof of this unit's installed layout.

### External References

`t5s3-reader` is the closest CrossPoint-style S3 hardware-port reference. Use it for board profiles, touch abstraction, frontlight settings, battery/charger patterns, USB detection, and hardware-shutdown structure, but not as a Japanese reader replacement.

`M5Stack-Paper-S3-Chinese-Books` is useful for S3/PSRAM practices, CJK diagnostics, and font tooling ideas. Do not port its reader engine; it is Chinese-first, PSRAM-first, and does not preserve this branch's Japanese EPUB semantics.

## Capability Model

Prefer one board profile plus capability checks over scattered board conditionals.

Suggested Murphy-relevant fields:

| Capability | Meaning |
| --- | --- |
| `SOC_FAMILY` | ESP32-C3, ESP32-S3, etc. |
| `HAS_PSRAM` | External RAM exists; optional caches may use it |
| `PSRAM_CACHE_BUDGET_BYTES` | Conservative cache budget after display/Wi-Fi/runtime overhead |
| `DISPLAY_WIDTH` / `DISPLAY_HEIGHT` | Native panel scan dimensions |
| `VISIBLE_WIDTH` / `VISIBLE_HEIGHT` | Logical UI viewport dimensions |
| `DISPLAY_GRAYSCALE_BITS` | 1-bit, 2-bit, 4-bit, etc. |
| `DISPLAY_PARTIAL_REFRESH` | Partial refresh exists and limits are known |
| `DISPLAY_SINGLE_BUFFER_REQUIRED` | Framebuffer strategy constraint |
| `INPUT_BUTTON_COUNT` | Physical navigation button count |
| `INPUT_HAS_TOUCH` | Enables tap targets and touch-specific menus |
| `TOUCH_CONTROLLER` | FT6336U, GT911, none, unknown |
| `HAS_FRONTLIGHT` | Frontlight brightness can be controlled |
| `FRONTLIGHT_CHANNELS` | One brightness channel or warm/cool channels |
| `HAS_RTC` | Device can keep wall time without network |
| `HAS_BATTERY_GAUGE` | Battery percentage/current can be read |
| `HAS_CHARGER_CONTROL` | Firmware can detect USB/charging or request shutdown |
| `SD_REQUIRED` | Reader requires SD for books/fonts/dictionaries/caches |

## Cache And Test Checklist

- Cache keys include board viewport, writing mode, font identity/version, font size, line spacing, ruby settings, and all layout-affecting display metrics.
- Ruby sidecars remain lazy and absent when not needed.
- Vertical geometry remains page/block-local, bounded, and logical-order aware.
- SD fonts work without PSRAM.
- Dictionary lookup has streaming/index fallback.
- Image decoders keep bounded-memory paths.
- Web server, firmware update, and USB MSC paths can free reader/font caches before taking over storage.
- Touch-specific actions have button/menu equivalents.
- Frontlight, RTC, battery, touch, and sensor settings are hidden or no-op on boards without those capabilities.
- Japanese EPUB tests are rerun after any display, cache-key, font, or layout change.
