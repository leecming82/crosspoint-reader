# ESP32 Device Migration Comparison

Date: 2026-05-25

This note compares three Japanese/CJK-oriented reader lines for future migration work:

- `japanese-dictionary-fusion`: this branch.
- [`crosspoint-jp/master`](https://github.com/zrn-ns/crosspoint-jp): the Japanese-focused Crosspoint fork by `zrn-ns`.
- [`yuleshow/M5Stack-Paper-S3-Chinese-Books`](https://github.com/yuleshow/M5Stack-Paper-S3-Chinese-Books): a Chinese-focused ESP32-S3/M5Stack Paper S3 project inspected locally at `/tmp/M5Stack-Paper-S3-Chinese-Books`.

The focus is migrating this branch to other ESP32 devices, including ESP32-S3 devices with PSRAM, while keeping the current X3/X4 paths viable.

## Current Snapshot

| Line | Ref inspected | Primary target | Main character |
| --- | --- | --- | --- |
| This branch | `japanese-dictionary-fusion` at `1af91186` | X3/X4 Crosspoint targets, with some X3-only features and no PSRAM assumed | Japanese EPUB support layered onto current Crosspoint with tight SRAM/flash constraints |
| crosspoint-jp | [`crosspoint-jp/master`](https://github.com/zrn-ns/crosspoint-jp) at `bdaa0a38` | ESP32-C3 family, broader Japanese fork | Larger Japanese product fork with UI, fonts, Aozora, direction settings, cache generation, and web/device additions |
| M5Stack Paper S3 Chinese Books | [`yuleshow/M5Stack-Paper-S3-Chinese-Books`](https://github.com/yuleshow/M5Stack-Paper-S3-Chinese-Books) clone at `/tmp/M5Stack-Paper-S3-Chinese-Books`, commit `9afb344` | ESP32-S3 M5Stack Paper S3, 540x960 4-bit e-paper, OPI PSRAM | Chinese vertical TXT/EPUB reader plus calendar/weather/Cangjie/tools, built around PSRAM and touch |

`crosspoint_jp_integration_status.md` is useful historical context for this branch, but it is not guaranteed to describe the newest branch state exactly. Treat it as a migration log plus checklist, not as the only source of truth.

## Feature Shape

### This Branch

This branch is the best source for Japanese EPUB semantics while preserving Crosspoint's no-PSRAM architecture:

- Japanese dictionary support in `lib/JapaneseDictionary` and `docs/japanese-dictionary.md`.
- Ruby/furigana parsing and sidecar storage through `ChapterHtmlSlimParser`, `ParsedText`, `TextBlock`, and section serialization.
- EPUB writing-mode handling through `EpubWritingMode`, CSS `writing-mode` parsing, and OPF `page-progression-direction`.
- Native vertical layout through `ParsedText::layoutAndExtractVerticalColumns`, `TextBlock` vertical geometry, and `VerticalTextUtils`.
- Tategaki rendering primitives in `GfxRenderer`, including upright CJK, sideways Latin, and tate-chu-yoko.
- Japanese cursor/dictionary geometry that follows logical text order rather than visual-only order.
- CJK UI font fallback headers generated from sparse codepoint sets.
- SD-card cpfont format updates, including v5 vertical substitution metadata.
- Memory-oriented tategaki chunking and prewarm changes from recent commits.

Migration implication: keep this as the semantic and architectural baseline. It has the Japanese reader behavior you want, and it is already shaped around bounded SRAM, SD-backed data, and section caches.

### crosspoint-jp

The fork is broader and more productized around Japanese use:

- Japanese i18n and UI work.
- CJK font documentation and bundled/generated font assets.
- External font system experiments under `lib/ExternalFont`.
- Aozora Bunko device/API planning and activities.
- Direction and line-spacing settings.
- Reading status icons and marked-read UX.
- Batch EPUB cache generation.
- Table row/block experiments.
- Sleep/calendar and web UI additions.
- Dev/release workflow changes.
- Several Japanese reader plans/specs under `docs/superpowers`.

Migration implication: use crosspoint-jp as a feature quarry and test corpus. It is closer to this branch than the M5Stack project in EPUB/Japanese intent, but the fork diff is large. Prefer porting isolated features or UX patterns over merging broad subsystems.

### M5Stack Paper S3 Chinese Books

The M5Stack project is a strong reference for ESP32-S3/PSRAM device pragmatics, but it is Chinese-first rather than Japanese EPUB-first:

- PlatformIO target is ESP32-S3 with `BOARD_HAS_PSRAM`, OPI PSRAM, M5Unified, OpenFontRender, and a 540x960 Paper S3 display.
- TXT/EPUB support uses vertical CJK columns, mostly tuned for Chinese text.
- EPUB path extracts ZIP entries and strips HTML to text; it does not preserve ruby/furigana semantics.
- Layout mode is mostly folder/category driven, for example English horizontal versus Chinese vertical.
- Font path supports TTF/TTC/OTF and pre-rendered BIN fonts, with CJK filtering, glyph centering, and font preview tooling.
- Runtime rendering leans on PSRAM glyph caches, large EPUB text buffers, FreeType/OpenFontRender, sprites, and whole-file/image buffers.
- It includes Chinese-specific Cangjie input, simplified/traditional conversion tables, calendar/weather/shopping/todo tools, and touch-first UI.

Migration implication: do not port its reader engine as the Japanese base. Use it to learn S3 display/touch/PSRAM patterns, font conversion ideas, glyph centering, and practical vertical CJK rendering diagnostics.

## Japanese and CJK Feature Comparison

| Capability | This branch | crosspoint-jp | M5Stack Paper S3 Chinese Books |
| --- | --- | --- | --- |
| Primary language focus | Japanese EPUB reading | Japanese reading/product fork | Traditional Chinese reader/tools |
| Target memory model | X3/X4, no PSRAM baseline | ESP32-C3 family, no PSRAM baseline | ESP32-S3 with OPI PSRAM |
| Ruby/furigana | Yes, sidecar parse/layout/render path | Yes/plan history, broader fork context | No dedicated support found; HTML stripped to text |
| EPUB writing mode | Yes, CSS/OPF-derived and separate from physical orientation | Yes, with direction settings history | No meaningful EPUB writing-mode model found |
| Native tategaki | Yes, native columns and vertical geometry | Yes, broader vertical-text work | Yes for CJK grid-style vertical layout, Chinese tuned |
| Japanese kinsoku | Yes, kana/punctuation aware | Yes/broader work | CJK punctuation rules, Chinese tuned |
| Japanese dictionary | Yes, active branch focus | Not the same focus | No Japanese dictionary; Chinese Cangjie/dictionary tooling |
| UI language | Existing Crosspoint UI plus sparse CJK fallback | Japanese UI/i18n | Traditional Chinese UI |
| Font model | SD-card `.cpfont`, sparse CJK UI fallback, vertical substitutions | SD/CJK/external font experiments | TTF/TTC/OTF plus BIN fonts, FreeType/OFR, PSRAM glyph caches |
| EPUB parsing style | Structured parser, blocks, sections, cache | Structured Crosspoint-derived parser with fork additions | ZIP plus direct HTML-to-text stripping |
| Memory posture | Bounded, cache/streaming oriented | Broader feature set, still ESP32-C3-family aware | PSRAM-first and multi-MB-buffer friendly |
| Best things to borrow | N/A: baseline branch | Aozora, direction UX, cache generation, Japanese UI flows | S3 board config, touch/display ideas, font tooling, glyph centering |

## Device Memory Model

### Current X3/X4 Targets

The current branch targets both X3 and X4-style Crosspoint devices, with some features currently X3-only. The portable baseline should still assume no PSRAM. In practice, code should assume:

- Internal SRAM is scarce and fragmented under Arduino/IDF, display, SD, WiFi, parser, and decoder use.
- Stack, hot state, DMA-capable buffers, XML/parser buffers, image decoder scratch, display buffers, and font decompression buffers all compete for the same limited internal RAM class.
- "Free heap" can be misleading: the largest allocatable block and DMA-capable heap are often the real limits.
- Flash is large enough for firmware and generated tables only when carefully compressed or sparsely generated.
- SD card is the only realistic place for large books, dictionaries, fonts, and caches.
- Large single allocations are dangerous even when total free heap looks acceptable.

Keep these patterns:

- Stream ZIP/EPUB data instead of loading whole books.
- Use section caches and file-backed metadata.
- Prewarm only the needed font/glyph data.
- Make vertical/ruby vectors lazy and absent for horizontal/no-ruby pages.
- Avoid runtime TTF/FreeType unless it is strictly optional and not built for the X3/X4 baseline.

### ESP32-S3 With PSRAM

An ESP32-S3 port can be more forgiving, but should not make PSRAM mandatory for the core reader:

- Internal SRAM remains the right place for stack, hot state, DMA-capable buffers, and small scratch allocations.
- PSRAM can hold optional caches, larger image buffers, font preview caches, dictionary acceleration tables, or broader prefetch windows.
- Flash can hold more generated assets only if partitioning is revisited.
- SD remains the source of truth for books, dictionaries, and user fonts.

Recommended stance: use PSRAM as an acceleration tier, not as a correctness requirement. The reader should run in a bounded no-PSRAM mode, then enable larger caches on S3.

### Scaling Out To PSRAM

There is a viable path to "scale out" to PSRAM on S3-class boards, but only if internal SRAM remains the control plane.

Good PSRAM candidates:

- Large, cold, or retryable caches: glyph bitmaps, advance tables, dictionary hot pages, EPUB prefetch windows, image decode staging, web screenshot buffers.
- Data with an SD-backed source of truth: font pages, dictionary blocks, generated label caches, section cache staging.
- Optional accelerators that can be dropped under pressure.

Poor PSRAM candidates:

- Task stacks unless the framework/board support is proven and latency is acceptable.
- Interrupt-sensitive state.
- DMA buffers unless the specific peripheral and allocator support PSRAM DMA.
- Tiny hot structs touched in tight render loops.
- Parser/control state where cache latency would hurt more than it helps.

Recommended implementation shape:

- Introduce memory domains: `InternalRequired`, `DmaRequired`, `ExternalPreferred`, `ExternalOnlyOptional`.
- Add a small allocation wrapper or cache manager that tries PSRAM for optional caches and cleanly falls back to smaller SRAM/SD-backed behavior.
- Keep object metadata in SRAM and bulk payloads in PSRAM. For example, a glyph cache table can remain small and hot, while bitmap payloads sit in PSRAM.
- Make every PSRAM cache reclaimable. If WiFi, image decode, or font loading needs memory, the reader should be able to evict PSRAM caches and rebuild them from SD.
- Avoid changing serialized cache formats merely because PSRAM exists. PSRAM should change how much is prefetched, not what the book means.
- Track both total free memory and largest free block for each heap class.

For this branch, the first useful S3 accelerators would probably be:

- A larger SD-font glyph/advance cache.
- A Japanese dictionary hot-block cache.
- Larger image decode staging buffers.
- Optional section prefetch beyond the current page/section.
- Web/screenshot buffers that never exist on X3/X4.

### What M5Stack Assumes

The M5Stack project effectively assumes several megabytes of free PSRAM:

- EPUB chapter range loading allocates a text buffer as `freePsram - decompressionHeadroom`, capped at 4 MB.
- It refuses EPUB range loading when free PSRAM is not greater than decompression headroom plus 64 KB.
- Glyph bitmap cache uses a 192 KB PSRAM bitmap pool plus cache metadata.
- Screenshot capture allocates roughly 519 KB for a 540x960 BMP plus a chunk buffer.
- SD label cache can load a roughly 1.3 MB `labels.bin`.
- Cangjie and conversion tables are loaded into PSRAM.
- Runtime TTF/OpenFontRender paths use PSRAM sprites and glyph caches.

Migration implication: these are useful S3 accelerators but poor X3/X4 defaults. Anything inspired by M5Stack should have a no-PSRAM fallback.

## Migration Risk Areas

### Display and HAL

This branch and crosspoint-jp assume the Open-X4 SDK display/input/storage stack. M5Stack assumes M5Unified and the Paper S3 display/touch environment. Other ESP32 devices may differ in:

- E-paper controller.
- Display resolution and rotation model.
- Grayscale depth and update modes.
- Partial refresh support.
- Touch versus buttons.
- SD wiring and speed.
- Battery and sleep hardware.

Porting should start by keeping reader logic unchanged and adding a board profile/HAL adapter for display, input, storage, power, and clock. Avoid sprinkling board-specific conditionals through reader layout code.

### Viewport and Layout

The Japanese branch treats writing mode separately from physical orientation. Preserve that split:

- Physical orientation controls UI/status/menu viewport.
- EPUB writing mode controls text flow.
- Cache keys must include effective writing mode and any layout-affecting board metrics.

For new devices, make viewport metrics explicit:

- Screen width/height.
- Status bar height.
- Reader content rectangle.
- E-paper safe margins.
- Page-turn direction policy.
- Minimum usable column width/line height.

M5Stack's 540x960 layout constants and touch-first UI are useful examples, but should not become global defaults.

### Touch and Small Screens

M5Stack Paper S3 has a 540x960 touch panel, which sounds roomy compared with smaller e-paper devices but still behaves like a small screen because e-paper refresh is slow, text is large, and touch targets need generous hit areas.

Observed design/implementation patterns worth studying:

- A persistent bitmap toolbar in the reader with a small set of touch actions: font smaller/larger, size display, font menu, TOC, and bookmark.
- Large icon-driven navigation zones rather than dense menus.
- Touch-first book list and TOC pages with paginated rows.
- Mid-render touch polling in reader rendering, so long page draws can abort when navigation is requested.
- Separate English horizontal and Chinese vertical renderers, which keeps layout logic simpler for touch hit boxes.
- Inline EPUB link markers become touchable rectangles in the vertical renderer.
- Font preview/list UI renders samples using the real selected font, helped by PSRAM.
- Several features free PSRAM-heavy reader/font caches before preview or menu operations to reduce fragmentation.

Things to avoid copying directly:

- Folder/category-driven layout mode as a substitute for EPUB writing-mode metadata.
- Fixed 540x960 constants outside a board profile.
- PSRAM-only touch/web/screenshot buffers in code that should run on X3/X4.
- Touch-only navigation paths that do not map cleanly to button devices.

For a Crosspoint S3 migration, touch should be treated as an input capability layered over the existing reader model:

- Keep all primary actions reachable by buttons or menu commands where the board lacks touch.
- Define minimum touch target sizes in physical/UI metrics, not raw pixels only.
- Keep hit testing tied to rendered `PageLine`/`TextBlock` geometry, especially for vertical Japanese dictionary cursor and links.
- Prefer a board-profile toolbar/action-strip abstraction over M5Stack-specific toolbar constants.
- On e-paper, avoid interactions that require rapid hover/drag feedback; use tap zones, paginated lists, and modal panels.

### Fonts

Font migration is the sharpest edge.

Keep for all targets:

- SD-card `cpfont` format.
- Sparse CJK UI fallback generation.
- `vert`/`vrt2` substitution metadata for vertical punctuation.
- Per-font prewarm and bounded decompression.

Optional for S3:

- Larger font caches in PSRAM.
- More aggressive glyph metric prefetch.
- Runtime preview caches for font menus.
- Multiple ruby/body font variants if memory allows.
- M5Stack-style glyph centering and font coverage diagnostics.

Avoid making runtime TTF rendering a dependency. M5Stack's OpenFontRender path and crosspoint-jp's external font experiments are interesting, but this branch's generated/SD font architecture is much safer across X3/X4 and S3.

### Dictionaries

Japanese dictionary work should stay SD-backed and chunked:

- Index files belong on SD.
- Small lookup windows can live in SRAM.
- PSRAM can optionally hold hot dictionary pages or a compact term index.
- Cursor geometry must continue using logical text data, not visual-only text order.

For S3, the tempting path is to load a full dictionary index into PSRAM. Make that an optional accelerator with clear fallback.

### Images and EPUB Optimizer

crosspoint-jp has useful optimizer/X3 work and image handling changes. M5Stack has practical S3 image/screenshot paths that assume PSRAM. For other devices:

- Derive optimizer dimensions from board/display metrics.
- Keep decoder buffers bounded.
- Treat PSRAM image buffers as optional.
- Preserve low-memory decode paths for X3/X4.
- Include display bit depth in any optimizer profile.

### Web Server and Uploads

Web UI features can be memory expensive because HTML, JSON, uploads, WebDAV, and firmware flashing compete with reader caches.

For migration:

- Free reader/font caches before large uploads or firmware operations.
- Make JSON generation streaming where possible.
- Avoid multi-megabyte in-memory web payloads on X3/X4.
- Use PSRAM on S3 only behind allocation checks.
- Treat M5Stack's PSRAM JSON/screenshot buffers as S3-only patterns.

## Porting Strategy

1. Define board profiles.

   Add a small board/device descriptor rather than hard-coding dimensions and memory assumptions. Include SoC, display size, update modes, input map, flash size, PSRAM availability, SD path, and safe memory budgets.

2. Keep X3/X4 as the strict baseline.

   Any feature that cannot fit the X3/X4 baseline should be behind a capability check or separate environment. This keeps the branch from drifting into S3-only assumptions.

3. Split core reader from accelerators.

   Core reader: EPUB parse, section cache, writing mode, ruby, tategaki, dictionary lookup, SD fonts.

   Accelerators: PSRAM glyph cache, large dictionary hot cache, full-chapter prefetch, higher-resolution image caches, font previews, M5Stack-style screenshot buffers.

4. Make layout cache keys board-aware.

   Cache headers should include effective writing mode, viewport/content dimensions, font identity/version, font size, line spacing, ruby settings, and any board metrics that change pagination.

5. Port display HAL first, then reader.

   A new S3 target should first render existing horizontal pages, menus, and images. Only after that should vertical/ruby/dictionary paths be validated.

6. Use the other projects selectively.

   From crosspoint-jp, prefer Japanese UX/features and tests: Aozora flows, direction/line-spacing UX, cache pregeneration ideas, CJK font docs, reading-status UI.

   From M5Stack, prefer S3/device patterns: Paper S3 PlatformIO settings, touch/display assumptions, glyph centering, BIN font tooling, PSRAM cache patterns, and practical CJK vertical rendering diagnostics.

## Suggested Board Capability Flags

These are conceptual flags, not a required implementation:

| Capability | Meaning |
| --- | --- |
| `HAS_PSRAM` | External RAM exists and can be used for optional caches |
| `PSRAM_BUDGET_BYTES` | Conservative budget for caches after display/WiFi/runtime overhead |
| `DISPLAY_WIDTH` / `DISPLAY_HEIGHT` | Native panel dimensions |
| `DISPLAY_GRAYSCALE_BITS` | 1-bit, 4-bit, etc. |
| `DISPLAY_PARTIAL_REFRESH` | Partial update support and quality limits |
| `DISPLAY_SINGLE_BUFFER_REQUIRED` | Whether framebuffer memory must stay single-buffered |
| `INPUT_HAS_TOUCH` | Enables touch-specific UI affordances |
| `INPUT_HAS_SIDE_BUTTONS` | Enables button navigation assumptions |
| `SD_REQUIRED` | Whether reader can boot meaningfully without SD |
| `CAN_PREFETCH_GLYPHS` | Allows larger font metric/glyph prewarm |
| `CAN_PREFETCH_DICTIONARY` | Allows optional dictionary index/cache acceleration |

## What To Keep Portable

- Ruby sidecars must remain lazy.
- Vertical geometry must remain page/block-local.
- CJK layout chunking must remain bounded.
- SD fonts must remain usable without PSRAM.
- Dictionary lookup must have a streaming/indexed fallback.
- Image decoders must keep bounded-memory paths.
- Web server should not require reader caches to stay resident.
- Physical orientation and EPUB writing mode must remain separate.

## What Can Be S3 Enhanced

- Larger glyph and advance caches.
- Larger EPUB section prefetch windows.
- Dictionary hot-page cache.
- Faster font preview rendering.
- More generous image decode buffers.
- Optional full-book cache pregeneration UI.
- Touch-first controls on devices with touch panels.
- Higher-resolution generated UI assets where flash allows.
- M5Stack-style screenshot and web preview buffers when PSRAM is available.

## Immediate Follow-Up Checklist

- Audit current branch for any accidental X3/X4-hostile allocation growth in tategaki/ruby paths.
- Add a compact memory budget table to platform docs: X3 baseline, X4 baseline, S3 no-PSRAM, S3 PSRAM.
- Identify board metrics currently hard-coded in reader activities and move them behind a profile.
- Re-run Japanese EPUB tests after changing cache keys or board metrics.
- Decide whether crosspoint-jp's Aozora and direction settings belong in this branch before S3 migration.
- Decide whether any M5Stack font-conversion/glyph-centering tooling should be adapted to `.cpfont`.
- Keep `.cpfont` as the common font format unless an S3-only optional renderer is introduced.
