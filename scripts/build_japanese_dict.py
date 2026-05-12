#!/usr/bin/env python3
import argparse
import glob
import json
import os
import random
import struct
import time

UNICODE_BUCKETS = 0x110000
KEY_BYTES = 96
RECORD_STRUCT = struct.Struct("<96sH B B I H H I I i i")
BUCKET_STRUCT = struct.Struct("<II")
TIER_COMMON = 0
TIER_MODERN = 1
TIER_RARE = 2
TIER_ARCHAIC = 3
SKIP_CONTENT_KINDS = {
    "attribution",
    "forms",
    "xref",
    "xref-content",
    "xref-glossary",
    "antonym",
    "antonym-content",
    "antonym-glossary",
    "reference-label",
}


def safe_prefix_bytes(text, limit):
    data = text.encode("utf-8")
    if len(data) <= limit:
        return data
    cut = limit
    while cut > 0 and (data[cut] & 0xC0) == 0x80:
        cut -= 1
    return data[:cut]


def text_content(node, out):
    if node is None:
        return
    if isinstance(node, str):
        if node.strip():
            out.append(node.strip())
        return
    if isinstance(node, list):
        for item in node:
            text_content(item, out)
        return
    if not isinstance(node, dict):
        return

    data = node.get("data")
    if isinstance(data, dict) and data.get("class") == "tag":
        return
    content_kind = data.get("content") if isinstance(data, dict) else None
    if content_kind in SKIP_CONTENT_KINDS:
        return
    text_content(node.get("content"), out)


def tag_codes(node, out):
    if isinstance(node, dict):
        data = node.get("data")
        if isinstance(data, dict) and data.get("class") == "tag":
            code = data.get("code")
            if code:
                out.add(code)
        for value in node.values():
            tag_codes(value, out)
    elif isinstance(node, list):
        for value in node:
            tag_codes(value, out)


def tag_label(code, content):
    if code == "vs" or content == "suru":
        return "suru verb"
    if code == "vt":
        return "transitive"
    if code == "vi":
        return "intransitive"
    if code == "uk":
        return "usually kana"
    return content.strip() if isinstance(content, str) else ""


def tag_labels(node, out):
    if isinstance(node, dict):
        data = node.get("data")
        if isinstance(data, dict) and data.get("class") == "tag":
            label = tag_label(data.get("code"), node.get("content"))
            if label and label not in out:
                out.append(label)
            return
        for value in node.values():
            tag_labels(value, out)
    elif isinstance(node, list):
        for value in node:
            tag_labels(value, out)


def flatten_glossary(glossary, include_examples=False):
    tags = []
    tag_labels(glossary, tags)
    pieces = []
    for item in glossary:
        if isinstance(item, dict):
            if include_examples:
                text_content(item.get("content"), pieces)
            else:
                text_content_without_examples(item.get("content"), pieces)
        else:
            text_content(item, pieces)
    compact = []
    for piece in tags + pieces:
        if not compact or compact[-1] != piece:
            compact.append(piece)
    return "; ".join(compact)


def redirect_target(glossary):
    for item in glossary or []:
        if not isinstance(item, list) or not item:
            continue
        if not isinstance(item[0], str) or len(item) < 2:
            continue
        notes = item[1]
        if isinstance(notes, list) and any(isinstance(note, str) and note.startswith("redirected from ") for note in notes):
            return item[0]
    return ""


def load_yomitan_entries(src_dir, include_examples=False):
    raw_entries = []
    paths = sorted(
        glob.glob(os.path.join(src_dir, "term_bank_*.json")),
        key=lambda p: int(os.path.basename(p).split("_")[-1].split(".")[0]),
    )
    for path in paths:
        with open(path, "r", encoding="utf-8") as f:
            for entry in json.load(f):
                term = entry[0] or ""
                if not term:
                    continue
                glossary = entry[5] or []
                tags = set()
                tag_codes(glossary, tags)
                score = int(entry[4] or 0)
                sequence = int(entry[6] or 0)
                raw_entries.append(
                    {
                        "term": term,
                        "reading": entry[1] or "",
                        "score": score,
                        "definition": flatten_glossary(glossary, include_examples=include_examples),
                        "sequence": sequence,
                        "tags": tags,
                        "tier": classify_tier(score, sequence, tags),
                        "redirect_target": redirect_target(glossary),
                    }
                )
    return raw_entries


