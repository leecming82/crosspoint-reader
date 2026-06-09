# Japanese Dictionary Implementation Plan

Date: 2026-06-09

This document tracks a Murphy M4-focused path for adding a standalone Japanese dictionary app to CrossPoint, using the working `~/cardputer-jpdict` project as the reference implementation. Bluetooth keyboard input is intentionally deferred: the first goal is a touch-first dictionary experience that works well on Murphy's larger e-paper screen, then Bluetooth can be added later if the touch keyboard is not good enough.

This is a Murphy M4 feature. X3/X4 are not target devices for this work because they do not have tap/touch input, which is a core requirement for the dictionary UI and reader cursor workflow.

## Summary

The primary feature is a Home-screen Dictionary app. It should provide typed Japanese lookup similar to `~/cardputer-jpdict`, but adapted for CrossPoint and Murphy:

- Touch-first UI.
- Larger result layout than Cardputer.
- Romaji-to-kana input.
- Exact, prefix, and deinflected lookup.
- Full definition browsing.
- Optional kanji search later.

The dictionary data format should come from `~/cardputer-jpdict`, installed under CrossPoint's dictionary directory:

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

The existing EPUB reader cursor dictionary should migrate to the same backend and dictionary format first. The reader cursor should remain responsible for extracting context from book text, but lookup, ranking, and result representation should be shared with the standalone dictionary app.

Bluetooth keyboard support remains useful, but it should be an optional input layer after the dictionary app exists. If touch typing is acceptable, BLE may not be urgent. If touch typing is poor, BLE has a clean target: feed text/control events into the already-working dictionary activity.

## Architecture

Target shape:

```text
Standalone Dictionary
  -> touch keyboard / future external keyboard
  -> romaji-to-kana composition
  -> shared dictionary backend
  -> dictionary result UI

EPUB reader cursor
  -> reader context extraction
  -> shared dictionary backend
  -> reader popup UI

Optional BLE keyboard
  -> KeyboardEventQueue
  -> text-aware activities
```

The shared backend should hide the storage format and expose lookup methods that both activities can use. The standalone dictionary and reader popup should have different UI/presentation layers, but they should not diverge in dictionary files, lookup behavior, or ranking semantics.

## Observed Cardputer Build Layout

The sister repo has a built SD-card payload under `~/cardputer-jpdict/build/sd`. It contains both the main dictionary and the optional kanji/radical lookup index:

```text
build/sd/jpdict/
  manifest.json
  buckets.bin
  records.bin
  strings.bin
  key_filter.bin

build/sd/kanji/
  manifest.json
  lookup.records.bin
  lookup.strings.bin
```

Observed file sizes:

```text
jpdict/buckets.bin          8.5 M
jpdict/key_filter.bin       128 K
jpdict/manifest.json        4.0 K
jpdict/records.bin          92 M
jpdict/strings.bin          18 M
kanji/lookup.records.bin    448 K
kanji/lookup.strings.bin    192 K
kanji/manifest.json         4.0 K
```

The main dictionary manifest reports:

- `format`: `cardputer-jpdict`
- `version`: `1`
- `title`: `Jitendex.org [2026-05-05]`
- `entry_count`: `748832`
- `record_size`: `128`
- `key_bytes`: `96`
- `unicode_buckets`: `1114112`
- `key_filter`: `key_filter.bin`, `131072` bytes, `2` hashes, `fnv1a32-double`
- `default_install_path`: `/jpdict`
- Reading aliases are included in the generated dictionary.

The kanji lookup manifest reports:

- `format`: `cardputer-kanji-lookup`
- `version`: `2`
- `record_count`: `5984`
- `lookup.records.bin` and `lookup.strings.bin` are the runtime files.
- Lookup types are encoded with typed prefixes:
  - `reading`: `r`
  - `radical_alias`: `a`
  - `component_kanji`: `c`
  - `radical_strokes`: `s`
  - `kanji_strokes`: `k`

CrossPoint should document `/.crosspoint/dicts/jpdict` and `/.crosspoint/dicts/kanji` as one required install bundle. Normal dictionary lookup technically only needs `jpdict`, but the app should treat the dictionary and kanji/radical lookup data as the expected complete product.

