# crosspoint-jp Integration Status

This branch is integrating selected Japanese reader features from `crosspoint-jp`
without merging the whole fork.

## Done

### Japanese System UI

- Sparse built-in CJK UI bitmap font stored in flash/`PROGMEM`.
- Translation-driven CJK UI glyph generation/checking.
- Display-only kana NFC normalization for filenames, metadata, authors, and chapter labels.
- Per-character tofu fallback for unsupported UI glyphs.
- Ideographic-space handling for UI display.
- EPUB body text remains on reader/built-in/SD fonts; CJK UI font is limited to UI text.

### Ruby Data Infrastructure

- `<ruby>`, `<rt>`, and `<rp>` are parsed without flattening readings into body text.
- Ruby is carried as lazy sidecar layout data through:
  - `ParsedText`.
  - `TextBlock`.
  - section serialization/cache.
  - horizontal proof rendering.
- Section cache version was bumped for the serialized ruby block.
- No-ruby paragraphs/blocks avoid allocating ruby vectors.
- Ruby survives the CJK chunked-layout fallback.
- On-device check with a Japanese EPUB confirmed:
  - Fresh rebuild parsed/rendered ruby pairs in a ruby-bearing spine.
  - Reopening loaded that spine from section cache.
  - Reader view showed ruby separately from body text instead of inline fallback parentheses.

### Native Vertical Text Foundation

- Writing-mode detection is in place:
  - CSS `writing-mode`, `-epub-writing-mode`, and `-webkit-writing-mode` are parsed.
  - OPF `page-progression-direction="rtl"` is parsed.
  - EPUBs resolve to a reader writing mode separate from device/system UI orientation.
- Reader profile plumbing is split from physical orientation:
  - The normal Orientation setting is again the physical reader/menu/status-bar orientation, matching the master approach.
  - Writing Mode is a separate preference: Book's Style, Horizontal, or Vertical RL.
  - Book's Style uses the EPUB's resolved writing mode; the explicit modes override the book for testing or preference.
  - The reader menu exposes both Orientation and Writing Mode; both currently update global reader settings, not per-book
    overrides.
  - Menu/status/system UI orientation is not inferred from Japanese/vertical writing mode.
  - Japanese dictionary cursor activation is gated by EPUB language metadata, not physical orientation; Japanese books can
    use cursor lookup in horizontal and vertical writing modes.
- Section cache plumbing includes writing mode:
  - The section cache version was bumped.
  - Section cache headers include resolved writing mode, preventing horizontal/vertical cache reuse.
  - Silent indexing and cache lookup use the same writing-mode key.
- Vertical rendering primitives exist for later layout integration:
  - `VerticalTextUtils` classifies upright CJK, kinsoku punctuation, sideways text, and tate-chu-yoko candidates.
  - `GfxRenderer` has proof-level vertical, sideways, and tate-chu-yoko text drawing helpers.
- Multi-font page prewarm now tracks scanned text per font id, so ruby/small-font text cannot steal the body CJK font
  prewarm slot.
- Initial native vertical column layout is implemented:
  - `ParsedText` has a vertical sibling layout path that emits top-to-bottom column blocks.
  - `TextBlock` can carry vertical `wordYpos` geometry and render upright CJK, sideways Latin, and tate-chu-yoko digits.
  - The chapter parser paginates vertical columns right-to-left for resolved vertical writing mode.
  - Section cache version was bumped for vertical `TextBlock` geometry.
