# ESP32 Device Migration Notes

Date: 2026-06-04

This note is about migrating this branch, especially its Japanese EPUB support, to other ESP32 devices. The other forks and device projects are references, not merge targets. Use them to understand hardware ports, UI ideas, memory tradeoffs, and feature gaps while keeping this branch's Japanese reader semantics as the baseline.

## Migration Goal

The portable core should remain:

- Japanese EPUB parsing with ruby/furigana sidecars.
- EPUB writing-mode handling from CSS and OPF metadata.
- Native vertical layout, tategaki rendering, punctuation handling, tate-chu-yoko, and logical cursor geometry.
- Japanese dictionary lookup with SD-backed data and bounded RAM use.
- SD-card `.cpfont` fonts, CJK UI fallback, vertical substitution metadata, and bounded glyph decompression.
- Section/page caches whose meanings do not change merely because a device has more memory.

Device-specific work should sit around that core:

- Display HAL and refresh behavior.
- Input capabilities: physical buttons, touch, power button, tilt, home key.
- Power hardware: charger, battery gauge, RTC, wake sources, sleep/off modes.
- Optional accelerators: PSRAM caches, larger image buffers, screenshot buffers, broader prefetch.
- Board UI affordances: frontlight controls, touch targets, button hints, orientation defaults.

The current ESP32-C3/button/no-frontlight device remains the strict compatibility baseline. New devices can add capabilities, but they should not make touch, frontlight, RTC, PSRAM, or a large screen required for the Japanese reader to work.

## References Inspected

| Project | Source | Ref inspected | Best use |
| --- | --- | --- | --- |
| This branch | Local repository | `japanese-dictionary-fusion` | Source of truth for Japanese EPUB semantics and no-PSRAM architecture |
| `M5Stack-Paper-S3-Chinese-Books` | <https://github.com/yuleshow/M5Stack-Paper-S3-Chinese-Books> | `yuleshow/M5Stack-Paper-S3-Chinese-Books` at `9afb344` | ESP32-S3/PSRAM/touch/display practices, Chinese vertical rendering diagnostics, font tooling ideas |
| `t5s3-reader` | <https://github.com/ShallowGreen123/t5s3-reader> | `ShallowGreen123/t5s3-reader` at `f6f869b5c3fee1dfe0a382882de2ca5b3b41d664` | CrossPoint-style LilyGo T5S3 hardware port: M5GFX display HAL, GT911 touch, frontlight, battery/charger, power handling |
| Murphy / Mofei firmware image | Local OTA image `test/murphy-26-0604-1.2.23.bin` | ESP32-S3 app image, Murphy `1.2.23`, built 2026-03-31, embedded app descriptor `14a0af9` | Evidence for a private CrossPoint-derived S3 product fork: Mofei hardware HAL, FT6336U touch, frontlight, buzzer, USB MSC, BLE, dashboard/study/weather features |
| Murphy Cloud public firmware site | <http://murphy.pandacat.ai/> | Fetched 2026-06-04: public routes, Next.js bundles, `/changelog.json`, `/manifest.json`, `/ota/latest`, `/ota/check`, `/version`, rollback manifests, `/flash`, `/firmware`, `/guide`, `/tools`, `/fonts`, `/images/device.jpg` | Public install metadata, partition layout, rollback image names, release notes, display/sensor/font clues, hardware button illustration, account/backup surface, and user-facing firmware support workflows |

## Hardware Snapshot

This table is a migration comparison, not a replacement for board schematics or panel datasheets. It records the useful implementation-level facts known from this branch and the inspected references.