`key_filter.bin` is a required part of the CrossPoint install even though the Cardputer runtime can technically fall back without it. It is a 128 KB Bloom filter used to avoid unnecessary SD seeks for keys that definitely do not exist. False positives are acceptable because they only cause an unnecessary lookup; false negatives should not occur.

## Milestones

### 1. Dictionary Format Baseline

- [x] Document the `~/cardputer-jpdict` SD dictionary format in CrossPoint terms.
- [x] Confirm the sister repo build contains both main dictionary and kanji/radical lookup binaries.
- [x] Confirm generated files from `~/cardputer-jpdict/scripts/build_jpdict.py` can live as an SD-card payload.
- [x] Confirm expected file paths: document `/.crosspoint/dicts/jpdict` and `/.crosspoint/dicts/kanji`.
- [x] Decide whether to keep `key_filter.bin` as optional or required: required.

Exit criteria: CrossPoint has a clear target dictionary layout and required install bundle.

### 2. Shared Dictionary Backend

- [x] Port or adapt the `~/cardputer-jpdict` dictionary reader.
- [x] Support `manifest.json`, `buckets.bin`, `records.bin`, `strings.bin`, and required `key_filter.bin`.
- [x] Add bundle validation that reports overall install status and lists only missing files.
- [x] Keep lookup bounded in RAM and SD reads.
- [x] Support exact lookup.
- [x] Support prefix lookup.
- [x] Support deinflected lookup.
- [x] Return a shared result model with term, reading, definition, score/source metadata, and match type.
- [x] Preserve the reader popup API through `lookupContext()` while switching the underlying backend to the Cardputer format.

Exit criteria: a test or diagnostic path can open `/jpdict` and return real matches for known queries.

### 3. Romaji-To-Kana Input

- [x] Port or share `RomajiKana` from `~/cardputer-jpdict`.
- [x] Maintain committed kana plus pending romaji state.
- [x] Convert incrementally as the user types.
- [x] Support final composition on search/enter.
- [x] Support backspace over pending romaji and committed UTF-8 kana.
- [ ] Handle direct kana input without forcing it through romaji composition.
- [x] Add a Hardware Diagnostics keyboard tester item that opens a test screen with an on-screen keyboard, a typed-text display, and a picker for English typing vs romaji-to-kana mode.

Exit criteria: typing `nihongo` produces `にほんご` predictably, mixed direct Japanese input still works, and Hardware Diagnostics can exercise both English and romaji keyboard modes.

### 4. Dictionary Activity Skeleton

- [x] Add `JapaneseDictionaryActivity`.
- [x] Add a Home-screen Dictionary item on Murphy.
- [x] Add a loading/missing-data state.
- [x] Add query state and empty results state.
- [x] Consume touch keyboard input first.
- [x] Preserve physical-button fallback for back/result navigation where natural.

Exit criteria: the app opens from Home, accepts a query, and handles missing dictionary data gracefully.

### 5. Murphy Touch UI

- [x] Design a touch-first layout for the 800x480 Murphy viewport.
- [x] Keep the query field visible while browsing results.
- [x] Render a result list with headword, reading, and short gloss.
- [x] Support tapping result rows.
- [x] Add clear/search/back controls.
- [x] Avoid nested cards and oversized marketing-style surfaces.
- [x] Ensure text does not overlap on long headwords/readings/definitions.

Exit criteria: lookup results are readable and directly tappable on Murphy without relying on hardware buttons.

### 6. Lookup And Result Browsing

- [x] Search exact results first.
- [x] Add prefix fallback.
- [x] Add deinflection results with visible but compact indication where useful.
- [x] Rank and cap results for predictable latency.
- [x] Add a full definition view.
- [x] Add pagination/scrolling for long definitions.
- [x] Keep redraws bounded for e-paper.

Exit criteria: common typed queries return useful Jitendex results and long definitions can be read without layout corruption.

### 7. E-Paper Typing UX

