#include <yetty/font/bm-font.hpp>
#include <yetty/gpu-resource-set.hpp>
#include <ytrace/ytrace.hpp>

// Android/Emscripten: stub implementation without FreeType/fontconfig
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
#define YETTY_BM_FONT_STUB 1
#else
#define YETTY_BM_FONT_STUB 0
#endif

#if !YETTY_BM_FONT_STUB
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_BITMAP_H
#include FT_SFNT_NAMES_H

#ifdef YETTY_USE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif
#endif // !YETTY_BM_FONT_STUB

#include <cstring>
#include <algorithm>

namespace yetty {

// =============================================================================
// Stub implementation for Android/Emscripten
// =============================================================================
#if YETTY_BM_FONT_STUB

BmFont::BmFont(const std::string& fontPath, uint32_t glyphSize, bool shared)
    : _fontPath(fontPath), _glyphSize(glyphSize), _shared(shared) {
    _glyphsPerRow = _atlasSize / _glyphSize;
}

BmFont::~BmFont() {
}

Result<void> BmFont::init() {
    ydebug("BmFont: stub implementation (no FreeType support)");
    _atlasData.resize(_atlasSize * _atlasSize * 4, 0);
    return Ok();
}

Result<void> BmFont::findFont() {
    return Err<void>("Font not available on this platform");
}

Result<void> BmFont::loadCommonGlyphs() {
    ydebug("BmFont: stub - no glyphs to load");
    return Ok();
}

Result<int> BmFont::loadGlyph(uint32_t) {
    return Ok(-1);
}

uint32_t BmFont::getGlyphIndex(uint32_t codepoint) {
    auto it = _codepointToIndex.find(codepoint);
    return (it != _codepointToIndex.end() && it->second >= 0)
        ? static_cast<uint32_t>(it->second) : 0;
}

uint32_t BmFont::getGlyphIndex(uint32_t codepoint, Style) {
    return getGlyphIndex(codepoint);
}

uint32_t BmFont::getGlyphIndex(uint32_t codepoint, bool, bool) {
    return getGlyphIndex(codepoint);
}

bool BmFont::hasGlyph(uint32_t codepoint) const {
    return _codepointToIndex.find(codepoint) != _codepointToIndex.end();
}

void BmFont::setCellSize(float, float) {
    // Stub: no-op
}

GpuResourceSet BmFont::getGpuResourceSet() const {
    GpuResourceSet res;
    res.shared = _shared;
    res.name = "bmFont";
    res.textureWidth = _atlasSize;
    res.textureHeight = _atlasSize;
    res.textureFormat = WGPUTextureFormat_RGBA8Unorm;
    res.textureWgslType = "texture_2d<f32>";
    res.textureData = _atlasData.data();
    res.textureDataSize = _atlasData.size();
    res.samplerFilter = WGPUFilterMode_Linear;
    res.bufferSize = _glyphMetadata.size() * sizeof(BitmapGlyphMetadata);
    res.bufferWgslType = "array<BitmapGlyphMetadata>";
    res.bufferReadonly = true;
    res.bufferData = reinterpret_cast<const uint8_t*>(_glyphMetadata.data());
    res.bufferDataSize = _glyphMetadata.size() * sizeof(BitmapGlyphMetadata);
    return res;
}

void BmFont::growAtlas() {}

Result<void> BmFont::renderGlyph(uint32_t, int, int) {
    return Ok();
}

// =============================================================================
// Full implementation for Desktop
// =============================================================================
#else // !YETTY_BM_FONT_STUB

BmFont::BmFont(const std::string& fontPath, uint32_t glyphSize, bool shared)
    : _fontPath(fontPath), _glyphSize(glyphSize), _shared(shared) {
    _glyphsPerRow = _atlasSize / _glyphSize;
}

BmFont::~BmFont() {
    if (_ftFace) {
        FT_Done_Face(static_cast<FT_Face>(_ftFace));
    }
    if (_ftLibrary) {
        FT_Done_FreeType(static_cast<FT_Library>(_ftLibrary));
    }
}

Result<void> BmFont::init() {
    FT_Library library;
    FT_Error error = FT_Init_FreeType(&library);
    if (error) {
        return Err<void>("Failed to initialize FreeType");
    }
    _ftLibrary = library;

    FT_Int major, minor, patch;
    FT_Library_Version(library, &major, &minor, &patch);
    ydebug("BmFont: FreeType version {}.{}.{}", major, minor, patch);

    if (auto res = findFont(); !res) {
        return Err<void>("Failed to find font", res);
    }

    _atlasData.resize(_atlasSize * _atlasSize * 4, 0);

    ydebug("BmFont initialized: {}x{} atlas, {}px glyphs", _atlasSize, _atlasSize, _glyphSize);
    return Ok();
}

Result<void> BmFont::findFont() {
    if (!_fontPath.empty()) {
        FT_Library library = static_cast<FT_Library>(_ftLibrary);
        FT_Face face = nullptr;
        FT_Error error = FT_New_Face(library, _fontPath.c_str(), 0, &face);
        if (error == 0) {
            _ftFace = face;
            if (face->num_fixed_sizes > 0) {
                FT_Select_Size(face, 0);
            }
            FT_Select_Charmap(face, FT_ENCODING_UNICODE);
            ydebug("BmFont: using font '{}'", _fontPath);
            return Ok();
        }
        return Err<void>("Failed to load font: " + _fontPath);
    }

#ifdef YETTY_USE_FONTCONFIG
    FcConfig* config = FcInitLoadConfigAndFonts();
    if (!config) {
        return Err<void>("Failed to initialize fontconfig");
    }

    const char* fontPatterns[] = {
        "Noto Color Emoji", "Apple Color Emoji", "Segoe UI Emoji",
        "Twemoji", "EmojiOne", "emoji"
    };

    FT_Library library = static_cast<FT_Library>(_ftLibrary);
    FT_Face face = nullptr;

    for (const char* pattern : fontPatterns) {
        FcPattern* fcPattern = FcNameParse(reinterpret_cast<const FcChar8*>(pattern));
        if (!fcPattern) continue;

        FcConfigSubstitute(config, fcPattern, FcMatchPattern);
        FcDefaultSubstitute(fcPattern);

        FcResult result;
        FcPattern* match = FcFontMatch(config, fcPattern, &result);
        FcPatternDestroy(fcPattern);

        if (match && result == FcResultMatch) {
            FcChar8* fontPath = nullptr;
            if (FcPatternGetString(match, FC_FILE, 0, &fontPath) == FcResultMatch) {
                FT_Error error = FT_New_Face(library, reinterpret_cast<const char*>(fontPath), 0, &face);
                if (error == 0) {
                    _fontPath = reinterpret_cast<const char*>(fontPath);
                    _ftFace = face;
                    if (face->num_fixed_sizes > 0) FT_Select_Size(face, 0);
                    FT_Select_Charmap(face, FT_ENCODING_UNICODE);
                    ydebug("BmFont: using font '{}'", _fontPath);
                    FcPatternDestroy(match);
                    FcConfigDestroy(config);
                    return Ok();
                }
            }
            FcPatternDestroy(match);
        }
    }
    FcConfigDestroy(config);
    return Err<void>("No suitable font found");
#elif defined(__APPLE__)
    FT_Library library = static_cast<FT_Library>(_ftLibrary);
    FT_Face face = nullptr;
    const char* paths[] = {
        "/System/Library/Fonts/Apple Color Emoji.ttc",
        "/System/Library/Fonts/AppleColorEmoji.ttc",
    };
    for (const char* path : paths) {
        if (FT_New_Face(library, path, 0, &face) == 0) {
            _ftFace = face;
            ydebug("BmFont: using macOS font '{}'", path);
            return Ok();
        }
    }
    return Err<void>("No suitable emoji font found on macOS");
#elif defined(_WIN32)
    FT_Library library = static_cast<FT_Library>(_ftLibrary);
    FT_Face face = nullptr;
    const char* paths[] = {
        "C:\\Windows\\Fonts\\seguiemj.ttf",
        "C:\\Windows\\Fonts\\segoeui.ttf",
    };
    for (const char* path : paths) {
        if (FT_New_Face(library, path, 0, &face) == 0) {
            _fontPath = path;
            _ftFace = face;
            if (face->num_fixed_sizes > 0) FT_Select_Size(face, 0);
            FT_Select_Charmap(face, FT_ENCODING_UNICODE);
            ydebug("BmFont: using Windows font '{}'", path);
            return Ok();
        }
    }
    return Err<void>("No suitable font found on Windows");
#else
    return Err<void>("No font path specified and fontconfig not available");
#endif
}

Result<void> BmFont::loadCommonGlyphs() {
    ydebug("BmFont: progressive atlas ready (glyphs loaded on demand)");
    return Ok();
}

Result<int> BmFont::loadGlyph(uint32_t codepoint) {
    auto it = _codepointToIndex.find(codepoint);
    if (it != _codepointToIndex.end()) {
        return Ok(it->second);
    }

    FT_Face face = static_cast<FT_Face>(_ftFace);
    if (!face) {
        return Err<int>("No font loaded");
    }

    FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
    if (glyphIndex == 0) {
        return Ok(-1);
    }

    FT_Error error = FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT | FT_LOAD_COLOR);
    if (error) {
        error = FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT);
        if (error) {
            return Err<int>("Failed to load glyph");
        }
    }

    FT_GlyphSlot slot = face->glyph;
    if (slot->format != FT_GLYPH_FORMAT_BITMAP) {
        error = FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);
        if (error) {
            return Err<int>("Failed to render glyph");
        }
    }

    FT_Bitmap& bitmap = slot->bitmap;
    if (bitmap.width == 0 || bitmap.rows == 0) {
        return Ok(-1);
    }

    if (_nextGlyphY * static_cast<int>(_glyphSize) + static_cast<int>(_glyphSize) > static_cast<int>(_atlasSize)) {
        if (_atlasSize >= ATLAS_MAX_SIZE) {
            yerror("BmFont: atlas at maximum size");
            return Ok(-1);
        }
        growAtlas();
    }

    int atlasX = _nextGlyphX * static_cast<int>(_glyphSize);
    int atlasY = _nextGlyphY * static_cast<int>(_glyphSize);
    int srcWidth = bitmap.width;
    int srcHeight = bitmap.rows;
    int offsetX = std::max(0, (static_cast<int>(_glyphSize) - srcWidth) / 2);
    int offsetY = std::max(0, (static_cast<int>(_glyphSize) - srcHeight) / 2);

    for (int y = 0; y < srcHeight && y + offsetY < static_cast<int>(_glyphSize); ++y) {
        for (int x = 0; x < srcWidth && x + offsetX < static_cast<int>(_glyphSize); ++x) {
            int dstX = atlasX + offsetX + x;
            int dstY = atlasY + offsetY + y;
            int dstIdx = (dstY * static_cast<int>(_atlasSize) + dstX) * 4;

            if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
                int srcIdx = y * bitmap.pitch + x * 4;
                _atlasData[dstIdx + 0] = bitmap.buffer[srcIdx + 2];
                _atlasData[dstIdx + 1] = bitmap.buffer[srcIdx + 1];
                _atlasData[dstIdx + 2] = bitmap.buffer[srcIdx + 0];
                _atlasData[dstIdx + 3] = bitmap.buffer[srcIdx + 3];
            } else if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
                int srcIdx = y * bitmap.pitch + x;
                uint8_t alpha = bitmap.buffer[srcIdx];
                _atlasData[dstIdx + 0] = 255;
                _atlasData[dstIdx + 1] = 255;
                _atlasData[dstIdx + 2] = 255;
                _atlasData[dstIdx + 3] = alpha;
            }
        }
    }

    BitmapGlyphMetadata meta;
    meta.uvMinX = static_cast<float>(atlasX) / _atlasSize;
    meta.uvMinY = static_cast<float>(atlasY) / _atlasSize;
    meta.uvMaxX = static_cast<float>(atlasX + _glyphSize) / _atlasSize;
    meta.uvMaxY = static_cast<float>(atlasY + _glyphSize) / _atlasSize;
    meta.width = static_cast<float>(_glyphSize);
    meta.height = static_cast<float>(_glyphSize);
    meta._pad1 = 0;
    meta._pad2 = 0;

    int index = static_cast<int>(_glyphMetadata.size());
    _glyphMetadata.push_back(meta);
    _codepointToIndex[codepoint] = index;

    _nextGlyphX++;
    if (_nextGlyphX >= static_cast<int>(_glyphsPerRow)) {
        _nextGlyphX = 0;
        _nextGlyphY++;
    }

    _dirty = true;
    return Ok(index);
}