| Device/project | Source | SoC/memory posture | Display | Display driver path | Input | Frontlight | RTC/time | Battery/power | Migration implication |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| X3 / current branch | Local repository | ESP32-C3, no PSRAM baseline | `792x528` e-paper | `EInkDisplay` X3 mode, UC81xx-class commands, X3 LUT/resync/grayscale policy | Physical buttons | No | No dedicated RTC assumed | Board-specific sleep/power path | Strict low-memory baseline; Japanese support must remain viable here |
| X4 / current branch | Local repository | ESP32-C3, no PSRAM baseline | `800x480` GDEQ0426T82-style e-paper target | `EInkDisplay` SSD1677 SPI path, BW/RED RAM differential refresh, custom LUTs | Physical buttons | No | No dedicated RTC assumed | Board-specific sleep/power path | Strict low-memory baseline for the same apparent Murphy display class, but not confirmed to be the same physical panel/module |
| M5Stack Paper S3 Chinese Books | <https://github.com/yuleshow/M5Stack-Paper-S3-Chinese-Books> | ESP32-S3 with PSRAM, PSRAM-first implementation style | `540x960` Paper S3 e-paper, grayscale-oriented | M5Stack/M5Unified/M5GFX-style board/display support | Touch-first | Board/model dependent; not treated as portable here | Board/model dependent | M5Stack board support patterns | Good S3/touch/PSRAM reference, but not a Japanese reader base |
| LilyGo T5S3 / `t5s3-reader` | <https://github.com/ShallowGreen123/t5s3-reader> | ESP32-S3, 16 MB flash, 8 MB PSRAM in referenced board notes | ED047TC1 4.7-inch e-paper, physical `960x540`, logical `540x960`, 16 gray | Custom T5S3 `HalDisplay` plus M5GFX `Panel_EPD`, TPS65185 rail sequencing, PSRAM-backed sprite/canvas | Buttons plus GT911 touch | Yes, PWM brightness setting | PCF85063 listed in board pins; app uses capability-specific clock behavior only where implemented | BQ25896 charger, BQ27220 gauge, USB detect, touch wake, hardware shutdown fallback | Closest CrossPoint-style S3 hardware-port reference; still lacks this branch's Japanese features |
| Murphy / Mofei device | Local OTA image plus Murphy Cloud public metadata | ESP32-S3, 16 MB flash; public partition table has two 6 MB OTA slots at `0x20000` and `0x620000`; PSRAM used by display/reader paths when available | Public site advertises `800x480` e-paper; changelog names Mofei QEMU `GDEQ0426T82` display and GDEQ partial refresh; embedded strings include `VATES-Q2826004J1`, still unresolved | Custom Mofei framebuffer/e-paper HAL, GDEQ/window partial refresh, foreground-only display update rule, previous-frame buffer for fast refresh | Public guide image shows four side buttons: Enter, Next, Lock/Back, Reset; firmware also has FT6336U touch, BLE remote/ring support, and BMI270 probe mentioned in changelog | Yes, separate brightness/color-temperature UI backed by cool/warm PWM paths | RTC not confirmed; firmware has trusted time/weather-clock flows, and SHT40/AHT20 sensor support is mentioned for weather clock | SD_MMC 1-bit, USB MSC exposure, buzzer on GPIO46/LEDC channel 2 per changelog, battery display setting, light sleep and wake mask support; charger/gauge chip unresolved | Best direct target for a Japanese CrossPoint S3 port once hardware arrives; likely shares the X4 display class, but exact panel/module and init sequence still need hardware confirmation |

## Murphy OTA Image Notes

The local Murphy image is an ESP32-S3 application image, not a full flash dump. It contains only the app payload expected at an app partition; it does not include the bootloader, partition table, NVS, or OTA metadata partitions.

Observed image metadata:

- File: `test/murphy-26-0604-1.2.23.bin`
- Size: `4,479,248` bytes.
- ESP image target: ESP32-S3.
- App descriptor: project `arduino-lib-builder`, version/hash-like string `14a0af9`, built `2026-03-31 11:43:43`, ESP-IDF `v5.5.4`.
- Product strings: `Murphy version: 1.2.23`, `PandaR-ESP32-1.2.23`, `Murphy-Reader`.
- Storage root changed from CrossPoint's `/.crosspoint` to `/.mofei`.
- The appended image SHA-256 trailer validates.
- Murphy Cloud `/ota/latest` reports current version `1.2.23`, build date `2026-06-04`, firmware size `4,479,248`, and `app_offset` `131072` (`0x20000`).
- Murphy Cloud `/manifest.json` is a WebSerial install manifest that writes `bootloader.bin` at `0x0`, `partitions.bin` at `0x8000`, `boot_app0.bin` at `0xf000`, and the app at `0x20000`.

The public `partitions.bin` decodes as:

| Name | Type | Subtype | Offset | Size |
| --- | --- | --- | --- | --- |
| `nvs` | data | nvs | `0x009000` | `0x006000` |
| `otadata` | data | ota | `0x00f000` | `0x002000` |
| `app0` | app | ota_0 | `0x020000` | `0x600000` |
| `app1` | app | ota_1 | `0x620000` | `0x600000` |
| `storage` | data | fat | `0xc20000` | `0x300000` |
| `cards` | data | fat | `0xf20000` | `0x0d0000` |
| `coredump` | data | coredump | `0xff0000` | `0x010000` |

Flashing implication:

