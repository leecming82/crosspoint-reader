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

## Still Pending

### Ruby

- Replace current `SMALL_FONT_ID` proof rendering with a proper small reader/ruby font path.
- Add vertical ruby placement after native vertical columns exist.
- Add a user-facing ruby enable/disable setting only after font and vertical behavior are stable.
- Re-test dictionary cursor, popup, selected text extraction, and lookup flow against ruby-heavy chapters.

### Native Vertical Text

- Port/adapt vertical primitives such as `VerticalTextUtils`.
- Parse CSS `writing-mode` and OPF `page-progression-direction=rtl`.
- Add true vertical column layout with right-to-left column flow.
- Add renderer support for vertical punctuation, kinsoku, sideways Latin, tate-chu-yoko, and vertical spacing.
- Reconcile `EpubReaderActivity` navigation, cache behavior, and dictionary popup behavior with vertical mode.
- Decide separately whether to port OpenType `vert` substitute glyph extraction.

## Recommended Order

1. Keep ruby data infrastructure as its own standalone commit.
2. Add vertical renderer primitives.
3. Add vertical column layout and RTL page flow.
4. Polish ruby-in-vertical and font selection.
