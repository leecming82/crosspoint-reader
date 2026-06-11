#!/usr/bin/env python3
"""Create static TTF instances from variable fonts."""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont


def parse_axis(value: str) -> tuple[str, float]:
    if "=" not in value:
        raise argparse.ArgumentTypeError(f"axis must be TAG=VALUE, got {value!r}")
    tag, raw = value.split("=", 1)
    tag = tag.strip()
    if len(tag) != 4:
        raise argparse.ArgumentTypeError(f"axis tag must be 4 characters, got {tag!r}")
    try:
        axis_value = float(raw)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"axis value must be numeric, got {raw!r}") from exc
    return tag, axis_value


def axis_label(axes: dict[str, float]) -> str:
    parts: list[str] = []
    for tag, value in sorted(axes.items()):
        if value.is_integer():
            parts.append(f"{tag}{int(value)}")
        else:
            parts.append(f"{tag}{value:g}".replace(".", "p"))
    return "-".join(parts)


def output_name(source: Path, axes: dict[str, float]) -> str:
    stem = source.stem
    for suffix in ("-VariableFont_wght", "_VariableFont_wght", "-VariableFont", "_VariableFont"):
        if stem.endswith(suffix):
            stem = stem[: -len(suffix)]
            break
    return f"{stem}-{axis_label(axes)}.ttf"


def axis_map(font: TTFont) -> dict[str, tuple[float, float, float]]:
    if "fvar" not in font:
        return {}
    return {
        axis.axisTag: (float(axis.minValue), float(axis.defaultValue), float(axis.maxValue))
        for axis in font["fvar"].axes
    }


def instantiate(source: Path, destination: Path, requested_axes: dict[str, float], overwrite: bool) -> None:
    if destination.exists() and not overwrite:
        raise FileExistsError(f"{destination} already exists; pass --overwrite to replace it")

    source_font = TTFont(str(source))
    try:
        available_axes = axis_map(source_font)
        if not available_axes:
            raise ValueError(f"{source} is not a variable font")

        axes: dict[str, float] = {}
        for tag, requested in requested_axes.items():
            if tag not in available_axes:
                known = ", ".join(sorted(available_axes))
                raise ValueError(f"{source} has no {tag!r} axis; available axes: {known}")
            minimum, _, maximum = available_axes[tag]
            clamped = min(max(requested, minimum), maximum)
            if clamped != requested:
                print(f"{source}: clamped {tag}={requested:g} to {clamped:g}", file=sys.stderr)
            axes[tag] = clamped

        destination.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp_name = tempfile.mkstemp(prefix=f".{destination.name}.", suffix=".tmp", dir=destination.parent)
        os.close(fd)
        tmp_path = Path(tmp_name)
        try:
            static_font = instantiateVariableFont(source_font, axes, updateFontNames=True, optimize=False)
            try:
                static_font.save(str(tmp_path))
            finally:
                static_font.close()
            tmp_path.replace(destination)
            destination.chmod(0o644)
        except Exception:
            tmp_path.unlink(missing_ok=True)
            raise

        axis_desc = ", ".join(f"{tag}={value:g}" for tag, value in sorted(axes.items()))
        print(f"{source} -> {destination} ({axis_desc})")
    finally:
        source_font.close()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fonts", nargs="+", type=Path, help="Variable TTF files to instantiate")
    parser.add_argument("--axis", action="append", type=parse_axis, default=[], help="Axis assignment, e.g. wght=400")
    parser.add_argument("--weight", type=float, help="Convenience alias for --axis wght=VALUE")
    parser.add_argument("--output-dir", type=Path, help="Output directory. Defaults to each source file directory")
    parser.add_argument("--overwrite", action="store_true", help="Replace existing output files")
    args = parser.parse_args(argv)

    axes = dict(args.axis)
    if args.weight is not None:
        axes["wght"] = args.weight
    if not axes:
        parser.error("provide --weight VALUE or at least one --axis TAG=VALUE")

    for source in args.fonts:
        source = source.expanduser().resolve()
        out_dir = args.output_dir.expanduser().resolve() if args.output_dir else source.parent
        destination = out_dir / output_name(source, axes)
        instantiate(source, destination, axes, args.overwrite)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