- This image can likely be written with normal ESP serial flashing tools if the device exposes the ESP32-S3 ROM bootloader, but it should be treated as an app-partition image only.
- Do not flash this app image at offset `0x0`. For the public Murphy partition table, the first OTA app slot is `0x20000`, not this repo's current `0x10000`.
- PlatformIO's normal `pio run -t upload` flow flashes the firmware produced by the current project environment. It does not automatically flash an arbitrary third-party OTA file unless the environment is configured to use that file as the upload source.
- If the device already runs Murphy, the safer first test is the device's own OTA/SD firmware update path, because it will write the correct OTA slot and update boot metadata.
- Before replacing firmware, capture a full flash backup, partition table, bootloader, and OTA data using serial tools. The Murphy app depends on board-specific boot, partition, and storage assumptions that are not present in the OTA image.
- Murphy Cloud rollback manifests are full merged recovery images such as `firmware_V623_E426_TOUCH_recovery_merged.bin` written at offset `0x0`; the site warns that rollback overwrites bootloader, partition table, and app.

The embedded string `14a0af9` is not present in this local git clone, so it should be treated as a private fork commit or build identifier until the fork source is found.

Additional app-image string mining:

- Source/build clues point at a CrossPoint-derived private fork, not an unrelated firmware: the binary has source-path strings such as `/lib/hal/HalGPIO.cpp`, `/lib/hal/HalPowerManager.cpp`, `/lib/hal/HalStorage.cpp`, `/lib/GfxRenderer/GfxRenderer.cpp`, `/src/activities/ActivityManager.cpp`, and `/src/activities/ActivityManagerRenderBackend.cpp`, plus one explicit legacy path-like string `CrossPoint/.mofei/wifi.bin.bak`. The build also carries many `/Users/isaac/.platformio/...` dependency paths.
- The Mofei fork adds or renames board HAL surfaces around `MofeiInput`, `MofeiTouch`, Mofei SD_MMC, Mofei light sleep, Mofei display framebuffer, and Mofei diagnostics. These strings are not enough to recover function bodies, but they identify the modules to recreate or request from source.
- The app embeds local HTTP/API routes beyond this branch's basic status/files surface, including `/api/diagnostics`, `/api/diagnostics/activity-performance/reset`, `/api/books/recent`, `/api/epub/package/capabilities`, `/api/study/report`, `/api/settings/reset`, `/api/fonts/reload`, `/api/fonts/install-murphy`, `/api/fonts/delete`, `/api/opds/delete`, `/api/screenshot`, and `/api/ota/start`. It also embeds `Authorization`, Basic-auth strings, upload/download/mkdir/rename/move routes, and the hard-coded OTA URL `http://murphy.pandacat.ai/ota/latest`.
- Persistent storage is centered on `/.mofei`, with binary-plus-json settings/state files, diagnostics under `/.mofei/diagnostics`, sleep wallpaper assets under `/.mofei/sleep`, recent/bookmark/highlight/read-later folders, study-card files under `/.mofei/study`, arcade/dashboard state, `/.mofei/trusted_time.txt`, `/.mofei/wifi.*`, `/.mofei/koreader.*`, and OPDS settings. This is useful for preserving user data when migrating, but it should not replace this branch's Japanese dictionary/cache layout without a deliberate compatibility layer.
- Font handling is broader than this branch's `.cpfont` path. Strings include EPF font-pack enforcement, `/.mofei/fonts/i18n_zh_cn_ui_12.epf`, `/.mofei/fonts/i18n_zh_tw_ui_12.epf`, `/.mofei/fonts/notosans_tc_8.epf` through `18.epf`, TTF fallbacks, `xiaomi_tc_45x60.bin`, and toggles such as `use_builtin_i18n_ui_fonts`, `use_storage_i18n_ui_fonts`, and `use_raw_bin_reader_font`.
- Reader packaging includes `MURPHY_EPUB_DERIVED`, `/derived.murphypkg`, `/book.bin`, `/sections`, image/text chunk names, cover assets, and derived-package fallback strings. Treat this as a separate EPUB acceleration/package format until the fork source is available; do not let it change this branch's Japanese EPUB semantics.
- BLE support is more than a simple remote: app strings include BLE file-transfer queues, BLE HID report handling, a BLE management/supervisor task, Find My queue strings, and `BLE_ANCS` iPhone notification-client paths such as "bond with iPhone first", ANCS service discovery, and subscribe failure messages. These should be optional input/transfer/notification providers, not reader dependencies.
- Crash and diagnostics support is app-owned: strings include `CRASH_REPORT_START`, `CRASH_REPORT_END`, `CRASH_REPORT_SOURCE:*`, `/crash_report.txt`, panic-recovery-loop handling, activity power diagnostics, and latest diagnostics JSON.
- Display mining found the exact short X4 differential grayscale LUT fingerprint from this branch's `lut_grayscale` array inside the Murphy app image. The two factory X4 LUT fingerprints checked from `lut_factory_fast` and `lut_factory_quality` were not found as exact byte sequences. This strengthens the shared-X4-display-code hypothesis, but still does not expose the full display init sequence, GPIO map, controller timing, or all active waveforms.
- Still missing from strings: a numeric button/frontlight/display GPIO map, charger/fuel-gauge/RTC chip names, a clean panel datasheet identifier, and this branch's Japanese dictionary markers such as `JapaneseDictionary`, `jitendex`, `jmnedict`, `cpdict`, or `/.crosspoint/dicts`.