def resolve_redirects(entries):
    by_term = {}
    by_sequence = {}
    for entry in entries:
        if entry["redirect_target"]:
            continue
        current = by_term.get(entry["term"])
        if current is None or (entry["tier"], -entry["score"], entry["reading"]) < (
            current["tier"],
            -current["score"],
            current["reading"],
        ):
            by_term[entry["term"]] = entry
        if entry["sequence"] > 0:
            current = by_sequence.get(entry["sequence"])
            if current is None or (entry["tier"], -entry["score"], entry["term"]) < (
                current["tier"],
                -current["score"],
                current["term"],
            ):
                by_sequence[entry["sequence"]] = entry

    resolved_count = 0
    unresolved_count = 0
    for entry in entries:
        target = entry["redirect_target"]
        if not target:
            continue
        canonical = by_term.get(target)
        if canonical is None and entry["sequence"] < 0:
            canonical = by_sequence.get(-entry["sequence"])
        if canonical is None:
            unresolved_count += 1
            continue

        entry["reading"] = entry["reading"] or canonical["reading"]
        entry["definition"] = canonical["definition"]
        entry["tags"] = set(entry["tags"]) | set(canonical["tags"])
        resolved_count += 1

    return resolved_count, unresolved_count


def text_content_without_examples(node, out):
    if node is None:
        return
    if isinstance(node, str):
        if node.strip():
            out.append(node.strip())
        return
    if isinstance(node, list):
        for item in node:
            text_content_without_examples(item, out)
        return
    if isinstance(node, dict):
        data = node.get("data")
        if isinstance(data, dict) and data.get("class") == "tag":
            return
        content_kind = data.get("content") if isinstance(data, dict) else None
        if content_kind in ("example-sentence", "example-sentence-a", "example-sentence-b", "example-keyword"):
            return
        if content_kind in SKIP_CONTENT_KINDS:
            return
        text_content_without_examples(node.get("content"), out)


def iter_yomitan_entries(src_dir, include_examples=False):
    entries = load_yomitan_entries(src_dir, include_examples=include_examples)
    resolve_redirects(entries)
    for entry in entries:
        yield entry


def classify_tier(score, sequence, tags):
    if sequence < 0 or tags.intersection({"arch", "obs"}):
        return TIER_ARCHAIC
    if tags.intersection({"rare", "dated"}):
        return TIER_RARE
    if score >= 0:
        return TIER_COMMON
    return TIER_MODERN


