# JMnedict Dictionary Implementation Plan

## Objectives

- Support JMnedict name dictionaries alongside regenerated Jitendex/JMdict dictionaries.
- Allow multiple dictionaries to be enabled, disabled, and ranked from the System settings menu.
- Prefer a clean new converted-dictionary format over compatibility with old converted dictionaries.
- Keep raw Yomitan dictionaries as the source of truth; converted dictionaries may need regeneration.
- Provide both CLI and browser-based conversion, using the same converter codebase so format/source updates are not duplicated.
- Make every implementation stage testable without serial access, using desktop commands, SD-card files, visible menus, and reader popup behavior.

## Format Reset

Backward compatibility with existing converted dictionaries is not required.

Use a new converted dictionary format, for example `crosspoint-jpdict` version `3`. The converter and firmware may change the manifest schema, record layout, metadata fields, directory naming, and lookup assumptions if doing so simplifies multi-dictionary support.

Required direction:

- `manifest.json` must contain firmware-facing metadata: `id`, `title`, `source_type`, `revision`, `entry_count`, `record_size`, `format`, and `version`.
- Source labels should come from the manifest, not from firmware guessing directory names.
- Old converted dictionaries should be marked incompatible or ignored with a visible status.
- New binaries should remain optimized for SD-card lookup, but record contents may change to store useful JMnedict category/source data.
- The CLI converter and browser converter must share the same core conversion logic. Avoid separate Python-only and JavaScript-only implementations that can drift.

## Dictionary Preparation

1. Create a shared converter core.

   The CLI and browser Dictionary tab should run the same conversion rules. Prefer a shared implementation that can be invoked from both environments, for example:

   - TypeScript/JavaScript converter core used directly by the browser and by a Node-based CLI wrapper
   - or a small portable converter core generated/reused by both wrappers

   The exact technology can be chosen during implementation, but the rule is strict: source detection, Jitendex shaping, JMnedict shaping, ranking metadata, manifest writing, binary record layout, string interning, deduplication, and bucket writing must live in one shared code path.

   Test without serial:

   - convert the same raw Jitendex and JMnedict sources through the CLI wrapper and browser wrapper
   - compare manifests and binary outputs, allowing only expected nondeterministic fields if any are intentionally present

2. Add source inspection.

   Add `scripts/build_japanese_dict.py inspect SRC` for raw Yomitan dictionaries. It should validate `index.json`, detect `term_bank_*.json`, check expected term record positions, count entries, and print title/revision/format/source-type hints.

   Test without serial:

   ```sh
   mkdir -p /tmp/jmnedict-src
   unzip -oq test/JMnedict.zip -d /tmp/jmnedict-src
   python3 scripts/build_japanese_dict.py inspect /tmp/jmnedict-src
   python3 scripts/build_japanese_dict.py inspect test/jitendex-yomitan
   ```

3. Refactor the converter around source loaders and a shared writer.

   Split conversion into a shared CrossPoint writer plus source-specific loaders:

   - `jitendex` / general JMdict-style Yomitan loader
   - `jmnedict` name-entry Yomitan loader
   - `auto` detection from `index.json`

   Required command shape:

   ```sh
   python3 scripts/build_japanese_dict.py convert SRC OUT --source-type auto
   python3 scripts/build_japanese_dict.py convert SRC OUT --source-type jitendex
   python3 scripts/build_japanese_dict.py convert SRC OUT --source-type jmnedict
   ```

   Expected converted output:

   - `manifest.json`
   - `buckets.bin`
   - `records.bin`
   - `strings.bin`

   Test without serial:

   ```sh
   python3 scripts/build_japanese_dict.py convert test/jitendex-yomitan /tmp/jitendex-cpdict \
     --first-char-scope japanese --reading-aliases
   python3 scripts/build_japanese_dict.py convert /tmp/jmnedict-src /tmp/jmnedict-cpdict \
     --first-char-scope japanese --reading-aliases
   ```

   Expected result: both manifests identify source type, title, revision, entry count, record size, and the new format version.

4. Implement JMnedict-specific entry shaping.

   JMnedict output must preserve name-focused metadata:

   - use `entry[2]` categories such as `surname`, `place`, `person`, `company`, `station`, `work`, and `product`
   - treat kana-only glossary values as readings when `entry[1]` is empty
   - keep English glosses when present
   - deduplicate repeated readings for the same term/category
   - store category/source metadata separately when cleaner than flattening it into the definition

   Recommended display data:

   - reading: kana reading when known
   - category: compact name category for display/ranking
   - definition: English glosses when available
   - fallback definition: alternate readings or compact category text

   Test without serial:

   ```sh
   python3 scripts/build_japanese_dict.py lookup /tmp/jmnedict-cpdict 東京
   python3 scripts/build_japanese_dict.py lookup /tmp/jmnedict-cpdict 山田
   python3 scripts/build_japanese_dict.py lookup /tmp/jmnedict-cpdict 吉川
   ```

   Expected result: output shows readable place/name/surname information, not blank readings with only kana in the definition.

5. Keep desktop lookup and benchmark tools working for the new format.

   Update `lookup` and `bench` subcommands to read the new manifest/record layout. They are the primary no-serial smoke tests before copying dictionaries to SD.

   Test without serial:

   ```sh
   python3 scripts/build_japanese_dict.py lookup /tmp/jitendex-cpdict 日本語
   python3 scripts/build_japanese_dict.py lookup /tmp/jmnedict-cpdict 東京
   python3 scripts/build_japanese_dict.py bench /tmp/jitendex-cpdict --samples 1000
   python3 scripts/build_japanese_dict.py bench /tmp/jmnedict-cpdict --samples 1000
   ```