uint32_t BmFont::getGlyphIndex(uint32_t codepoint) {
    auto it = _codepointToIndex.find(codepoint);
    if (it != _codepointToIndex.end() && it->second >= 0) {
        return static_cast<uint32_t>(it->second);
    }
    auto res = loadGlyph(codepoint);
    if (res && *res >= 0) {
        return static_cast<uint32_t>(*res);
    }
    return 0;
}

uint32_t BmFont::getGlyphIndex(uint32_t codepoint, Style) {
    return getGlyphIndex(codepoint);
}

uint32_t BmFont::getGlyphIndex(uint32_t codepoint, bool, bool) {
    return getGlyphIndex(codepoint);
}

bool BmFont::hasGlyph(uint32_t codepoint) const {
    return _codepointToIndex.find(codepoint) != _codepointToIndex.end();
}

void BmFont::setCellSize(float, float) {
    // BmFont uses fixed glyph size, no re-rasterization needed
}

GpuResourceSet BmFont::getGpuResourceSet() const {
    GpuResourceSet res;
    res.shared = _shared;
    res.name = "bmFont";
    res.textureWidth = _atlasSize;
    res.textureHeight = _atlasSize;
    res.textureFormat = WGPUTextureFormat_RGBA8Unorm;
    res.textureWgslType = "texture_2d<f32>";
    res.textureData = _atlasData.data();
    res.textureDataSize = _atlasData.size();
    res.samplerFilter = WGPUFilterMode_Linear;
    res.bufferSize = _glyphMetadata.size() * sizeof(BitmapGlyphMetadata);
    res.bufferWgslType = "array<BitmapGlyphMetadata>";
    res.bufferReadonly = true;
    res.bufferData = reinterpret_cast<const uint8_t*>(_glyphMetadata.data());
    res.bufferDataSize = _glyphMetadata.size() * sizeof(BitmapGlyphMetadata);
    res.uniformFields = {
        {"bmFontGlyphSize", "f32", sizeof(float)},
    };
    return res;
}