### Murphy Website Inspection

The public Murphy site is a Next.js app. Static route chunks expose the same public surface as the visible navigation: `/flash`, `/tools`, `/guide`, `/changelog`, `/changelog.json`, `/firmware`, `/fonts`, `/manifest.json`, `/rollback-manifest.json`, `/ota/latest`, `/ota/check`, `/version`, `/account`, `/dashboard`, `/backups`, `/privacy`, `/terms`, and auth routes. English `/en/...` routes returned 404 during inspection, except the standalone `/flash/en` page. Account, dashboard, and backups render public shells but private backup state requires Google sign-in or a verified token.

Website findings that matter for the hardware port:

- The guide describes the project documentation source as firmware, Murphy Cloud, Murphy Mate app source, and project documents, so public guide wording is useful but still not a substitute for board schematics or fork source.
- The public device illustration is available at `/images/device.jpg`; the flash page currently references `/flash/images/device.jpg`, which returned 404 during inspection.
- The illustration labels four side controls: `Enter`, `Next`, `Lock/Back`, and `Reset`. Long-press `Enter` enters USB transfer mode. `Lock/Back` returns from the current page and locks/unlocks from the main screen. `Reset` reboots the device.
- The illustrated download-mode sequence is not the generic `BOOT` label: hold `Lock/Back`, briefly press `Reset` while still holding `Lock/Back`, then release `Lock/Back`.
- The manual says `Power + Volume Down` takes screenshots and screenshots are saved in a `screenshots` folder. Confirm whether `Volume Down` is a logical label, a hidden/alternate hardware button, or a documentation mismatch when the device arrives.
- The manual says Murphy Mate can pair by QR, local Wi-Fi discovery, or BLE Wi-Fi provisioning. QR pairing carries device address and auth token; the on-device Settings has an `API Token` field used by Murphy Mate and the local HTTP API.
- The manual's Settings list exposes likely firmware setting names and board capabilities: `Device Diagnostics`, `Check Updates`, `BLE Settings`, `Frontlight Cool / Warm`, `Sleep Wallpaper`, `Sunlight Fading Fix`, `Refresh Frequency`, `Text Anti-Aliasing`, `Orientation`, `Show Hidden Files`, OPDS, and KOReader Sync.
- The firmware hub and backups pages distinguish daily update tools from destructive rollback/restore flows. Treat rollback/recovery as a full-device operation, not an OTA-equivalent app update.

## What Must Not Regress

Japanese support is the migration anchor. Before a new board is considered viable, verify:

- Ruby/furigana survives parse, cache serialization, layout, rendering, cursor movement, and dictionary lookup.
- Vertical and horizontal writing modes remain separate from physical screen orientation.
- Page progression direction follows EPUB metadata, not folder/category/device defaults.
- Dictionary hit geometry follows logical text order and works in vertical pages.
- Cache keys include all layout-affecting metrics: viewport, writing mode, font identity/version, font size, line spacing, ruby settings, and board display metrics.
- Low-memory paths still work when PSRAM is missing, disabled, fragmented, or reserved by display/WiFi.

Avoid replacing this branch's structured Japanese reader with another project's simpler EPUB path. The M5Stack project strips EPUB HTML to text and is Chinese-first. `t5s3-reader` is closer architecturally, but it does not include this branch's Japanese dictionary, writing-mode, ruby, vertical layout, CJK UI fallback, or SD `.cpfont` system. The Murphy firmware appears to include vertical/ruby/CJK support and a dictionary UI label, but the image does not expose this branch's Japanese dictionary implementation markers such as `JapaneseDictionary`, `jitendex`, `jmnedict`, `cpdict`, or `/.crosspoint/dicts`; keep this branch's dictionary path as the source of truth.

