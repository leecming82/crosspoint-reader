# ESP32 Device Migration Notes

Date: 2026-06-03

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

## Hardware Snapshot

This table is a migration comparison, not a replacement for board schematics or panel datasheets. It records the useful implementation-level facts known from this branch and the inspected references.

| Device/project | Source | SoC/memory posture | Display | Display driver path | Input | Frontlight | RTC/time | Battery/power | Migration implication |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| X3 / current branch | Local repository | ESP32-C3, no PSRAM baseline | `792x528` e-paper | `EInkDisplay` X3 mode, UC81xx-class commands, X3 LUT/resync/grayscale policy | Physical buttons | No | No dedicated RTC assumed | Board-specific sleep/power path | Strict low-memory baseline; Japanese support must remain viable here |
| X4 / current branch | Local repository | ESP32-C3, no PSRAM baseline | `800x480` GDEQ0426T82-style e-paper | `EInkDisplay` SSD1677 SPI path, BW/RED RAM differential refresh, custom LUTs | Physical buttons | No | No dedicated RTC assumed | Board-specific sleep/power path | Similar low-memory baseline with different panel/controller behavior from X3 |
| M5Stack Paper S3 Chinese Books | <https://github.com/yuleshow/M5Stack-Paper-S3-Chinese-Books> | ESP32-S3 with PSRAM, PSRAM-first implementation style | `540x960` Paper S3 e-paper, grayscale-oriented | M5Stack/M5Unified/M5GFX-style board/display support | Touch-first | Board/model dependent; not treated as portable here | Board/model dependent | M5Stack board support patterns | Good S3/touch/PSRAM reference, but not a Japanese reader base |
| LilyGo T5S3 / `t5s3-reader` | <https://github.com/ShallowGreen123/t5s3-reader> | ESP32-S3, 16 MB flash, 8 MB PSRAM in referenced board notes | ED047TC1 4.7-inch e-paper, physical `960x540`, logical `540x960`, 16 gray | Custom T5S3 `HalDisplay` plus M5GFX `Panel_EPD`, TPS65185 rail sequencing, PSRAM-backed sprite/canvas | Buttons plus GT911 touch | Yes, PWM brightness setting | PCF85063 listed in board pins; app uses capability-specific clock behavior only where implemented | BQ25896 charger, BQ27220 gauge, USB detect, touch wake, hardware shutdown fallback | Closest CrossPoint-style S3 hardware-port reference; still lacks this branch's Japanese features |

## What Must Not Regress

Japanese support is the migration anchor. Before a new board is considered viable, verify:

- Ruby/furigana survives parse, cache serialization, layout, rendering, cursor movement, and dictionary lookup.
- Vertical and horizontal writing modes remain separate from physical screen orientation.
- Page progression direction follows EPUB metadata, not folder/category/device defaults.
- Dictionary hit geometry follows logical text order and works in vertical pages.
- Cache keys include all layout-affecting metrics: viewport, writing mode, font identity/version, font size, line spacing, ruby settings, and board display metrics.
- Low-memory paths still work when PSRAM is missing, disabled, fragmented, or reserved by display/WiFi.

Avoid replacing this branch's structured Japanese reader with another project's simpler EPUB path. The M5Stack project strips EPUB HTML to text and is Chinese-first. `t5s3-reader` is closer architecturally, but it does not include this branch's Japanese dictionary, writing-mode, ruby, vertical layout, CJK UI fallback, or SD `.cpfont` system.

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

### Displays

Port display first, then validate Japanese layout:

- Bring up menus, horizontal EPUB/TXT pages, images, and screenshots before testing vertical Japanese.
- Keep physical scan geometry separate from logical UI viewport.
- Include display size, margins, grayscale depth, and refresh mode in board metrics.
- Keep the renderer's public coordinate model stable where possible.
- Do not copy fixed `540x960` or `960x540` constants outside a T5S3 board profile.

Good reference: `t5s3-reader` uses M5GFX for the T5S3 e-paper panel, with PSRAM-backed panel sprites, grayscale conversion planes, TPS65185 rail sequencing, and page-turn sweep effects.

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

## Porting Sequence

1. Add board profile and capability definitions.
2. Port storage, display, input, power, and clock HALs with a minimal UI smoke test.
3. Verify existing horizontal reader flows: file browser, EPUB/TXT open, images, menus, cache, sleep/resume.
4. Add optional capability UI: touch taps, frontlight setting, RTC clock setting, battery screen, button remap.
5. Validate Japanese EPUB behavior: writing mode, vertical layout, ruby, dictionary cursor, bookmarks/navigation, cache reuse.
6. Add PSRAM accelerators only after no-PSRAM behavior passes.
7. Add board-specific tests or fixtures for viewport/cache-key changes.

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
