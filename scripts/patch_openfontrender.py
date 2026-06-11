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
        src_dir = os.path.join(libdeps_dir, env_name, "OpenFontRender", "src")
        header = os.path.join(src_dir, "FileSupport.h")
        ofr_header = os.path.join(src_dir, "OpenFontRender.h")
        ofr_cpp = os.path.join(src_dir, "OpenFontRender.cpp")
        if not os.path.isfile(header):
            continue
        _patch_header(header)
        if os.path.isfile(ofr_header) and os.path.isfile(ofr_cpp):
            _patch_glyph_bitmap_api(ofr_header, ofr_cpp)


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


def _patch_glyph_bitmap_api(header, cpp):
    with open(header, "r", encoding="utf-8") as fh:
        header_text = fh.read()

    header_original = header_text
    if "struct GlyphBitmap {" not in header_text:
        marker = "\tuint32_t getTextHeight(const char *fmt, ...);\n"
        addition = (
            "\n"
            "\tstruct GlyphBitmap {\n"
            "\t\tuint32_t glyph_index = 0;\n"
            "\t\tint left = 0;\n"
            "\t\tint top = 0;\n"
            "\t\tint width = 0;\n"
            "\t\tint rows = 0;\n"
            "\t\tint pitch = 0;\n"
            "\t\tint pixel_mode = 0;\n"
            "\t\tint advance_x = 0;\n"
            "\t\tconst unsigned char *buffer = nullptr;\n"
            "\t};\n"
            "\tFT_Error renderGlyphBitmap(uint32_t unicode, GlyphBitmap &out);\n"
        )
        if marker in header_text:
            header_text = header_text.replace(marker, marker + addition)

    if header_text != header_original:
        with open(header, "w", encoding="utf-8", newline="") as fh:
            fh.write(header_text)
        print("Patched OpenFontRender.h with glyph bitmap API")

    with open(cpp, "r", encoding="utf-8") as fh:
        cpp_text = fh.read()

    cpp_original = cpp_text
    if "OpenFontRender::renderGlyphBitmap" not in cpp_text:
        marker = "uint32_t OpenFontRender::getTextHeight(const char *fmt, ...) {\n"
        start = cpp_text.find(marker)
        if start != -1:
            next_marker = cpp_text.find("\n/*!\n * @brief Calculates the maximum font size", start)
            if next_marker != -1:
                method = (
                    "\n"
                    "FT_Error OpenFontRender::renderGlyphBitmap(uint32_t unicode, GlyphBitmap &out) {\n"
                    "\tout = GlyphBitmap{};\n"
                    "\tFT_Size asize = NULL;\n"
                    "\tFTC_ScalerRec scaler;\n"
                    "\tscaler.face_id = &_face_id;\n"
                    "\tscaler.width = 0;\n"
                    "\tscaler.height = _text.size;\n"
                    "\tscaler.pixel = true;\n"
                    "\tFT_Error error = FTC_Manager_LookupSize(_ftc_manager, &scaler, &asize);\n"
                    "\tif (error) return error;\n"
                    "\tFT_Int cmap_index = FT_Get_Charmap_Index(asize->face->charmap);\n"
                    "\tFT_UInt glyph_index = FTC_CMapCache_Lookup(_ftc_cmap_cache, &_face_id, cmap_index, unicode);\n"
                    "\tif (glyph_index == 0) return FT_Err_Invalid_Glyph_Index;\n"
                    "\tFTC_ImageTypeRec image_type;\n"
                    "\timage_type.face_id = scaler.face_id;\n"
                    "\timage_type.width = scaler.width;\n"
                    "\timage_type.height = scaler.height;\n"
                    "\timage_type.flags = FT_LOAD_RENDER;\n"
                    "\tFT_Glyph aglyph;\n"
                    "\terror = FTC_ImageCache_Lookup(_ftc_image_cache, &image_type, glyph_index, &aglyph, NULL);\n"
                    "\tif (error) return error;\n"
                    "\tif (aglyph->format != FT_GLYPH_FORMAT_BITMAP) {\n"
                    "\t\terror = FT_Glyph_To_Bitmap(&aglyph, FT_RENDER_MODE_NORMAL, nullptr, true);\n"
                    "\t\tif (error) return error;\n"
                    "\t}\n"
                    "\tFT_BitmapGlyph bit = (FT_BitmapGlyph)aglyph;\n"
                    "\tout.glyph_index = glyph_index;\n"
                    "\tout.left = bit->left;\n"
                    "\tout.top = bit->top;\n"
                    "\tout.width = bit->bitmap.width;\n"
                    "\tout.rows = bit->bitmap.rows;\n"
                    "\tout.pitch = bit->bitmap.pitch;\n"
                    "\tout.pixel_mode = bit->bitmap.pixel_mode;\n"
                    "\tout.advance_x = aglyph->advance.x >> 16;\n"
                    "\tout.buffer = bit->bitmap.buffer;\n"
                    "\treturn FT_Err_Ok;\n"
                    "}\n"
                )
                cpp_text = cpp_text[:next_marker] + method + cpp_text[next_marker:]

    if cpp_text != cpp_original:
        with open(cpp, "w", encoding="utf-8", newline="") as fh:
            fh.write(cpp_text)
        print("Patched OpenFontRender.cpp with glyph bitmap API")


patch_openfontrender(env)  # noqa: F821