## Board Capability Model

Prefer a board profile plus capability checks over scattered board conditionals.

Suggested fields:

| Capability | Meaning |
| --- | --- |
| `SOC_FAMILY` | ESP32-C3, ESP32-S3, etc. |
| `HAS_PSRAM` | External RAM exists; optional caches may try it |
| `PSRAM_CACHE_BUDGET_BYTES` | Conservative cache budget after display/WiFi/runtime overhead |
| `DISPLAY_WIDTH` / `DISPLAY_HEIGHT` | Native panel scan dimensions |
| `VISIBLE_WIDTH` / `VISIBLE_HEIGHT` | Logical UI viewport dimensions |
| `DISPLAY_GRAYSCALE_BITS` | 1-bit, 2-bit, 4-bit, etc. |
| `DISPLAY_PARTIAL_REFRESH` | Partial/fast refresh exists and its quality limits are known |
| `DISPLAY_SINGLE_BUFFER_REQUIRED` | Framebuffer strategy constraint |
| `INPUT_BUTTON_COUNT` | Physical navigation button count |
| `INPUT_HAS_TOUCH` | Enables tap targets and touch-specific menus |
| `INPUT_HAS_HOME_TOUCH_KEY` | Dedicated touch/home key or gesture exists |
| `INPUT_HAS_TILT` | Tilt page-turn support exists |
| `HAS_FRONTLIGHT` | Frontlight/backlight brightness can be controlled |
| `HAS_RTC` | Device can keep wall time without network |
| `HAS_BATTERY_GAUGE` | Battery percentage/current can be read |
| `HAS_CHARGER_CONTROL` | Firmware can detect USB/charging or request hardware shutdown |
| `SD_REQUIRED` | Reader requires SD for books/fonts/dictionaries/caches |

Use these capabilities to decide behavior. For example, frontlight settings should be hidden or inert on the C3 device, touch controls should augment buttons where available, and PSRAM caches should be optional accelerators.

## Hardware Feature Guidance

### Touch Screens

Touch should layer over existing actions, not replace them:

- Keep all primary flows reachable by buttons or menu commands.
- Add `onTouchTap(x, y)` at the activity level, with default no-op behavior.
- Use tap zones in reader pages: previous, menu/dictionary/action, next.
- Let list screens map row taps to selection/open.
- Let touch button-hint zones inject existing logical button events.
- Tie EPUB link/dictionary hit testing to rendered `TextBlock`/page geometry, especially for vertical Japanese.
- Avoid interactions that require fast drag/hover feedback; e-paper is better with taps, paginated lists, and modal panels.

Good reference: `t5s3-reader` implements GT911 touch in the HAL, converts taps into activity callbacks, and can inject synthetic button taps from touch button-hint regions. It keeps the button workflow intact.

Murphy/Mofei evidence: the OTA image embeds `MofeiTouch`, `FT6336U`, touch queue handling, and FT6336U validation/configuration messages. Plan for FT6336U I2C touch first, not GT911, while keeping the touch abstraction controller-agnostic.

Murphy Cloud changelog confirms `MOFEI_TOUCH_OFFSET_X=5` and `OFFSET_Y=-8` calibration in version `1.2.3`, and mentions touch coordinate-direction fixes in later releases. Treat touch calibration as a board-profile field, not a hard-coded activity adjustment.

Murphy Cloud's public device illustration labels side controls as `Enter`, `Next`, `Lock/Back`, and `Reset`. Model these as logical actions first: `Enter` maps to confirm/select and long-press USB transfer mode, `Next` maps to next/page-forward/navigation, `Lock/Back` maps to back plus lock/unlock behavior, and `Reset` is a hardware reset path rather than a normal app action. The same illustration says download mode is entered by holding `Lock/Back`, briefly pressing `Reset`, then releasing `Lock/Back`; verify this on the received unit before assuming a visible BOOT button exists.

For cursor dictionary mode, do not rely on exact word taps as the primary interaction. On small e-paper touch screens, taps should place the dictionary cursor near a line, column, paragraph, or text block, then existing cursor controls should provide precise movement by logical text order. Touch devices can add large nudge/action zones for previous/next token, expand/shrink selection, lookup, and exit. For vertical Japanese, hit testing must use rendered `TextBlock`/ruby-aware geometry and snap to logical text offsets; raw visual glyph positions are not enough because ruby, vertical punctuation, sideways Latin, and tate-chu-yoko can make visible glyph placement diverge from lookup order.