- Vertical spacing/kinsoku has a first polish pass:
  - Vertical layout uses representative CJK glyph advance instead of raw line height for body rhythm.
  - Column breaks use the existing Japanese kinsoku head/tail helpers.
  - Single vertical glyph rendering stays on the precomputed layout position and avoids render-time width measurement.
  - The over-aggressive half-width punctuation/small-kana compression was backed out after device testing showed overlap.
  - Native vertical blocks currently suppress ruby sidecars until proper vertical ruby placement is implemented, so ruby-bearing
    base text follows the same per-glyph column rhythm as surrounding Japanese text.
  - Japanese EPUB font size selection is keyed from EPUB language metadata (`ja`/`jpn`), not vertical/horizontal writing mode.
  - SD-card cpfont format v5 stores OpenType `vert`/`vrt2` codepoint substitutions; the reader remains compatible with
    v4 fonts, but vertical rendering and layout use punctuation alternates only when regenerated v5 fonts provide them.
  - Manual Vertical RL is guarded to Japanese-language or book-declared vertical EPUBs; non-Japanese horizontal EPUBs ignore
    a stale global Vertical RL setting and hide that choice in the book menu.
  - Section cache version was bumped for the revised vertical geometry.
- Dictionary cursor geometry now uses the same page/block positions as rendering:
  - Cursor rectangles are built from `PageLine` plus `TextBlock` coordinates, using `wordYpos` for vertical blocks and
    horizontal prefix advances for horizontal blocks.
  - Column/line jumps compare the relevant axis for the selected block: vertical columns by `PageLine::xPos`, horizontal
    lines by `PageLine::yPos`.
  - Lookup text still walks logical `TextBlock::words`, so ruby readings remain out of dictionary context.
  - Dictionary popup presentation now follows normal UI orientation for both horizontal and vertical Japanese text.
- The short-lived combined Reading Layout/Auto orientation infrastructure has been removed:
  - Reader menu rotation, status bar drawing, sleep popups, chapter/footnote/percent/QR/sync sub-activities, and section
    viewport sizing use the explicit physical orientation.
  - Existing settings with the old combined `readingLayout` key are migrated into explicit `orientation` plus
    `writingModePreference` where possible.
  - Temporary page-render timing telemetry was removed after the BW/AA performance fix was validated on device.

## Still Pending

### Ruby

- Replace current `SMALL_FONT_ID` proof rendering with a proper small reader/ruby font path.
- Add vertical ruby placement after native vertical columns exist.
- Add a user-facing ruby enable/disable setting only after font and vertical behavior are stable.
- Re-test dictionary cursor, popup, selected text extraction, and lookup flow against ruby-heavy chapters in horizontal and
  vertical writing modes.

### Native Vertical Text

Goal: support EPUB writing mode as native layout state, not as rotated screen
orientation or rotated horizontal lines pretending to be columns.

- Preserve English/horizontal EPUB behavior as vertical layout work continues:
  - Horizontal remains the default unless the book/section explicitly resolves to vertical mode.
  - Existing horizontal layout, hyphenation, focus reading, ruby sidecar data, cache loading, and dictionary text extraction
    should remain on the current path.
  - Vertical-only vectors and metadata should stay lazy/absent for horizontal pages where possible.
- Extend layout data explicitly:
  - Keep `TextBlock` as the logical text carrier, but add vertical geometry such as `wordYpos`, an `isVertical` flag,
    and per-word/per-unit vertical behavior where needed.
  - Keep ruby readings as sidecar data parallel to logical body words; do not flatten ruby back into dictionary text.
  - Keep horizontal `wordXpos` behavior unchanged for non-vertical blocks.
- Continue hardening native vertical column layout:
  - Rework image placement and mixed block spacing for vertical pages.
  - Tighten vertical kinsoku at column breaks after device testing.
  - Keep chunked CJK layout/fallback memory behavior in mind for very large Japanese paragraphs.
- Add renderer support:
  - Add native `drawTextVertical`/sideways/tate-chu-yoko rendering instead of rotating whole lines.
  - Draw vertical punctuation and ruby based on the resolved per-unit behavior.
  - OpenType `vert`/`vrt2` codepoint substitutions are supported for regenerated v5 SD fonts. Glyph-id-only vertical
    alternates, such as Noto Serif JP's vertical long sound mark, still fall back to the original glyph.
