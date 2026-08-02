#!/usr/bin/env python3
"""
Generate a resampled, merged epdiy waveform for the HZ5.2 panel.

Two transformations, both of which exist because this board's content is strictly
black and white (GfxRenderer is 1bpp; push() expands to 4bpp levels 0x0 and 0xF only)
and because the ESP32-S3 LCD render path drives every waveform phase for the same
fixed time.

1. Resampling
-------------
epdiy's waveform tables carry a per-phase on-time (`EpdWaveformPhases.phase_times`),
and ED097TC2's are heavily tail-weighted -- GC16's first half is
`15,8,8,8,8,8,10,10,10,10,20,20,50,100,200`, so the final settle phase is meant to be
41% of the half-waveform.

On the ESP32-S3 those times are never read. `phase_times` is consumed in exactly one
place, `output_i2s/render_i2s.c`; the LCD render path this board uses gives every phase
one panel scan at fixed line timing, so all phases get an equal ~24.4 ms. The waveform
that reaches the glass is therefore a badly distorted version of the designed one, with
the short leading phases hugely over-weighted and the long final settle starved (6.7% of
the time instead of 41%). A pixel that never gets its long final drive lands short of the
rail.

Since every output frame costs the same, holding a phase longer means *repeating* it.
This resamples the designed timeline onto a fixed number of equal frames: for output
frame i of N, take the phase covering t = (i + 0.5) * T / N on the cumulative time axis.

2. Merging the GL16 halves
--------------------------
Decoded for pure B/W content, GL16 is exactly mode 16 (WHITE_TO_GL16) concatenated with
mode 17 (BLACK_TO_GL16):

           W->W   W->B      B->W       B->B
  GL16      --    darken    lighten     --
                  (0-14)    (15-29)

So each pixel is idle for half the frames -- a W->B pixel does nothing during 15-29, a
B->W pixel nothing during 0-14. For greyscale content that sequencing is required, since
intermediate levels need both halves. For binary content the two active classes are
disjoint, and the panel drives every pixel independently (2 bits per pixel per frame), so
the halves can be superimposed instead of sequenced: 15 frames of identical per-pixel
drive rather than 30.

**The merge is only valid while the content stays binary, and this is load-bearing.**
Counted over all 256 (from, to) transitions, real GL16 drives 224 of them in *both*
halves -- a mid-grey to mid-grey pixel genuinely needs drive-to-rail then drive-to-target,
in sequence. Only 30 transitions are single-half, and those are exactly the ones starting
from a rail, which is all binary content can reach. Worse, modes 16/17 only cover those
same 30 transitions at all, so a greyscale pixel would get *no drive whatsoever* from the
merged table, not merely a degraded one. If the 4bpp render path lands (milestone 9),
regenerate with --no-merge.

GC16 is never merged. Its W->W entry needs both halves -- darken then lighten is what
re-seats the unchanged background, and is the visible flash.

Usage
-----
    python scripts/gen_hz52_waveform.py \\
        .pio/libdeps/hz52/epdiy/src/waveforms/epdiy_ED097TC2.h \\
        -o src/hz52_waveform.h

Frame count is the only thing that costs time on this hardware (~24.4 ms each), so
`--frames` / `--gl-frames` are the quality-vs-speed knobs. Defaults reproduce ~0.74 s for
the GC16 interval refresh and ~0.37 s for a GL16 page turn.
"""

import argparse
import re
import sys

ENTRIES_PER_PHASE = 16
BYTES_PER_ENTRY = 4
BYTES_PER_PHASE = ENTRIES_PER_PHASE * BYTES_PER_ENTRY

MODE_DU = 1
MODE_GC16 = 2
MODE_GL16 = 5
MODE_WHITE_TO_GL16 = 16
MODE_BLACK_TO_GL16 = 17
MODE_NAMES = {
    MODE_DU: "DU",
    MODE_GC16: "GC16",
    MODE_GL16: "GL16",
    MODE_WHITE_TO_GL16: "WHITE_TO_GL16",
    MODE_BLACK_TO_GL16: "BLACK_TO_GL16",
}

BLACK, WHITE = 0, ENTRIES_PER_PHASE - 1


def parse_mode(src, waveform, mode):
    """Return (phase_times, phases) for one mode, phases as a list of 64-byte rows."""
    times_m = re.search(
        rf"const int epd_wp_{waveform}_{mode}_0_times\[(\d+)\] = \{{([^}}]*)\}};", src
    )
    data_m = re.search(
        rf"const uint8_t epd_wp_{waveform}_{mode}_0_data\[(\d+)\]\[(\d+)\]\[(\d+)\] = (.*?);\n",
        src,
        re.S,
    )
    if not times_m or not data_m:
        return None

    times = [int(x) for x in times_m.group(2).split(",") if x.strip()]
    nphase, nentry, nbyte = (int(data_m.group(i)) for i in (1, 2, 3))
    if (nentry, nbyte) != (ENTRIES_PER_PHASE, BYTES_PER_ENTRY):
        sys.exit(f"unexpected LUT shape [{nphase}][{nentry}][{nbyte}]")

    flat = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", data_m.group(4))]
    if len(flat) != nphase * BYTES_PER_PHASE:
        sys.exit(f"mode {mode}: expected {nphase * BYTES_PER_PHASE} bytes, got {len(flat)}")
    if len(times) != nphase:
        sys.exit(f"mode {mode}: {len(times)} times for {nphase} phases")

    phases = [flat[p * BYTES_PER_PHASE : (p + 1) * BYTES_PER_PHASE] for p in range(nphase)]
    return times, phases


