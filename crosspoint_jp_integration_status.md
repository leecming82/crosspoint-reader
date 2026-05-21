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
  - Auto layout selects vertical reader layout for resolved vertical EPUBs.
  - Menu/status/system UI orientation remains governed by reader/device orientation.
  - Dictionary cursor activation is gated by resolved writing mode instead of orientation alone.
- Section cache plumbing includes writing mode:
  - The section cache version was bumped.
  - Section cache headers include resolved writing mode, preventing horizontal/vertical cache reuse.
  - Silent indexing and cache lookup use the same writing-mode key.
- Vertical rendering primitives exist for later layout integration:
  - `VerticalTextUtils` classifies upright CJK, kinsoku punctuation, sideways text, and tate-chu-yoko candidates.
  - `GfxRenderer` has proof-level vertical, sideways, and tate-chu-yoko text drawing helpers.
- Multi-font page prewarm now tracks scanned text per font id, so ruby/small-font text cannot steal the body CJK font
  prewarm slot.

## Still Pending

### Ruby

- Replace current `SMALL_FONT_ID` proof rendering with a proper small reader/ruby font path.
- Add vertical ruby placement after native vertical columns exist.
- Add a user-facing ruby enable/disable setting only after font and vertical behavior are stable.
- Re-test dictionary cursor, popup, selected text extraction, and lookup flow against ruby-heavy chapters.
- Fix cursor rectangle placement on ruby-bearing lines if needed. Ruby readings are transparent to cursor indexing and
  dictionary text because they are not stored in `TextBlock::words`, but the cursor box currently does not account for
  the extra `rubyTopPadding()` used when drawing the shifted base text.

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
- Add true vertical column layout:
  - Add a `ParsedText` vertical layout path that measures column height, emits columns, and flows columns right-to-left.
  - Apply vertical kinsoku at column breaks.
  - Handle vertical spacing and tate-chu-yoko as layout measurements, not renderer-only hacks.
  - Keep chunked CJK layout/fallback memory behavior in mind for large Japanese paragraphs.
- Add renderer support:
  - Add native `drawTextVertical`/sideways/tate-chu-yoko rendering instead of rotating whole lines.
  - Draw vertical punctuation and ruby based on the resolved per-unit behavior.
  - Decide separately whether to port OpenType `vert` substitute glyph extraction; this is quality polish and touches the
    SD font format, so it should not block first native columns.
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
4. Add vertical column layout and RTL page flow.
5. Rework cursor/dictionary geometry for native vertical blocks.
6. Polish ruby-in-vertical, small font selection, and optional OpenType `vert` support.

## Native Vertical Text Testable Slices

These slices should stay small enough to test from the reader UI, with serial logs useful but not required.

1. Done: writing-mode detection only.
   - Change:
     - Parse OPF `page-progression-direction` and CSS `writing-mode`/`-epub-writing-mode`.
     - Resolve and store a per-book/per-spine writing-mode decision.
   - Visible/manual tests:
     - Open a known English horizontal EPUB and confirm it is still horizontal.
     - Open a known Japanese vertical EPUB and confirm auto layout selects the vertical reader profile.
     - Open the reader menu/status bar and confirm UI orientation is unchanged by the detected writing mode.

2. Done: cache-key plumbing.
   - Change:
     - Add resolved writing mode/page flow to the section cache header and bump the section cache version.
     - Keep horizontal and vertical caches from being reused across modes.
   - Visible/manual tests:
     - Open an English EPUB twice; second open should use cache and look unchanged.
     - Open a Japanese vertical EPUB; cache should rebuild once after the version bump.
     - If a temporary setting/debug override is used to force horizontal vs vertical, switching modes should rebuild the
       section instead of reusing stale pages.

3. Done: vertical primitive proof drawing.
   - Change:
     - Add `VerticalTextUtils` classification and renderer proof drawing for upright CJK, punctuation, sideways Latin, and
       short tate-chu-yoko numbers.
     - Keep this behind a narrow vertical-only path or debug/proof block.
   - Visible/manual tests:
     - A simple Japanese test page shows characters upright top-to-bottom, not as a rotated horizontal line.
     - Latin words render sideways and 1-2 digit numbers render as tate-chu-yoko.
     - Menus/status bar still follow device orientation, not text writing mode.

4. Native vertical column layout.
   - Change:
     - Add a sibling vertical layout path in `ParsedText`, sharing CJK/kana/punctuation helpers where practical.
     - Emit `TextBlock` vertical geometry (`wordYpos`, `isVertical`, behavior metadata) while leaving horizontal `wordXpos`
       unchanged.
     - Lay out columns right-to-left for vertical-rl books.
   - Visible/manual tests:
     - Japanese text flows top-to-bottom in columns, then right-to-left across columns.
     - Page turns advance in the expected Japanese direction.
     - English EPUBs still paginate and hyphenate as before.
     - Long Japanese paragraphs do not crash or visibly truncate.

5. Vertical kinsoku and spacing.
   - Change:
     - Apply no-column-start/no-column-end rules using shared CJK punctuation helpers.
     - Measure vertical advance, sideways Latin height, tate-chu-yoko cell height, and vertical char spacing in layout.
   - Visible/manual tests:
     - Closing punctuation and small kana do not start a new column where avoidable.
     - Opening punctuation does not end a column where avoidable.
     - Changing vertical spacing visibly affects vertical text without changing horizontal EPUB spacing.

6. Dictionary cursor geometry.
   - Change:
     - Move dictionary/cursor activation from orientation assumptions to resolved writing mode.
     - Build cursor rectangles from vertical layout geometry while lookup context still comes from logical
       `TextBlock::words`.
     - Keep ruby text out of lookup strings.
   - Visible/manual tests:
     - Cursor highlight lands on the selected kanji in vertical columns.
     - Up/down move within a column; left/right move between columns.
     - Dictionary popup lookup still finds Japanese terms from logical reading order.
     - Ruby-heavy pages do not include readings in selected lookup text.

7. Ruby-in-vertical polish.
   - Change:
     - Draw ruby beside vertical base glyphs with a proper small reader/ruby font path.
     - Fix cursor rectangles where ruby expands visual ink bounds.
   - Visible/manual tests:
     - Ruby appears beside base text in vertical pages and above base text in horizontal pages.
     - Disabling or hiding ruby later should not change dictionary lookup text.
     - Cursor boxes remain aligned on ruby-heavy vertical and horizontal pages.

8. Optional OpenType `vert` support.
   - Change:
     - Only after native columns are stable, decide whether to extend SD font conversion/format for vertical substitute
       glyphs.
   - Visible/manual tests:
     - Vertical punctuation improves compared with the renderer-only fallback.
     - Existing SD fonts without `vert` data still render with the fallback path.