- Reconcile reader navigation and cache behavior:
  - Page-turn direction should come from resolved writing mode/page progression, not from physical orientation alone.
  - Status bar/menu orientation should remain governed by device/system UI orientation.
  - Section cache, prewarm, silent indexing, and progress restoration must include writing mode in their assumptions.
- Preserve cursor/dictionary lookup:
  - Dictionary context should continue to come from logical `TextBlock::words`, not visual order or ruby text.
  - Cursor rectangles and movement need a vertical geometry map from logical word/byte offsets to screen coordinates.
  - Movement should understand vertical columns: up/down within a column, left/right across columns, while lookup text still
    follows logical reading order.
  - Re-test popup placement and selected text extraction on ruby-heavy vertical pages.

## Recommended Order

1. Keep ruby data infrastructure as its own standalone commit.
2. Done: add writing-mode detection/cache plumbing and split writing mode from device orientation.
3. Done: add vertical primitives and renderer proof drawing.
4. Done: add initial vertical column layout and RTL column flow.
5. Done: vertical kinsoku and spacing first pass.
6. Done: dictionary cursor geometry for horizontal and vertical Japanese blocks.
7. Polish ruby-in-vertical and glyph-id-only vertical alternates.

## Native Vertical Text Testable Slices

These slices should stay small enough to test from the reader UI, with serial logs useful but not required.

1. Done: writing-mode detection only.
   - Change:
     - Parse OPF `page-progression-direction` and CSS `writing-mode`/`-epub-writing-mode`.
     - Resolve and store a per-book/per-spine writing-mode decision.
     - Keep physical orientation as an independent user setting.
   - Visible/manual tests:
     - Open a known English horizontal EPUB and confirm it is still horizontal.
     - Open a known Japanese vertical EPUB with Writing Mode = Book's Style and confirm native vertical columns are used.
     - Open the reader menu/status bar and confirm UI orientation follows Settings -> Display -> Orientation, not the
       detected writing mode.
     - From the reader menu, cycle Writing Mode and confirm it shows the same three values as the Settings entry.

2. Done: cache-key plumbing.
   - Change:
     - Add resolved writing mode/page flow to the section cache header and bump the section cache version.
     - Keep horizontal and vertical caches from being reused across modes.
   - Visible/manual tests:
     - Open an English EPUB twice; second open should use cache and look unchanged.
     - Open a Japanese vertical EPUB; cache should rebuild once after the version bump.
     - Switching Writing Mode between Book's Style, Horizontal, and Vertical RL should rebuild the section instead of
       reusing stale pages.
     - Switching Writing Mode from the reader menu should behave the same as switching it from Settings.

3. Done: vertical primitive proof drawing.
   - Change:
     - Add `VerticalTextUtils` classification and renderer proof drawing for upright CJK, punctuation, sideways Latin, and
       short tate-chu-yoko numbers.
     - Keep this behind a narrow vertical-only path or debug/proof block.
   - Visible/manual tests:
     - A simple Japanese test page shows characters upright top-to-bottom, not as a rotated horizontal line.
     - Latin words render sideways and 1-2 digit numbers render as tate-chu-yoko.
     - Menus/status bar still follow the explicit Orientation setting, not text writing mode.

4. Done: native vertical column layout.
   - Change:
     - Add a sibling vertical layout path in `ParsedText`, sharing CJK/kana/punctuation helpers where practical.
     - Emit `TextBlock` vertical geometry (`wordYpos`, `isVertical`, behavior metadata) while leaving horizontal `wordXpos`
       unchanged.
     - Lay out columns right-to-left for vertical-rl books.
   - Visible/manual tests:
     - Japanese text flows top-to-bottom in columns, then right-to-left across columns.
     - Changing Orientation rotates the whole reader UI/content view in the master-style way.
     - Changing Writing Mode changes horizontal versus vertical EPUB layout without changing menu/status-bar policy.
     - The reader menu's Writing Mode row changes layout only after leaving the menu, preserving approximate page position
       across the reflow.
     - Page turns advance in the expected Japanese direction.
     - English EPUBs still paginate and hyphenate as before.
     - Long Japanese paragraphs do not crash or visibly truncate.

