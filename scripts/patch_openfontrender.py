"""
PlatformIO pre-build script: make OpenFontRender use ESP32 PSRAM allocators.

Upstream checks CONFIG_SPIRAM_SUPPORT, but this project's ESP32-S3 builds use
CONFIG_SPIRAM. Patch the libdep copy idempotently so FreeType allocations can
land in PSRAM.
"""

Import("env")  # noqa: F821
import os


def patch_openfontrender(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return

    for env_name in os.listdir(libdeps_dir):
        header = os.path.join(libdeps_dir, env_name, "OpenFontRender", "src", "FileSupport.h")
        if not os.path.isfile(header):
            continue
        _patch_header(header)


def _patch_header(header):
    with open(header, "r", encoding="utf-8") as fh:
        text = fh.read()

    original = text
    if "#include <sdkconfig.h>" not in text:
        text = text.replace("#include <cstddef>\n", "#include <cstddef>\n#include <sdkconfig.h>\n")

    psram_decls = (
        "#if defined(ARDUINO_ARCH_ESP32)\n"
        "extern \"C\" void* ps_malloc(size_t size);\n"
        "extern \"C\" void* ps_calloc(size_t n, size_t size);\n"
        "extern \"C\" void* ps_realloc(void* ptr, size_t size);\n"
        "#endif\n\n"
    )
    if "extern \"C\" void* ps_malloc(size_t size);" not in text:
        text = text.replace("#define FT_FILE void\n", psram_decls + "#define FT_FILE void\n")

    text = text.replace("#ifdef CONFIG_SPIRAM_SUPPORT", "#if defined(CONFIG_SPIRAM_SUPPORT) || defined(CONFIG_SPIRAM)")

    if text != original:
        with open(header, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)
        print("Patched OpenFontRender FileSupport.h for ESP32 PSRAM")


patch_openfontrender(env)  # noqa: F821