def convert(args):
    os.makedirs(args.out_dir, exist_ok=True)
    drop_tags = {tag.strip() for tag in args.drop_tags.split(",") if tag.strip()}
    entries = []
    skipped = {"min_score": 0, "tags": 0, "negative_sequence": 0}
    raw_entries = load_yomitan_entries(args.src_dir, include_examples=args.include_examples)
    resolved_redirects, unresolved_redirects = resolve_redirects(raw_entries)
    for entry in raw_entries:
        if args.min_score is not None and entry["score"] < args.min_score:
            skipped["min_score"] += 1
            continue
        if args.drop_negative_sequence and entry["sequence"] < 0:
            skipped["negative_sequence"] += 1
            continue
        if drop_tags and entry["tags"].intersection(drop_tags):
            skipped["tags"] += 1
            continue
        entries.append(entry)
    entries.sort(key=lambda e: (e["term"], e["tier"], -e["score"], e["reading"], e["sequence"]))

    buckets = [(0, 0)] * UNICODE_BUCKETS
    bucket_start = {}
    bucket_count = {}
    for idx, entry in enumerate(entries):
        cp = ord(entry["term"][0])
        bucket_start.setdefault(cp, idx)
        bucket_count[cp] = bucket_count.get(cp, 0) + 1
    for cp, start in bucket_start.items():
        buckets[cp] = (start, bucket_count[cp])

    with open(os.path.join(args.out_dir, "buckets.bin"), "wb") as f:
        for start, count in buckets:
            f.write(BUCKET_STRUCT.pack(start, count))

    with open(os.path.join(args.out_dir, "records.bin"), "wb") as records, open(
        os.path.join(args.out_dir, "strings.bin"), "wb"
    ) as strings:
        for entry in entries:
            key = safe_prefix_bytes(entry["term"], KEY_BYTES)
            reading = entry["reading"].encode("utf-8")
            definition = entry["definition"].encode("utf-8")
            reading_off = strings.tell()
            strings.write(reading)
            def_off = strings.tell()
            strings.write(definition)
            records.write(
                RECORD_STRUCT.pack(
                    key.ljust(KEY_BYTES, b"\0"),
                    len(key),
                    entry["tier"],
                    0,
                    reading_off,
                    len(reading),
                    0,
                    def_off,
                    len(definition),
                    entry["score"],
                    entry["sequence"],
                )
            )

    meta = {
        "format": "crosspoint-jpdict",
        "version": 2,
        "entry_count": len(entries),
        "record_size": RECORD_STRUCT.size,
        "key_bytes": KEY_BYTES,
        "unicode_buckets": UNICODE_BUCKETS,
        "tiers": {
            "0": "common/high-priority modern",
            "1": "modern",
            "2": "rare_or_dated",
            "3": "archaic_obsolete_or_old_variant",
        },
        "filters": {
            "min_score": args.min_score,
            "drop_tags": sorted(drop_tags),
            "drop_negative_sequence": args.drop_negative_sequence,
            "include_examples": args.include_examples,
            "skipped": skipped,
            "redirects": {
                "resolved": resolved_redirects,
                "unresolved": unresolved_redirects,
            },
        },
    }
    with open(os.path.join(args.out_dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)


class JapaneseDictFile:
    def __init__(self, path):
        self.path = path
        self.buckets = open(os.path.join(path, "buckets.bin"), "rb")
        self.records = open(os.path.join(path, "records.bin"), "rb")
        self.strings = open(os.path.join(path, "strings.bin"), "rb")

    def bucket(self, cp):
        self.buckets.seek(cp * BUCKET_STRUCT.size)
        return BUCKET_STRUCT.unpack(self.buckets.read(BUCKET_STRUCT.size))

    def record_key(self, index):
        self.records.seek(index * RECORD_STRUCT.size)
        rec = self.records.read(RECORD_STRUCT.size)
        if len(rec) != RECORD_STRUCT.size:
            return "", None
        fields = RECORD_STRUCT.unpack(rec)
        key = fields[0][: fields[1]].decode("utf-8")
        return key, fields

    def read_string(self, offset, length):
        self.strings.seek(offset)
        return self.strings.read(length).decode("utf-8", errors="replace")

    def find_exact(self, term):
        if not term:
            return []
        start, count = self.bucket(ord(term[0]))
        lo, hi = start, start + count
        while lo < hi:
            mid = (lo + hi) // 2
            key, _ = self.record_key(mid)
            if key < term:
                lo = mid + 1
            else:
                hi = mid

        results = []
        pos = lo
        while pos < start + count:
            key, fields = self.record_key(pos)
            if key != term:
                break
            results.append(
                {
                    "term": key,
                    "tier": fields[2],
                    "reading": self.read_string(fields[4], fields[5]),
                    "definition": self.read_string(fields[7], fields[8]),
                    "score": fields[9],
                    "sequence": fields[10],
                }
            )
            pos += 1
        return results

    def lookup_context(self, context, max_chars=24):
        prefixes = [context[:i] for i in range(1, min(len(context), max_chars) + 1)]
        matches = []
        for prefix in reversed(prefixes):
            found = self.find_exact(prefix)
            if found:
                matches.extend(found)
        matches.sort(key=lambda e: (-len(e["term"]), e["tier"], -e["score"], e["term"]))
        return matches


def lookup(args):
    dictionary = JapaneseDictFile(args.dict_dir)
    start = time.perf_counter_ns()
    matches = dictionary.lookup_context(args.query)
    elapsed_us = (time.perf_counter_ns() - start) / 1000
    print(f"{len(matches)} matches in {elapsed_us:.1f} us")
    for match in matches[: args.limit]:
        print(
            f"{match['term']} [{match['reading']}] tier={match['tier']} "
            f"score={match['score']} seq={match['sequence']}"
        )
        print(f"  {match['definition'][:240]}")


def bench(args):
    dictionary = JapaneseDictFile(args.dict_dir)
    samples = []
    with open(os.path.join(args.dict_dir, "records.bin"), "rb") as records:
        size = os.path.getsize(os.path.join(args.dict_dir, "records.bin"))
        count = size // RECORD_STRUCT.size
        for _ in range(args.samples):
            idx = random.randrange(count)
            records.seek(idx * RECORD_STRUCT.size)
            fields = RECORD_STRUCT.unpack(records.read(RECORD_STRUCT.size))
            samples.append(fields[0][: fields[1]].decode("utf-8"))

    timings = []
    for sample in samples:
        start = time.perf_counter_ns()
        dictionary.lookup_context(sample)
        timings.append((time.perf_counter_ns() - start) / 1000)
    timings.sort()
    print(f"samples={len(timings)}")
    print(f"p50={timings[len(timings)//2]:.1f} us")
    print(f"p95={timings[int(len(timings)*0.95)]:.1f} us")
    print(f"max={timings[-1]:.1f} us")


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(required=True)
    p = sub.add_parser("convert")
    p.add_argument("src_dir")
    p.add_argument("out_dir")
    p.add_argument("--min-score", type=int)
    p.add_argument("--drop-tags", default="")
    p.add_argument("--drop-negative-sequence", action="store_true")
    p.add_argument("--include-examples", action="store_true")
    p.set_defaults(func=convert)
    p = sub.add_parser("lookup")
    p.add_argument("dict_dir")
    p.add_argument("query")
    p.add_argument("--limit", type=int, default=5)
    p.set_defaults(func=lookup)
    p = sub.add_parser("bench")
    p.add_argument("dict_dir")
    p.add_argument("--samples", type=int, default=1000)
    p.set_defaults(func=bench)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