5. Done: vertical kinsoku and spacing.
   - Change:
     - Apply no-column-start/no-column-end rules using shared CJK punctuation helpers.
     - Measure vertical advance from representative CJK glyph metrics instead of raw line height.
     - Render single vertical glyphs from precomputed layout positions without half-cell punctuation/small-kana compression.
     - Suppress vertical ruby sidecars for now, rather than treating ruby-bearing words as separate whole-word vertical runs.
     - Keep Japanese EPUB font size selection keyed from EPUB language metadata, not writing mode.
     - Extract and apply OpenType `vert`/`vrt2` codepoint substitutions from regenerated v5 SD fonts while keeping v4 fonts
       compatible with the original fallback rendering.
   - Visible/manual tests:
     - Closing punctuation and small kana do not start a new column where avoidable.
     - Opening punctuation does not end a column where avoidable.
     - Japanese vertical text looks less like a naive square grid without glyph overlap or status-bar bleed.
     - Ruby-bearing base characters align with surrounding column text, even though vertical ruby annotations are not drawn yet.
     - Switching a Japanese EPUB between horizontal and vertical keeps the Japanese font-size setting.
     - Regenerated v5 Japanese cpfonts use vertical punctuation alternates in vertical mode.
     - Fonts without `vert` data, or `vert` replacements that have no Unicode codepoint, continue to render the original glyph.
     - An English horizontal EPUB still opens/indexes horizontally even if the global Writing Mode setting was left on Vertical RL.
     - A Japanese vertical EPUB rebuilds cache once after the version bump, then reopens from cache.
     - Horizontal EPUB spacing is unchanged.

6. Done: dictionary cursor geometry.
   - Change:
     - Move dictionary/cursor activation from orientation assumptions to Japanese-language EPUBs.
     - Build cursor rectangles from shared `PageLine` + `TextBlock` geometry while lookup context still comes from logical
       `TextBlock::words`.
     - Use vertical `wordYpos` for native columns and horizontal prefix advance/ruby padding for horizontal lines.
     - Make line/column jumps compare the selected block's flow axis.
     - Use a normal UI-oriented dictionary popup for horizontal and vertical Japanese text.
     - Keep ruby text out of lookup strings.
   - Visible/manual tests:
     - Cursor highlight lands on the selected kanji in vertical columns.
     - Cursor highlight lands on the selected kanji in horizontal Japanese lines, including ruby-bearing lines.
     - Sequential movement follows logical reading order in both writing modes.
     - Side-button jumps move between vertical columns or horizontal lines, depending on the selected block.
     - Dictionary popup lookup still finds Japanese terms from logical reading order.
     - Ruby-heavy pages do not include readings in selected lookup text.
     - English EPUBs do not enter dictionary cursor mode.

7. Ruby-in-vertical polish.
   - Change:
     - Draw ruby beside vertical base glyphs with a proper small reader/ruby font path.
     - Fix cursor rectangles where ruby expands visual ink bounds.
   - Visible/manual tests:
     - Ruby appears beside base text in vertical pages and above base text in horizontal pages.
     - Disabling or hiding ruby later should not change dictionary lookup text.
     - Cursor boxes remain aligned on ruby-heavy vertical and horizontal pages.

8. Glyph-id-only vertical alternates.
   - Change:
     - Extend cpfont beyond codepoint-to-codepoint `vert` substitutions so glyph-id-only replacements can be stored and
       rendered without private Unicode hacks.
   - Visible/manual tests:
     - The Japanese long sound mark and dash-like glyphs use the font's true vertical forms when available.
     - Existing SD fonts without glyph-id-only alternate data still render with the fallback path.