### Fewer Or More Buttons

Treat hardware buttons as physical inputs mapped to logical actions:

- Logical actions: Back, Confirm/Menu, Previous, Next, Up, Down, PageBack, PageForward, Power.
- Physical buttons: board-specific count/order/pins.
- User mapping: optional, but useful when devices have awkward button placement.
- Long-press behavior should be capability-aware and documented per board.

For two-button devices, a practical minimum is:

- Short previous/next for lists and page turns.
- Long confirm/menu on one button.
- Long power/off or sleep on a dedicated power-capable button if hardware supports it.
- Touch or menu fallbacks where available.

For devices with more buttons, expose direct Back/Menu/Previous/Next and keep side-button swap/remap settings.

### Frontlight

Frontlight is a board capability, not a reader feature:

- Store brightness as `0..N`, where `0` means off.
- Apply brightness at boot after settings load.
- Turn it off before sleep, deep sleep, hardware shutdown, and firmware update if needed.
- Hide or disable the setting on boards without frontlight.
- Use perceptual/nonlinear PWM mapping if the hardware response is harsh.

Good reference: `t5s3-reader` uses a `backlightLevel` setting and `BoardT5S3::setBacklightLevel()`, with PWM duty derived from a squared level curve.

Murphy/Mofei evidence: the OTA image has separate `frontlightCool` and `frontlightWarm` settings and frontlight PWM failure strings. Murphy Cloud changelog version `1.2.21` describes a two-row frontlight UI: brightness and color temperature, with underlying conversion to cool/warm PWM. Model this as a two-channel frontlight capability instead of a single brightness value.

### RTC

RTC support can improve UX without becoming mandatory:

- Prefer RTC time for status bar clocks, reading history timestamps, screenshots, and cache metadata.
- Fall back to compile/build time, network sync, or monotonic timestamps when RTC is absent.
- Keep reader cache validity independent of wall-clock correctness.
- Add a clock-sync flow only when the board can store time meaningfully.
- Avoid waking frequently just to maintain UI clocks on e-paper.

This branch already has clock-related activities. A board profile should determine whether those settings are shown and whether RTC-backed time is available.

### Battery, Charger, And Sleep

Power behavior is highly board-specific:

- Separate "sleep with screen preserved", "deep sleep", and "hardware power-off" where the board supports them.
- Use wake sources from board capabilities: power button, touch interrupt, RTC alarm, charger/USB, etc.
- Detect USB/charging before auto power-off if possible.
- Do not assume a battery gauge exists; battery UI should degrade gracefully.
- Before sleep/off, release display power, frontlight, SD bus, radios, touch state, and optional caches.

Good reference: `t5s3-reader` integrates BQ25896/BQ27220, touch wake, USB detection, auto power-off when idle and unplugged, and a hardware-shutdown fallback to deep sleep.

Murphy/Mofei evidence: the OTA image embeds SD_MMC 1-bit mounting, USB MSC, buzzer, battery-level fields, Mofei light-sleep messages, and a button wake mask. Murphy Cloud changelog version `1.2.8` says the buzzer memory game uses GPIO46 and LEDC channel 2, and version `1.2.6` mentions SHT40/AHT20 temperature/humidity sensor support with multiple SHT40 I2C pin scan paths. It does not expose clear BQ/charger/RTC chip names. Treat battery percentage, USB/charging state, and hardware power-off as unknown until probed on-device.

### Displays

Port display first, then validate Japanese layout:

- Bring up menus, horizontal EPUB/TXT pages, images, and screenshots before testing vertical Japanese.
- Keep physical scan geometry separate from logical UI viewport.
- Include display size, margins, grayscale depth, and refresh mode in board metrics.
- Keep the renderer's public coordinate model stable where possible.
- Do not copy fixed `540x960` or `960x540` constants outside a T5S3 board profile.

Good reference: `t5s3-reader` uses M5GFX for the T5S3 e-paper panel, with PSRAM-backed panel sprites, grayscale conversion planes, TPS65185 rail sequencing, and page-turn sweep effects.