6. Add browser-side conversion in the web transfer UI.

   Add a separate `Dictionaries` tab to the network transfer web app, distinct from the existing Fonts page. It should let users import raw Yomitan zip files without using the command line:

   - choose or drag/drop `jitendex-yomitan.zip`, `JMnedict.zip`, or another supported Yomitan zip
   - inspect source metadata before conversion
   - convert in the browser using the shared converter core
   - show progress by phase: unzip, inspect, parse, normalize, sort, write binaries, upload
   - upload the converted files to `/.crosspoint/dicts/<id>/`
   - warn that large dictionaries should be converted from a desktop browser, not a phone

   Implementation requirements:

   - run heavy work in a Web Worker so the UI remains responsive
   - use the existing browser-side ZIP support where practical
   - avoid loading more raw JSON state than needed, but keep output deterministic
   - surface conversion errors visibly in the page
   - do not add firmware-side raw Yomitan import

   Test without serial:

   - open the web interface from a desktop browser
   - convert `test/JMnedict.zip` through the Dictionary tab
   - confirm `manifest.json`, `buckets.bin`, `records.bin`, and `strings.bin` appear under `/.crosspoint/dicts/<id>/` on SD
   - compare the browser-generated output against CLI-generated output for the same source

## Firmware

1. Add SD-card dictionary discovery.

   Discover dictionary directories under:

   - `/.crosspoint/dicts`
   - `/dict`

   A valid regenerated dictionary must contain the four converted files and a supported manifest version. A helper such as `JapaneseDictionaryCatalog` should expose path, id, display title, source type, entry count, status, enabled state, and rank.

   Test without serial:

   - copy regenerated dictionaries to `/.crosspoint/dicts/jitendex` and `/.crosspoint/dicts/jmnedict`
   - open a visible dictionary list screen
   - remove one directory from SD and reopen the screen

   Expected result: valid dictionaries appear, removed dictionaries disappear or show missing status, and old-format dictionaries show incompatible status.

2. Persist enabled dictionaries and rank order.

   Store dictionary selection outside `CrossPointSettings`, preferably:

   ```text
   /.crosspoint/dictionaries.json
   ```

   Suggested shape:

   ```json
   {
     "version": 1,
     "dictionaries": [
       {"id": "jitendex", "path": "/.crosspoint/dicts/jitendex", "enabled": true},
       {"id": "jmnedict", "path": "/.crosspoint/dicts/jmnedict", "enabled": true}
     ]
   }
   ```

   Discovery should merge current SD contents with this file. New dictionaries can default to disabled, or enabled only when no settings file exists. Priority order is list order.

   Test without serial:

   - enable both dictionaries
   - reorder them
   - exit settings
   - inspect `/.crosspoint/dictionaries.json` on SD
   - restart and confirm the menu reloads the same state

3. Add the System -> Dictionaries menu.

   Add a System settings action named `Dictionaries`. The screen must support:

   - check/uncheck current dictionary
   - move selected dictionary up/down in priority
   - show status: valid, missing, incompatible, or invalid
   - show compact metadata: title/source type and entry count
   - save on Back

   Suggested controls:

   - Confirm: toggle enabled
   - Next/Previous: move selection
   - long Next/Previous or secondary command: reorder priority
   - Back: save and return

   Test without serial:

   - copy two valid dictionaries to SD
   - open Settings -> System -> Dictionaries
   - toggle and reorder dictionaries
   - leave and reopen the screen

   Expected result: checkmarks, statuses, and ordering persist visibly.

4. Replace single-dictionary lookup with a multi-dictionary manager.

   The manager should open enabled dictionaries in priority order and merge results. It must not stop after the first dictionary returns matches.

   Required ranking:

   - longest consumed source text first
   - dictionary priority
   - dictionary tier/category rank
   - score
   - deinflection depth
   - term

   This prevents a short general-dictionary match like `山` from hiding a longer JMnedict name match like `山田`.

   Test without serial:

   - create or open a Japanese EPUB from SD containing names such as `山田`, `東京`, and `吉川`
   - enable only Jitendex, only JMnedict, then both in each priority order

   Expected result: JMnedict can supply full name matches when enabled, and priority affects otherwise comparable results without hiding longer matches.

5. Add source labels to popup results.

   Extend dictionary matches with source label metadata from `manifest.json`. Show labels when multiple dictionaries are enabled or when mixed-source results appear. Keep the popup compact for e-ink.

   Test without serial:

   - enable both dictionaries
   - lookup a shared term such as `東京`
   - cycle through popup matches

   Expected result: the popup distinguishes general dictionary results from name dictionary results.

6. Handle missing, incompatible, and corrupted dictionaries visibly.

   Required behavior:

   - missing binary file: invalid status, skip during lookup
   - unsupported manifest/version: incompatible status, skip during lookup
   - no valid enabled dictionaries: no crash; lookup behaves as no match
   - no discovered dictionaries: empty dictionary menu state and no popup matches

   Test without serial:

   - remove `records.bin` from a copied dictionary, open the menu, and try lookup
   - copy an old converted dictionary to SD and open the menu
   - restore/regenerate the dictionary and confirm it becomes valid

7. Final no-serial regression matrix.

   Verify these SD-card states:

   - only regenerated Jitendex
   - only JMnedict
   - both dictionaries, Jitendex first
   - both dictionaries, JMnedict first
   - old incompatible dictionary
   - no dictionaries

   Use only visible menus, EPUB lookup popups, SD-card JSON inspection, and desktop converter/lookup smoke tests.
