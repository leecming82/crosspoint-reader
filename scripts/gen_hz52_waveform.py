#!/usr/bin/env python3
"""
Generate a resampled epdiy waveform for the HZ5.2 panel.

Why this exists
---------------
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
rail -- which is the residue signature this port has been chasing.

Since every output frame costs the same, holding a phase longer means *repeating* it.
This resamples the designed timeline onto a fixed number of equal frames: for output
frame i of N, take the phase covering t = (i + 0.5) * T / N on the cumulative time axis.
Long phases naturally win more frames, short ones may drop out entirely, and the drive
envelope the panel sees approximates what the waveform author intended.

Usage
-----
    python scripts/gen_hz52_waveform.py \
        .pio/libdeps/hz52/epdiy/src/waveforms/epdiy_ED097TC2.h \
        --frames 30 -o src/hz52_waveform.h

`--frames` is the knob: frame count is the only thing that costs time on this hardware
(~24.4 ms each), so 30 reproduces today's GC16 cost and lower values trade quality for
speed. DU is copied through unchanged -- it is not resampled, having flat phase times.
"""

import argparse
import re
import sys

SOURCE_MODES = {1: "DU", 2: "GC16", 5: "GL16"}
ENTRIES_PER_PHASE = 16
BYTES_PER_ENTRY = 4
BYTES_PER_PHASE = ENTRIES_PER_PHASE * BYTES_PER_ENTRY


def parse_mode(src, waveform, mode):
    """Return (phase_times, phases) for one mode, phases as a list of 64-byte rows."""
    times_m = re.search(
        r"const int epd_wp_%s_%d_0_times\[(\d+)\] = \{([^}]*)\};" % (waveform, mode), src
    )
    data_m = re.search(
        r"const uint8_t epd_wp_%s_%d_0_data\[(\d+)\]\[(\d+)\]\[(\d+)\] = (.*?);\n"
        % (waveform, mode),
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
    """Pick `frames` phases along the cumulative time axis, midpoint sampling."""
    total = sum(times)
    cumulative = []
    running = 0
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


def emit_mode(out, name, mode, picked, times, phases):
    per_frame = sum(times) / len(picked)
    out.append(f"// mode {mode} ({SOURCE_MODES.get(mode, '?')}): {len(picked)} frames")
    out.append(f"// source phase index per frame: {picked}")
    out.append(
        f"const int {name}_{mode}_0_times[{len(picked)}] = {{ "
        + ",".join(str(round(per_frame)) for _ in picked)
        + " };"
    )
    rows = []
    for p in picked:
        row = phases[p]
        entries = [
            "{" + ",".join(f"0x{b:02x}" for b in row[e * 4 : e * 4 + 4]) + "}"
            for e in range(ENTRIES_PER_PHASE)
        ]
        rows.append("{" + ",".join(entries) + "}")
    out.append(
        f"const uint8_t {name}_{mode}_0_data[{len(picked)}][16][4] = {{"
        + ",".join(rows)
        + "};"
    )
    # Designator order must match EpdWaveformPhases' declaration order (phases, luts,
    # phase_times) -- C++ rejects out-of-order designated initializers, unlike C99.
    out.append(
        f"const EpdWaveformPhases {name}_{mode}_0 = {{ .phases = {len(picked)}, "
        f".luts = (const uint8_t*)&{name}_{mode}_0_data[0], "
        f".phase_times = &{name}_{mode}_0_times[0] }};"
    )
    out.append(
        f"const EpdWaveformPhases* {name}_{mode}_ranges[1] = {{ &{name}_{mode}_0 }};"
    )
    out.append(
        f"const EpdWaveformMode {name}_{mode} = {{ .type = {mode}, .temp_ranges = 1, "
        f".range_data = &{name}_{mode}_ranges[0] }};"
    )
    out.append("")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", help="path to epdiy_ED097TC2.h")
    ap.add_argument("--frames", type=int, default=30, help="frames for GC16 (default 30)")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--name", default="hz52_waveform")
    args = ap.parse_args()

    with open(args.source, encoding="utf-8") as fp:
        src = fp.read()
    waveform = re.search(r"const EpdWaveform (epdiy_\w+) = ", src).group(1)
    intervals = re.search(r"%s_intervals\[(\d+)\] = \{([^;]*)\};" % waveform, src)

    out = [
        "// GENERATED by scripts/gen_hz52_waveform.py -- do not edit by hand.",
        f"//   source:  {args.source.split('/')[-1]} ({waveform})",
        f"//   command: gen_hz52_waveform.py <source> --frames {args.frames}",
        "//",
        "// Resamples the designed phase timeline onto equal-duration frames, because the",
        "// ESP32-S3 LCD render path ignores EpdWaveformPhases.phase_times entirely (only",
        "// output_i2s/render_i2s.c reads it) and gives every phase the same ~24.4 ms. See the",
        "// generator's docstring for the reasoning.",
        "//",
        "// clang-format off  -- generated LUT data, reformatting it would churn every regen",
        "#pragma once",
        "",
        '#include "epd_internals.h"',
        "",
    ]

    modes = []
    for mode in sorted(SOURCE_MODES):
        parsed = parse_mode(src, waveform, mode)
        if parsed is None:
            continue
        times, phases = parsed
        # DU has flat phase times, so resampling it is a no-op; pass it through untouched
        # to keep it available as a fallback.
        frames = len(phases) if mode == 1 else args.frames
        picked = resample(times, frames)
        emit_mode(out, args.name, mode, picked, times, phases)
        modes.append(mode)

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
    print(f"wrote {args.output}: modes {modes}, GC16 frames {args.frames}")


if __name__ == "__main__":
    main()
