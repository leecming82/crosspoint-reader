# Japanese Dictionary

CrossPoint has a Murphy M4-focused Japanese dictionary app and EPUB reader
popup lookup. The feature is touch-first: the on-screen romaji keyboard is good
enough for the current product, so the earlier Bluetooth keyboard milestones
have been removed rather than carried as planned work.

X3/X4 are not target devices for the standalone dictionary UI because they do
not have touch input. They continue to use the existing reader/device flows.

## Current Status

Implemented:

- Home-screen Japanese Dictionary activity on Murphy M4.
- Touch keyboard with romaji-to-kana composition.
- Exact, prefix, deinflected, katakana fallback, and segmented exact lookup.
- Result list with headword, reading, and gloss preview.
- Full definition view with paged long-definition browsing.
- Shared `JapaneseDictionary` backend used by both the standalone app and EPUB
  reader popup.
- Required SD-card dictionary bundle validation with visible missing-file
  guidance.
- Kanji/radical search using the Cardputer-derived `kanji` lookup payload.
- Kanji search by reading, radical/component alias, radical stroke count,
  component, and total kanji stroke count.
- Selected kanji insertion into the dictionary query.

Still worth improving:

- E-paper typing polish: partial refresh, batching, cursor treatment, and
  clearer pending-romaji display.
- Recent query history if useful.
- Final home placement/icon/label polish.
- Broader no-regression validation for SD_MMC, reader caches, sleep, Wi-Fi, and
  diagnostics.

## Install Layout

Copy the built dictionary payload to the SD card under:

```text
/.crosspoint/dicts/jpdict/manifest.json
/.crosspoint/dicts/jpdict/buckets.bin
/.crosspoint/dicts/jpdict/records.bin
/.crosspoint/dicts/jpdict/strings.bin
/.crosspoint/dicts/jpdict/key_filter.bin
/.crosspoint/dicts/kanji/manifest.json
/.crosspoint/dicts/kanji/lookup.records.bin
/.crosspoint/dicts/kanji/lookup.strings.bin
```

The app treats `jpdict` and `kanji` as one required bundle. If any required
file is absent, the dictionary screen reports the missing files.

Existing file transfer is the install path for now. No Bluetooth keyboard or
Bluetooth dictionary-install path is planned.

## Source Build

The reference builder currently lives in the sister repo:

```sh
cd ~/cardputer-jpdict
python3 scripts/build_jpdict.py convert test/jitendex-yomitan build/sd/jpdict
python3 scripts/build_kanji_index.py convert-all test/kanjidic2.xml.gz test/kradfile build/sd/kanji
```

Then copy `build/sd/jpdict` and `build/sd/kanji` to the CrossPoint install
paths above.

## Runtime Shape

Main dictionary lookup uses:

- `buckets.bin`: dense Unicode first-codepoint bucket table.
- `records.bin`: fixed 128-byte sorted records.
- `strings.bin`: readings and flattened definitions.
- `key_filter.bin`: required Bloom filter for fast miss rejection.

Kanji/radical lookup uses:

- `lookup.records.bin`: fixed 76-byte sorted records.
- `lookup.strings.bin`: packed UTF-8 candidate strings.
- typed lookup keys:
  - `r:<reading>` for kanji readings
  - `a:<alias>` for radical/component aliases
  - `c:<component>` for kanji containing a component
  - `s:<count>` for radical stroke counts
  - `k:<count>` for total kanji stroke counts

Both the standalone dictionary app and EPUB reader popup use the shared
`JapaneseDictionary` backend and the same `/.crosspoint/dicts/jpdict` files.

## Lookup Behavior

Standalone lookup:

1. Compose pending romaji into kana on search.
2. Try exact lookup for the query.
3. Try exact lookup for katakana fallback when appropriate.
4. Add prefix matches up to bounded limits.
5. Add deinflected matches.
6. Fall back to segmented exact lookup for multi-segment queries.
7. Rank by tier, score, deinflection depth, flags, and term.

Reader popup lookup:

1. EPUB reader extracts context starting at the selected Japanese character.
2. Shared backend generates dictionary prefixes from that context.
3. Longest consumed text wins first.
4. Exact and deinflected matches are returned through the shared result model.
5. Popup rendering remains separate from the standalone dictionary UI.

## UI Notes

The Murphy dictionary UI is intentionally touch-native rather than a literal
copy of the Cardputer app:

- The result list keeps the query visible.
- Result rows are tappable.
- Full definitions show the term on the first line and the reading on its own
  bracketed line.
- Kanji search uses a vertically stacked layout: input, selected parts, parts
  grid, kanji grid, keyboard.
- Radical/component taps add filters; kanji taps insert into the main query.
- The soft keyboard is the primary input method.

## Completed Milestones

- Dictionary format baseline.
- Shared dictionary backend.
- Romaji-to-kana input.
- Dictionary activity skeleton.
- Murphy touch UI.
- Lookup and result browsing.
- Reader cursor integration.
- Dictionary install/file-management decisions.
- Kanji search.

Bluetooth keyboard support was removed from the dictionary plan after real soft
keyboard use proved good enough.

## Remaining Polish

- Avoid full refresh on every keystroke where possible.
- Batch redraws when typing quickly.
- Use partial updates where safe.
- Keep pending romaji visually distinct from committed kana.
- Add a clear cursor treatment.
- Validate repeated lookup/edit cycles for ghosting and responsiveness.
- Verify dictionary lookup does not destabilize SD_MMC or reader caches.
- Verify touch keyboard and dictionary rendering do not create display artifacts.
- Decide whether recent query history is useful.

## Future JMnedict / Multi-Dictionary Work

JMnedict support should be treated as a future format reset rather than a
compatibility extension of the current single-dictionary bundle.

Goals:

- Support JMnedict name dictionaries alongside regenerated Jitendex/JMdict
  dictionaries.
- Allow multiple dictionaries to be enabled, disabled, and ranked from a System
  settings menu.
- Prefer a clean new converted-dictionary format over compatibility with old
  converted dictionaries.
- Keep raw Yomitan dictionaries as the source of truth.
- Provide both CLI and browser-based conversion using one shared converter core.

Recommended future format direction:

- `manifest.json` should contain firmware-facing metadata such as `id`, `title`,
  `source_type`, `revision`, `entry_count`, `record_size`, `format`, and
  `version`.
- Source labels should come from the manifest, not from firmware guessing
  directory names.
- Old converted dictionaries should be marked incompatible or ignored with a
  visible status.
- New binaries should remain optimized for SD-card lookup, but record contents
  may change to store useful JMnedict category/source data.

Recommended future implementation steps:

1. Create a shared converter core usable from both CLI and browser.
2. Add raw Yomitan source inspection.
3. Split conversion into source-specific loaders and a shared CrossPoint writer.
4. Implement JMnedict-specific entry shaping for surnames, places, people,
   companies, stations, works, and products.
5. Keep desktop lookup and benchmark tools working for the new format.
6. Add browser-side conversion in the web transfer UI.
7. Add SD-card dictionary discovery under `/.crosspoint/dicts`.
8. Persist enabled dictionaries and rank order in a small SD-card settings file.
9. Add a System -> Dictionaries menu.
10. Replace single-dictionary lookup with a multi-dictionary manager.
11. Add source labels to popup results when multiple dictionaries are enabled.
12. Handle missing, incompatible, and corrupted dictionaries visibly.

Suggested no-serial regression matrix for that future work:

- only regenerated Jitendex
- only JMnedict
- both dictionaries, Jitendex first
- both dictionaries, JMnedict first
- old incompatible dictionary
- no dictionaries