- [ ] Avoid full refresh on every keystroke where possible.
- [ ] Batch redraws when typing quickly.
- [ ] Use partial updates where safe.
- [ ] Keep pending romaji visually distinct from committed kana.
- [ ] Add a clear cursor treatment.
- [ ] Validate repeated lookup/edit cycles for ghosting and responsiveness.

Exit criteria: touch typing and result navigation feel acceptable on Murphy M4.

### 8. Reader Cursor Integration Polish

- [x] Keep reader cursor context extraction in the EPUB reader.
- [x] Replace reader lookup with the shared dictionary backend.
- [ ] Preserve reader popup behavior and result limits.
- [ ] Preserve deinflection/context behavior where better than the standalone lookup.
- [ ] Remove the old reader-only dictionary format/path once parity is confirmed.
- [ ] Verify existing Japanese reader cursor flows still work.

Exit criteria: both the standalone dictionary and EPUB cursor dictionary use the same dictionary files and lookup backend.

### 9. Dictionary Install And File Management

- [ ] Decide the preferred install path.
- [ ] Add user-facing missing-file guidance.
- [ ] Consider web/file-transfer affordances for installing `/jpdict`.
- [ ] Keep old dictionary paths readable during migration if needed.
- [ ] Document host conversion commands.

Exit criteria: users can understand where dictionary files go and what is missing when lookup is unavailable.

### 10. Kanji Search

- [ ] Port or adapt the optional kanji lookup index from `~/cardputer-jpdict`.
- [ ] Support reading, radical name, component, radical stroke count, and total stroke count lookup.
- [ ] Let selected kanji insert into the current query.
- [ ] Decide whether to block the full Dictionary app when `/kanji` data is absent, or allow a degraded developer-only path.
- [ ] Adapt the UI for touch and the larger Murphy screen rather than copying the Cardputer panes directly.

Exit criteria: kanji search works as part of the required dictionary bundle.

### 11. Optional Bluetooth Keyboard

- [ ] Reassess after real touch-keyboard use.
- [ ] If needed, add a Murphy-only BLE keyboard backend.
- [ ] Prefer `NimBLE-Arduino` if the built-in Arduino ESP32 BLE stack does not provide a suitable HID host/client path.
- [ ] Scan/connect to BLE HID service `0x1812`.
- [ ] Parse keyboard reports into text/control events.
- [ ] Feed events into the same dictionary input path used by the touch keyboard.
- [ ] Keep BLE disabled by default until pairing and power behavior are stable.

Exit criteria: Bluetooth is an optional accelerator, not a dependency for the dictionary feature.

### 12. Power And Coexistence

- [ ] Verify dictionary lookup does not destabilize SD_MMC or reader caches.
- [ ] Verify touch keyboard and dictionary rendering do not create display artifacts.
- [ ] If BLE is added, define when it is enabled/disabled.
- [ ] If BLE is added, suspend it before deep sleep.
- [ ] If BLE is added, verify Wi-Fi/BLE coexistence during web server and downloads.

Exit criteria: dictionary use does not regress normal reading, sleep, Wi-Fi, or diagnostics.

### 13. Polish And Defaults

- [ ] Decide final Home placement and icon/label.
- [ ] Add a useful empty state.
- [ ] Add recent query history if helpful.
- [ ] Add clear/install guidance for dictionary data.
- [ ] Keep feature gates Murphy-specific until broader validation.

Exit criteria: the dictionary is usable as a first-class app surface, not just a diagnostic experiment.

## Open Questions

- Should development builds support `/jpdict` and `/kanji` as temporary compatibility fallbacks, or only the documented `/.crosspoint/dicts/...` paths?
- If the main dictionary is present but kanji data is missing, should the public UI block entry entirely or show a degraded dictionary-only mode?
- Should the standalone dictionary reuse the current CrossPoint reader dictionary runtime or port the Cardputer dictionary reader wholesale?
- How much of Cardputer's ranking/deinflection behavior should become the shared CrossPoint behavior?
- What is the best touch keyboard layout for romaji input on Murphy?
- Should the dictionary app appear on Home for all boards with enough support, or Murphy only at first?
- Does real touch typing feel good enough to defer Bluetooth indefinitely?