def resample(times, frames):
    """Pick `frames` phase indices along the cumulative time axis, midpoint sampling."""
    total = sum(times)
    cumulative, running = [], 0
    for t in times:
        running += t
        cumulative.append(running)

    picked = []
    for i in range(frames):
        t = (i + 0.5) * total / frames
        for p, edge in enumerate(cumulative):
            if t < edge:
                picked.append(p)
                break
        else:
            picked.append(len(times) - 1)
    return picked


def entry(row, to, frm):
    """Read the 2-bit drive for one (to, from) transition out of a 64-byte phase row."""
    return (row[to * BYTES_PER_ENTRY + frm // 4] >> ((3 - (frm % 4)) * 2)) & 0b11


def merge_rows(a, b):
    """Superimpose two phase rows: take a's drive where it is non-zero, else b's.

    Returns (merged_row, conflicts) where conflicts counts entries both rows drive
    differently. Only the four B/W corners can ever be consulted here, since the
    framebuffer holds levels 0x0 and 0xF exclusively, so conflicts in the greyscale
    interior are harmless -- but they are counted so a real overlap cannot pass silently.
    """
    merged = [0] * BYTES_PER_PHASE
    conflicts = 0
    for to in range(ENTRIES_PER_PHASE):
        for frm in range(ENTRIES_PER_PHASE):
            ea, eb = entry(a, to, frm), entry(b, to, frm)
            if ea and eb and ea != eb:
                conflicts += 1
            value = ea if ea else eb
            idx = to * BYTES_PER_ENTRY + frm // 4
            merged[idx] |= value << ((3 - (frm % 4)) * 2)
    return merged, conflicts


def corner_conflicts(rows_a, rows_b):
    """Count conflicts restricted to the four transitions binary content can reach."""
    corners = ((WHITE, WHITE), (BLACK, WHITE), (WHITE, BLACK), (BLACK, BLACK))
    total = 0
    for a, b in zip(rows_a, rows_b):
        for to, frm in corners:
            ea, eb = entry(a, to, frm), entry(b, to, frm)
            if ea and eb and ea != eb:
                total += 1
    return total


def emit_mode(out, name, mode, rows, per_frame, note):
    out.append(f"// mode {mode} ({MODE_NAMES.get(mode, '?')}): {len(rows)} frames -- {note}")
    out.append(
        f"const int {name}_{mode}_0_times[{len(rows)}] = {{ "
        + ",".join(str(per_frame) for _ in rows)
        + " };"
    )
    body = ",".join(
        "{"
        + ",".join(
            "{" + ",".join(f"0x{b:02x}" for b in row[e * 4 : e * 4 + 4]) + "}"
            for e in range(ENTRIES_PER_PHASE)
        )
        + "}"
        for row in rows
    )
    out.append(f"const uint8_t {name}_{mode}_0_data[{len(rows)}][16][4] = {{{body}}};")
    # Designator order must match EpdWaveformPhases' declaration order (phases, luts,
    # phase_times) -- C++ rejects out-of-order designated initializers, unlike C99.
    out.append(
        f"const EpdWaveformPhases {name}_{mode}_0 = {{ .phases = {len(rows)}, "
        f".luts = (const uint8_t*)&{name}_{mode}_0_data[0], "
        f".phase_times = &{name}_{mode}_0_times[0] }};"
    )
    out.append(f"const EpdWaveformPhases* {name}_{mode}_ranges[1] = {{ &{name}_{mode}_0 }};")
    out.append(
        f"const EpdWaveformMode {name}_{mode} = {{ .type = {mode}, .temp_ranges = 1, "
        f".range_data = &{name}_{mode}_ranges[0] }};"
    )
    out.append("")


def finish(out, args, intervals, total_conflicts):
    """Append the mode table and EpdWaveform wrapper, then write the header out."""
    modes = [MODE_DU, MODE_GC16, MODE_GL16]
    out.append(
        f"const EpdWaveformMode* {args.name}_modes[{len(modes)}] = {{ "
        + ",".join(f"&{args.name}_{m}" for m in modes)
        + " };"
    )
    out.append(
        f"const EpdWaveformTempInterval {args.name}_intervals[{intervals.group(1)}] = "
        f"{{{intervals.group(2)}}};"
    )
    out.append(
        f"const EpdWaveform {args.name} = {{ .num_modes = {len(modes)}, "
        f".num_temp_ranges = {intervals.group(1)}, "
        f".mode_data = &{args.name}_modes[0], "
        f".temp_intervals = &{args.name}_intervals[0] }};"
    )
    out.append("")

    with open(args.output, "w", encoding="utf-8") as fp:
        fp.write("\n".join(out))

    how = (
        "unmerged (greyscale-safe)"
        if total_conflicts is None
        else f"merged, {total_conflicts} greyscale-interior conflicts, 0 on B/W transitions"
    )
    print(
        f"wrote {args.output}: GC16 {args.frames} frames, GL16 {args.gl_frames} frames -- {how}"
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", help="path to an epdiy waveform header")
    ap.add_argument("--frames", type=int, default=30, help="frames for GC16 (default 30)")
    ap.add_argument(
        "--gl-frames", type=int, default=15, help="frames for merged GL16 (default 15)"
    )
    ap.add_argument(
        "--no-merge",
        action="store_true",
        help="emit real 30-phase GL16 instead of the merged halves. Required if the render "
        "path ever produces greyscale: the merge covers only rail-to-x transitions.",
    )
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--name", default="hz52_waveform")
    args = ap.parse_args()

    with open(args.source, encoding="utf-8") as fp:
        src = fp.read()
    waveform = re.search(r"const EpdWaveform (epdiy_\w+) = ", src).group(1)
    intervals = re.search(rf"{waveform}_intervals\[(\d+)\] = \{{([^;]*)\}};", src)

    wanted = [MODE_DU, MODE_GC16]
    wanted += [MODE_GL16] if args.no_merge else [MODE_WHITE_TO_GL16, MODE_BLACK_TO_GL16]
    parsed = {}
    for mode in wanted:
        got = parse_mode(src, waveform, mode)
        if got is None:
            sys.exit(f"{waveform} has no mode {mode} ({MODE_NAMES[mode]})")
        parsed[mode] = got

    out = [
        "// GENERATED by scripts/gen_hz52_waveform.py -- do not edit by hand.",
        f"//   source:  {args.source.split('/')[-1]} ({waveform})",
        f"//   command: gen_hz52_waveform.py <source> --frames {args.frames}"
        f" --gl-frames {args.gl_frames}",
        "//",
        "// GL16 is the two 15-phase halves (WHITE_TO_GL16, BLACK_TO_GL16) superimposed rather",
        "// than concatenated, which is valid because this board's content is strictly black and",
        "// white so the halves drive disjoint sets of pixels. GC16 keeps both halves in sequence:",
        "// its W->W entry needs darken *then* lighten. Both are resampled onto equal-duration",
        "// frames, because the ESP32-S3 LCD render path ignores phase_times. See the generator's",
        "// docstring.",
        "//",
        "// clang-format off  -- generated LUT data, reformatting it would churn every regen",
        "#pragma once",
        "",
        '#include "epd_internals.h"',
        "",
    ]

    # DU: flat phase times, so resampling is a no-op. Passed through untouched so the mode
    # stays available; it is not used for page turns (no reset stage, so it cannot clear).
    du_times, du_phases = parsed[MODE_DU]
    emit_mode(out, args.name, MODE_DU, du_phases, du_times[0], "passthrough, unresampled")

    gc_times, gc_phases = parsed[MODE_GC16]
    gc_picked = resample(gc_times, args.frames)
    emit_mode(
        out,
        args.name,
        MODE_GC16,
        [gc_phases[p] for p in gc_picked],
        round(sum(gc_times) / args.frames),
        f"resampled from {len(gc_phases)}, source phases {gc_picked}",
    )

    if args.no_merge:
        gl_times, gl_phases = parsed[MODE_GL16]
        gl_picked = resample(gl_times, args.gl_frames)
        emit_mode(
            out,
            args.name,
            MODE_GL16,
            [gl_phases[p] for p in gl_picked],
            round(sum(gl_times) / args.gl_frames),
            f"unmerged, resampled from {len(gl_phases)}, source phases {gl_picked}",
        )
        total_conflicts = None
        return finish(out, args, intervals, total_conflicts)

    w_times, w_phases = parsed[MODE_WHITE_TO_GL16]
    b_times, b_phases = parsed[MODE_BLACK_TO_GL16]
    w_picked = resample(w_times, args.gl_frames)
    b_picked = resample(b_times, args.gl_frames)
    w_rows = [w_phases[p] for p in w_picked]
    b_rows = [b_phases[p] for p in b_picked]

    corners = corner_conflicts(w_rows, b_rows)
    if corners:
        sys.exit(
            f"refusing to merge: {corners} conflicting drives on the four B/W transitions. "
            "The halves are not disjoint for this waveform."
        )

    merged, total_conflicts = [], 0
    for w_row, b_row in zip(w_rows, b_rows):
        row, conflicts = merge_rows(w_row, b_row)
        merged.append(row)
        total_conflicts += conflicts

    emit_mode(
        out,
        args.name,
        MODE_GL16,
        merged,
        round((sum(w_times) + sum(b_times)) / 2 / args.gl_frames),
        f"merged halves, source phases W{w_picked} B{b_picked}",
    )

    return finish(out, args, intervals, total_conflicts)


if __name__ == "__main__":
    main()
