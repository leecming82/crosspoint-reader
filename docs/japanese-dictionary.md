# Japanese Dictionary

This is the current phase-2 candidate for Yomitan/Jitendex lookup on device.

## Source

The Yomitan zip is unpacked under `test/jitendex-yomitan/` for inspection.
The local Jitendex sample has:

- 216 `term_bank_*.json` files
- 430,822 term entries
- 5,987 non-empty first-character buckets
- largest bucket: `ア`, 5,629 entries
- largest kanji buckets: `一`, 2,904 entries; `大`, 2,797 entries

## Proposed SD Format

The format favors physical storage over RAM. No large index needs to be loaded
on device.

Files:

- `buckets.bin`: dense Unicode bucket table, one `<start,count>` pair per codepoint.
  - Size: `0x110000 * 8`, about 8.5 MB.
  - Lookup is one seek to `ord(first_char) * 8`.
- `records.bin`: fixed-size sorted records.
  - Current record size: 124 bytes.
  - Each record stores a UTF-8 term key inline, plus offsets into `strings.bin`.
  - Includes a 1-byte result tier.
  - Sorted by term, then tier, so a bucket can be binary-searched with fixed
    record seeks and normal results appear before obscure ones.
- `strings.bin`: readings and flattened definitions.
  - Only the strings for returned matches are read.
- `manifest.json`: version and sizing metadata.

Full ranked output for the current sample:

- `buckets.bin`: 8.5 MB
- `records.bin`: 51 MB
- `strings.bin`: 37 MB
- total: about 97 MB

Result tiers:

- `0`: common/high-priority modern (`score >= 0`)
- `1`: modern but lower-priority (`score < 0`)
- `2`: rare or dated
- `3`: archaic, obsolete, or old-form redirect/variant

The converter can still prune at preprocessing time, mostly for comparison:

```sh
python3 scripts/build_japanese_dict.py convert test/jitendex-yomitan test/jitendex-cpdict-modern \
  --drop-tags arch,obs,rare,dated --drop-negative-sequence

python3 scripts/build_japanese_dict.py convert test/jitendex-yomitan test/jitendex-cpdict-common \
  --min-score 0 --drop-tags arch,obs,rare,dated --drop-negative-sequence
```

Generated outputs:

- `ranked`: 430,822 entries, about 97 MB.
- `modern`: 283,956 entries, about 66 MB.

The stricter `common` command above is still available for comparison; the old
120-byte-record `common` sample was removed.

By default examples are stripped from flattened definitions. Add
`--include-examples` if examples are wanted in the output blob.

## Lookup Shape

The reader cursor now supplies context text starting at the selected kanji.
The dictionary lookup should:

1. Read the bucket for the first Unicode codepoint.
2. Generate prefixes from the selected context.
3. Search longest prefixes first.
4. Return exact term matches before phase-3 deinflection exists.

This fits the kanji-first cursor because most selected buckets are small. Even
large kanji buckets are a few thousand records, and binary search keeps reads
bounded.

## Build Commands

```sh
python3 scripts/build_japanese_dict.py convert test/jitendex-yomitan test/jitendex-cpdict-ranked
python3 scripts/build_japanese_dict.py lookup test/jitendex-cpdict-ranked 日本語
python3 scripts/build_japanese_dict.py bench test/jitendex-cpdict-ranked --samples 2000
```

Current host benchmark:

- p50: about 62 us
- p95: about 166 us
- max: about 347 us

These are not SD-card timings. The next useful firmware step is a read-only
`CpDict` loader plus a debug benchmark that writes rows like this to
`/.crosspoint/dict_bench.csv`:

```csv
query,bucket_us,search_us,string_us,total_us,matches
日本語,120,2400,900,3420,9
```

## Firmware Probe

`JapaneseDictionary` currently looks for these SD-card directories, in order:

- `/.crosspoint/dicts/jitendex-cpdict-ranked`
- `/.crosspoint/dicts/jitendex-cpdict-modern`
- `/dict/jitendex-cpdict-ranked`
- `/dict/jitendex-cpdict-modern`
- `/jitendex-cpdict-ranked`
- `/jitendex-cpdict-modern`

Copy the generated `buckets.bin`, `records.bin`, and `strings.bin` together
under one of those directories. The kanji cursor popup performs a basic
longest-prefix exact lookup and appends telemetry to:

```text
/.crosspoint/dict_bench.csv
```

The first firmware probe opens the files on each popup lookup. That makes
`open_us` visible and intentionally exposes the worst case; if timings look
acceptable with this, caching open files later should only help.
