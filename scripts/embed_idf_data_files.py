"""
PlatformIO post-load script: generate the assembly stubs for ESP-IDF components
that embed a data file, so they exist before SCons assembles them.

Only needed when an environment sets `custom_sdkconfig`, which makes pioarduino
rebuild the Arduino IDF libs from source (see the hz52 env in platformio.ini).
In that mode PlatformIO drives the build with SCons rather than ninja, and it
reimplements ESP-IDF's `target_add_binary_data()` for exactly one case, the
mbedtls certificate bundle (espidf.py: generate_mbedtls_bundle). Every other
component that embeds a file -- esp_insights and esp_rainmaker embed server
certificates -- gets a source-less `<name>.S` in the code model, and the build
dies with "Source `.pio/build/<env>/x.S' not found".

CMake still emits the correct recipe into build.ninja, so we read the recipes
back out and register an equivalent SCons builder for each. The alternative,
dropping the offending components, would also drop Arduino libraries that have
no `CONFIG_ARDUINO_SELECTIVE_*` symbol to re-enable them (USB, HTTPUpdate,
ESP_NOW, ESP_I2S, ESP_HostedOTA), leaving hz52 with a different Arduino surface
to the murphy_m4 baseline.
"""

import os
import re

Import("env")  # noqa: F821  -- provided by PlatformIO at script load

BUILD_DIR = env.subst("$BUILD_DIR")  # noqa: F821
NINJA_BUILDFILE = os.path.join(BUILD_DIR, "build.ninja")

# cmake -D DATA_FILE=<in> -D SOURCE_FILE=<out> -D FILE_TYPE=<TEXT|BINARY> -P <script>
EMBED_COMMAND_RE = re.compile(
    r"^\s*COMMAND = .*?&& (?P<cmake>\S+) "
    r"-D DATA_FILE=(?P<data_file>\S+) "
    r"-D SOURCE_FILE=(?P<source_file>\S+) "
    r"-D FILE_TYPE=(?P<file_type>\S+) "
    r"-P (?P<script>\S+data_file_embed_asm\.cmake)\s*$"
)

# PlatformIO generates this one itself, immediately via env.Execute rather than as
# a node, so leave it alone -- a second recipe for it would be a duplicate.
SKIP_SOURCE_FILES = {"x509_crt_bundle.S"}


def register_embed_builders():
    if not os.path.isfile(NINJA_BUILDFILE):
        # Prebuilt-Arduino-libs build (e.g. murphy_m4): no CMake stage, nothing to do.
        return

    with open(NINJA_BUILDFILE, encoding="utf-8") as fp:
        recipes = [m.groupdict() for m in map(EMBED_COMMAND_RE.match, fp) if m]

    for recipe in recipes:
        source_file = recipe["source_file"]
        if os.path.basename(source_file) in SKIP_SOURCE_FILES:
            continue
        if not os.path.isfile(recipe["data_file"]):
            print(f"*** Skipping embed of missing data file {recipe['data_file']} ***")
            continue

        env.Command(  # noqa: F821
            source_file,
            recipe["data_file"],
            env.VerboseAction(  # noqa: F821
                " ".join(
                    [
                        f'"{recipe["cmake"]}"',
                        f'-D DATA_FILE="{recipe["data_file"]}"',
                        f'-D SOURCE_FILE="{source_file}"',
                        f'-D FILE_TYPE={recipe["file_type"]}',
                        "-P",
                        f'"{recipe["script"]}"',
                    ]
                ),
                f"Generating assembly for {os.path.basename(recipe['data_file'])}...",
            ),
        )


register_embed_builders()