void BmFont::growAtlas() {
    uint32_t oldSize = _atlasSize;
    uint32_t newSize = std::min(_atlasSize * 2, ATLAS_MAX_SIZE);

    ydebug("BmFont: growing atlas {}x{} -> {}x{}", oldSize, oldSize, newSize, newSize);

    std::vector<uint8_t> newData(static_cast<size_t>(newSize) * newSize * 4, 0);
    for (uint32_t y = 0; y < oldSize; ++y) {
        std::memcpy(&newData[y * newSize * 4], &_atlasData[y * oldSize * 4], oldSize * 4);
    }
    _atlasData = std::move(newData);

    float scale = static_cast<float>(oldSize) / static_cast<float>(newSize);
    for (auto& meta : _glyphMetadata) {
        meta.uvMinX *= scale;
        meta.uvMinY *= scale;
        meta.uvMaxX *= scale;
        meta.uvMaxY *= scale;
    }

    _atlasSize = newSize;
    _glyphsPerRow = _atlasSize / _glyphSize;
    _dirty = true;
}

Result<void> BmFont::renderGlyph(uint32_t, int, int) {
    return Ok();
}

#endif // YETTY_BM_FONT_STUB

// =============================================================================
// Factory
// =============================================================================

Result<BmFont*> BmFont::createImpl(const std::string& fontPath,
                                    uint32_t glyphSize,
                                    bool shared) {
    auto* font = new BmFont(fontPath, glyphSize, shared);
    if (auto res = font->init(); !res) {
        yerror("BmFont creation failed: {}", error_msg(res));
        delete font;
        return Err<BmFont*>("Failed to initialize BmFont", res);
    }
    return Ok(font);
}

} // namespace yetty