Murphy/Mofei evidence: the OTA image embeds `Mofei frameBuffer`, `Mofei Gray4 Quality`, `Mofei busy timeout`, previous-frame buffer fallback, and a foreground-only display update rule. Murphy Cloud advertises an `800x480` e-paper display, and changelog version `1.0.0` names Mofei QEMU simulator peripherals as FT6336U touch plus `GDEQ0426T82` e-paper display. Later releases mention `GDEQ0426T82` window partial refresh, GDEQ partial refresh cadence, dirty-window planning, and Mofei partial refresh policy. The image exposes display-mode strings including `fast lut`, `du lut`, `gray4 quality lut`, and `window partial gdeq`, so LUT-backed update paths are almost certainly compiled into the app. Byte mining found an exact match for this branch's short X4 differential grayscale LUT fingerprint, but not for the checked X4 factory fast/quality LUT fingerprints. This is strong evidence that Murphy targets the same display class as the X4 path, but it does not prove the same physical panel module, flex variant, full LUT set, or init sequence. The image also contains the string `VATES-Q2826004J1`, which may be a panel/module marker, but no public datasheet or controller match was confirmed during inspection. First hardware tasks are to dump boot logs, inspect the board/panel flex labels, and capture display init traffic or source code before assuming controller commands or resolution.

### PSRAM

PSRAM should be an acceleration tier, not a correctness requirement:

- Keep control state, small hot metadata, task stacks, and DMA-required buffers in internal RAM unless the board support is proven.
- Put large retryable payloads in PSRAM: glyph bitmap caches, dictionary hot pages, section prefetch, screenshot buffers, image staging.
- Make all PSRAM caches evictable and rebuildable from SD.
- Track largest free block, not just total free heap.
- If allocation fails, fall back to C3-style streaming/cache behavior.

Strong S3 candidates:

- Larger SD-font glyph and advance caches.
- Dictionary hot-block cache.
- Wider EPUB section prefetch.
- Larger image decode buffers.
- Screenshot/web preview buffers.
- Font preview caches.

Poor candidates:

- Required parser state.
- Interrupt-sensitive data.
- Tiny structs touched in tight loops.
- Anything that changes serialized cache semantics.

## Project-Specific Lessons

### This Branch

Use as the semantic and architectural base:

- Japanese dictionary and logical cursor geometry.
- Ruby/furigana parse/layout/render path.
- EPUB writing-mode and page-progression handling.
- Native vertical layout and tategaki rendering.
- SD `.cpfont`, sparse CJK UI fallback, vertical substitutions.
- Bounded SRAM, streaming EPUB/ZIP reads, SD-backed caches.

### M5Stack Paper S3 Chinese Books

Use for S3/PSRAM practices and CJK diagnostics:

- Large PSRAM buffers and glyph caches.
- Touch-first CJK reader UI ideas.
- Font conversion/preview/coverage tooling.
- Practical image, screenshot, and web buffer patterns.

Do not port its reader engine as the Japanese base. It is Chinese-first, PSRAM-first, and does not preserve ruby/writing-mode semantics.

### t5s3-reader

Use as the closest CrossPoint-style hardware-port reference:

- LilyGo T5S3 board profile and PlatformIO environment.
- M5GFX e-paper HAL for a `960x540` physical, `540x960` logical display.
- GT911 touch HAL and activity-level tap handling.
- Frontlight/backlight setting and PWM control.
- Battery gauge, charger control, USB detection, touch wake, hardware shutdown fallback.
- Button remapping and touch button-hint injection.

Do not treat it as a Japanese reader replacement. It lacks the Japanese support in this branch.

### Murphy / Mofei Firmware

Use as the most relevant product-fork reference once the physical device arrives:

- ESP32-S3 build target and 16 MB flash posture.
- Murphy partition table: OTA app slots at `0x20000` and `0x620000`, each `0x600000`; FAT partitions named `storage` and `cards`; coredump at `0xff0000`.
- Custom Mofei display/input/storage/power HALs, with source-path strings that line up with this repo's HAL/activity/GfxRenderer structure.
- A direct CrossPoint-origin clue: the app image contains `CrossPoint/.mofei/wifi.bin.bak`.
- FT6336U touch handling.
- Public guide image labels hardware controls as Enter, Next, Lock/Back, and Reset; the documented download-mode sequence uses Lock/Back plus Reset.
- `800x480` GDEQ0426T82 display target, with window partial refresh and dirty-window planning in release notes.
- Exact match for this branch's short X4 differential grayscale LUT fingerprint; no exact match found for the checked X4 factory fast/quality LUT fingerprints.
- Two-channel warm/cool frontlight control.
- SD_MMC 1-bit storage and USB mass-storage exposure.
- BLE remote/ring, BLE HID, BLE file transfer, Find My Device behavior, and ANCS/iPhone-notification client strings.
- Murphy Mate / local API pairing by QR, Wi-Fi discovery, BLE Wi-Fi provisioning, and API token are described in the public guide.
- Local HTTP/API surface includes diagnostics, settings reset, OTA start, screenshot, font install/reload/delete, OPDS, recent books, EPUB package capabilities, and study report endpoints.
- Derived EPUB package support appears under `MURPHY_EPUB_DERIVED` and `/derived.murphypkg`.
- Dashboard, study-card, weather-clock, word-cloud sleep wallpaper, and font-pack workflows.
- Device Diagnostics, screenshots, OPDS, KOReader Sync, OTA update checks, and dynamic settings are public manual features.
- BMI270 motion sensor probe and SHT40/AHT20 temperature/humidity sensor support are mentioned in release notes.
- Buzzer on GPIO46 / LEDC channel 2 is mentioned in release notes.
- Crash-report and diagnostics paths under `/.mofei`.

Do not use it as a source substitute. From the OTA image alone, the recoverable information is strings, image metadata, and broad architecture clues; it is not enough to reconstruct display init tables, GPIO maps, partition layout, charger behavior, or panel timing with confidence.

Likely useful feature deltas to port after Japanese reading works:

- USB MSC SD-card mode, gated behind safe unmount/remount handling.
- BLE remote/ring as an optional input provider that emits logical actions.
- Two-channel frontlight setting and sleep/off shutdown handling.
- Dashboard widgets only as optional home-screen affordances; do not let them change reader/cache semantics.
- Weather/time flows if they can reuse existing clock-sync settings and degrade cleanly offline.
- EPF/font-pack ideas only after deciding whether to keep `.cpfont` as the canonical format or add EPF as an import/install format. Murphy Cloud publishes 4-bit regular EPF reader font packs for Noto Sans TC 8/10/12/14/16/18pt, UI subset EPFs, and TTF/OTF files under `/.mofei/fonts/`.

## Porting Sequence

1. Add board profile and capability definitions.
2. Port storage, display, input, power, and clock HALs with a minimal UI smoke test.
3. Verify existing horizontal reader flows: file browser, EPUB/TXT open, images, menus, cache, sleep/resume.
4. Add optional capability UI: touch taps, frontlight setting, RTC clock setting, battery screen, button remap.
5. Validate Japanese EPUB behavior: writing mode, vertical layout, ruby, dictionary cursor, bookmarks/navigation, cache reuse.
6. Add PSRAM accelerators only after no-PSRAM behavior passes.
7. Add board-specific tests or fixtures for viewport/cache-key changes.

For the Murphy/Mofei device specifically:

1. Before flashing anything custom, back up the whole flash and save boot logs, partition table, bootloader, NVS, OTA data, and original app slots.
2. Confirm serial bootloader access and actual flash/PSRAM size with `esptool.py flash_id` and boot output.
3. Dump and decode the partition table; the public Murphy installer uses `app0` at `0x20000` and `app1` at `0x620000`, but confirm the received unit before flashing.
4. Capture board facts: panel/flex labels, touch controller address, I2C pins, SD pins, frontlight PWM pins, Enter/Next/Lock-Back/Reset button GPIOs, any Power/Volume-Down equivalent, buzzer pin, battery/charger IC markings, and wake-capable GPIOs.
5. Bring up a minimal Murphy board profile in this branch: S3 build, storage, framebuffer display, button input, then FT6336U touch.
6. Keep Murphy product features disabled behind capabilities until Japanese EPUB, dictionary, ruby, vertical writing, and SD fonts pass on the new viewport.

## Cache And Test Checklist

- Cache keys include board viewport and writing-mode-affecting metrics.
- Ruby sidecars remain lazy and absent when not needed.
- Vertical geometry remains page/block-local and bounded.
- SD fonts work without PSRAM.
- Dictionary lookup has streaming/index fallback.
- Image decoders keep bounded-memory paths.
- Web server and firmware update paths can free reader/font caches.
- Touch-specific actions have button/menu equivalents.
- Frontlight, RTC, battery, and touch settings are hidden or no-op on boards without those capabilities.
- Japanese EPUB tests are rerun after any display, cache-key, font, or layout change.

## Near-Term Actions

- Introduce a compact board capability descriptor before adding another device.
- Audit current hard-coded dimensions and input assumptions in reader activities.
- Keep `.cpfont` as the common font format; adapt only tooling from other projects unless an S3-only optional renderer is explicitly introduced.
- Prototype touch as an activity-level optional input, using existing logical actions as the bridge.
- Prototype frontlight as a capability-gated setting with no behavior on the current C3 target.
