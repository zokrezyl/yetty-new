#include <yetty/font/raster-font.hpp>
#include <yetty/gpu-resource-set.hpp>
#include <ytrace/ytrace.hpp>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <unordered_map>
#include <vector>
#include <cmath>
#include <algorithm>

namespace yetty {

//=============================================================================
// Internal types
//=============================================================================

namespace {

// Glyph UV offset - minimal for monospace terminal
struct RasterGlyphUV {
    float uvX, uvY;  // UV top-left corner
};

static_assert(sizeof(RasterGlyphUV) == 8, "RasterGlyphUV must be 8 bytes");

} // anonymous namespace

//=============================================================================
// RasterFontImpl - CPU only, no GPU code
//=============================================================================

class RasterFontImpl : public RasterFont {
public:
    RasterFontImpl(const std::string& ttfPath, float cellWidth, float cellHeight, bool shared)
        : _ttfPath(ttfPath), _cellWidth(cellWidth), _cellHeight(cellHeight), _shared(shared) {}

    ~RasterFontImpl() override {
        cleanup();
    }

    //=========================================================================
    // Initialization
    //=========================================================================

    Result<void> init() {
        // Initialize FreeType
        if (FT_Init_FreeType(&_ftLibrary)) {
            return Err<void>("Failed to initialize FreeType");
        }

        // Load font face
        if (FT_New_Face(_ftLibrary, _ttfPath.c_str(), 0, &_ftFace)) {
            FT_Done_FreeType(_ftLibrary);
            _ftLibrary = nullptr;
            return Err<void>("Failed to load font: " + _ttfPath);
        }

        // Calculate proper font size from metrics
        updateFontSize();

        // Initialize atlas data (R8 grayscale)
        _atlasData.resize(_atlasSize * _atlasSize, 0);

        ydebug("RasterFont loaded: {} (cell={}x{}, fontSize={}, baseline={}, atlas={}x{})",
              _ttfPath, _cellWidth, _cellHeight, _fontSize, _baseline, _atlasSize, _atlasSize);

        return Ok();
    }

    void updateFontSize() {
        if (!_ftFace) return;

        // Set initial size to get metrics (use cell height as starting point)
        FT_Set_Pixel_Sizes(_ftFace, 0, static_cast<uint32_t>(_cellHeight));

        // Get font metrics in pixels (at current size)
        // FreeType metrics are in 26.6 fixed point (divide by 64)
        int ascender = _ftFace->size->metrics.ascender >> 6;
        int descender = std::abs(_ftFace->size->metrics.descender >> 6);
        int lineHeight = ascender + descender;

        // Scale font size so line height fits in cell height (with small margin)
        int targetHeight = static_cast<int>(_cellHeight) - 2;  // 1px margin top/bottom
        if (lineHeight > 0 && targetHeight > 0) {
            _fontSize = static_cast<uint32_t>(_cellHeight * targetHeight / lineHeight);
        } else {
            _fontSize = static_cast<uint32_t>(_cellHeight);
        }

        // Apply the calculated font size
        FT_Set_Pixel_Sizes(_ftFace, 0, _fontSize);

        // Recalculate metrics at new size
        ascender = _ftFace->size->metrics.ascender >> 6;
        descender = std::abs(_ftFace->size->metrics.descender >> 6);

        // Baseline position from top of cell (center the text vertically)
        int actualLineHeight = ascender + descender;
        int topMargin = (static_cast<int>(_cellHeight) - actualLineHeight) / 2;
        _baseline = topMargin + ascender;
    }

    //=========================================================================
    // Cell size management
    //=========================================================================

    void setCellSize(float cellWidth, float cellHeight) override {
        if (cellWidth == _cellWidth && cellHeight == _cellHeight) {
            return;
        }

        _cellWidth = cellWidth;
        _cellHeight = cellHeight;

        updateFontSize();
        rasterizeAll();
    }

    //=========================================================================
    // Glyph loading
    //=========================================================================

    Result<void> loadGlyphs(const std::vector<uint32_t>& codepoints) override {
        for (uint32_t cp : codepoints) {
            if (_glyphIndices.count(cp)) continue;

            auto result = rasterizeGlyph(cp);
            if (!result) {
                ywarn("Failed to rasterize glyph for U+{:04X}", cp);
                continue;
            }
            _loadedCodepoints.push_back(cp);
        }

        _dirty = true;
        return Ok();
    }

    Result<void> loadBasicLatin() override {
        std::vector<uint32_t> codepoints;

        // Basic Latin (ASCII printable: space to tilde)
        for (uint32_t cp = 0x20; cp <= 0x7E; ++cp) {
            codepoints.push_back(cp);
        }

        // Latin-1 Supplement (0xA0-0xFF)
        for (uint32_t cp = 0xA0; cp <= 0xFF; ++cp) {
            codepoints.push_back(cp);
        }

        // Latin Extended-A (0x100-0x17F)
        for (uint32_t cp = 0x100; cp <= 0x17F; ++cp) {
            codepoints.push_back(cp);
        }

        // Box Drawing (0x2500-0x257F)
        for (uint32_t cp = 0x2500; cp <= 0x257F; ++cp) {
            codepoints.push_back(cp);
        }

        // Block Elements (0x2580-0x259F)
        for (uint32_t cp = 0x2580; cp <= 0x259F; ++cp) {
            codepoints.push_back(cp);
        }

        // Common math/programming symbols
        const uint32_t extraSymbols[] = {
            0x2190, 0x2191, 0x2192, 0x2193,  // arrows
            0x2194, 0x2195, 0x21D0, 0x21D2,  // double arrows
            0x2200, 0x2203, 0x2205, 0x2208,  // math
            0x2260, 0x2264, 0x2265, 0x2227,
            0x2228, 0x2229, 0x222A, 0x2248,
            0x221E, 0x2022, 0x2026, 0x00B7,
        };
        for (uint32_t cp : extraSymbols) {
            codepoints.push_back(cp);
        }

        return loadGlyphs(codepoints);
    }

    //=========================================================================
    // GpuResourceSet - describes what GPU resources are needed
    //=========================================================================

    GpuResourceSet getGpuResourceSet() const override {
        GpuResourceSet res;
        res.shared = _shared;
        res.name = "rasterFont";

        // Texture: R8 atlas
        res.textureWidth = _atlasSize;
        res.textureHeight = _atlasSize;
        res.textureFormat = WGPUTextureFormat_R8Unorm;
        res.textureWgslType = "texture_2d<f32>";
        res.textureData = _atlasData.data();
        res.textureDataSize = _atlasData.size();

        // Sampler: linear filtering
        res.samplerFilter = WGPUFilterMode_Linear;

        // Buffer: glyph UV data
        res.bufferSize = _glyphUVs.size() * sizeof(RasterGlyphUV);
        res.bufferWgslType = "array<RasterGlyphUV>";
        res.bufferReadonly = true;
        res.bufferData = reinterpret_cast<const uint8_t*>(_glyphUVs.data());
        res.bufferDataSize = _glyphUVs.size() * sizeof(RasterGlyphUV);

        // Uniform fields
        res.uniformFields = {
            {"fontCellSize", "vec2<f32>", sizeof(float) * 2},
            {"fontAtlasSize", "vec2<f32>", sizeof(float) * 2},
        };

        return res;
    }

    //=========================================================================
    // Font interface
    //=========================================================================

    uint32_t getGlyphIndex(uint32_t codepoint) override {
        auto it = _glyphIndices.find(codepoint);
        if (it != _glyphIndices.end()) {
            return it->second;
        }
        return 0;
    }

    uint32_t getGlyphIndex(uint32_t codepoint, Style) override {
        return getGlyphIndex(codepoint);
    }

    uint32_t getGlyphIndex(uint32_t codepoint, bool, bool) override {
        return getGlyphIndex(codepoint);
    }

    //=========================================================================
    // CPU data accessors
    //=========================================================================

    bool isDirty() const override { return _dirty; }
    void clearDirty() override { _dirty = false; }
    uint32_t getAtlasSize() const override { return _atlasSize; }
    const std::vector<uint8_t>& getAtlasData() const override { return _atlasData; }
    const void* getGlyphUVData() const override { return _glyphUVs.data(); }
    size_t getGlyphUVDataSize() const override { return _glyphUVs.size() * sizeof(RasterGlyphUV); }

private:
    //=========================================================================
    // Re-rasterize all loaded glyphs (after cell size change)
    //=========================================================================

    void rasterizeAll() {
        std::fill(_atlasData.begin(), _atlasData.end(), 0);
        _nextSlotIdx = 0;
        _glyphUVs.clear();
        _glyphIndices.clear();

        for (uint32_t cp : _loadedCodepoints) {
            auto result = rasterizeGlyph(cp);
            if (!result) {
                ywarn("Failed to re-rasterize glyph for U+{:04X}", cp);
            }
        }

        _dirty = true;
        ydebug("RasterFont: re-rasterized {} glyphs at cell size {}x{}",
              _loadedCodepoints.size(), _cellWidth, _cellHeight);
    }

    //=========================================================================
    // Rasterize a single glyph into a cell-sized slot
    //=========================================================================

    Result<void> rasterizeGlyph(uint32_t codepoint) {
        FT_UInt glyphIndex = FT_Get_Char_Index(_ftFace, codepoint);
        if (glyphIndex == 0) {
            return Err<void>("Glyph not found in font");
        }

        if (FT_Load_Glyph(_ftFace, glyphIndex, FT_LOAD_RENDER)) {
            return Err<void>("Failed to render glyph");
        }

        FT_GlyphSlot slot = _ftFace->glyph;
        FT_Bitmap& bitmap = slot->bitmap;

        if (codepoint >= _glyphUVs.size()) {
            _glyphUVs.resize(codepoint + 1, {-1.0f, -1.0f});
        }

        if (bitmap.width == 0 || bitmap.rows == 0) {
            _glyphUVs[codepoint] = {-1.0f, -1.0f};
            _glyphIndices[codepoint] = codepoint;
            return Ok();
        }

        constexpr int SLOT_PADDING = 1;
        int cellW = static_cast<int>(_cellWidth);
        int cellH = static_cast<int>(_cellHeight);
        int slotW = cellW + SLOT_PADDING * 2;
        int slotH = cellH + SLOT_PADDING * 2;
        int slotsPerRow = static_cast<int>(_atlasSize) / slotW;

        if (slotsPerRow == 0) {
            return Err<void>("Cell width too large for atlas");
        }

        int slotCol = _nextSlotIdx % slotsPerRow;
        int slotRow = _nextSlotIdx / slotsPerRow;
        int slotX = slotCol * slotW + SLOT_PADDING;
        int slotY = slotRow * slotH + SLOT_PADDING;

        if (slotY + cellH > static_cast<int>(_atlasSize)) {
            return Err<void>("Atlas is full");
        }

        // Clear cell slot
        for (int y = -SLOT_PADDING; y < cellH + SLOT_PADDING; ++y) {
            for (int x = -SLOT_PADDING; x < cellW + SLOT_PADDING; ++x) {
                int dstIdx = (slotY + y) * static_cast<int>(_atlasSize) + (slotX + x);
                if (dstIdx >= 0 && dstIdx < static_cast<int>(_atlasData.size())) {
                    _atlasData[dstIdx] = 0;
                }
            }
        }

        // Position glyph
        int glyphW = static_cast<int>(bitmap.width);
        int glyphH = static_cast<int>(bitmap.rows);
        int bearingX = slot->bitmap_left;
        int bearingY = slot->bitmap_top;
        int offsetX = std::max(0, std::min(bearingX, cellW - glyphW));
        int offsetY = std::max(0, std::min(_baseline - bearingY, cellH - glyphH));

        // Copy bitmap to atlas
        for (int y = 0; y < glyphH && (offsetY + y) < cellH; ++y) {
            for (int x = 0; x < glyphW && (offsetX + x) < cellW; ++x) {
                int srcIdx = y * bitmap.pitch + x;
                int dstIdx = (slotY + offsetY + y) * static_cast<int>(_atlasSize) + (slotX + offsetX + x);
                if (dstIdx >= 0 && dstIdx < static_cast<int>(_atlasData.size())) {
                    _atlasData[dstIdx] = bitmap.buffer[srcIdx];
                }
            }
        }

        _glyphUVs[codepoint] = {
            static_cast<float>(slotX) / _atlasSize,
            static_cast<float>(slotY) / _atlasSize
        };
        _glyphIndices[codepoint] = codepoint;

        _nextSlotIdx++;
        _dirty = true;
        return Ok();
    }

    //=========================================================================
    // Cleanup
    //=========================================================================

    void cleanup() {
        if (_ftFace) {
            FT_Done_Face(_ftFace);
            _ftFace = nullptr;
        }
        if (_ftLibrary) {
            FT_Done_FreeType(_ftLibrary);
            _ftLibrary = nullptr;
        }
    }

    //=========================================================================
    // Private data
    //=========================================================================

    std::string _ttfPath;
    float _cellWidth;
    float _cellHeight;
    uint32_t _fontSize = 0;
    int _baseline = 0;
    bool _shared = false;

    // FreeType
    FT_Library _ftLibrary = nullptr;
    FT_Face _ftFace = nullptr;

    // Atlas (CPU)
    static constexpr uint32_t _atlasSize = 1024;
    std::vector<uint8_t> _atlasData;

    // Glyph data (CPU)
    int _nextSlotIdx = 0;
    std::vector<RasterGlyphUV> _glyphUVs;
    std::unordered_map<uint32_t, uint32_t> _glyphIndices;
    std::vector<uint32_t> _loadedCodepoints;

    bool _dirty = false;
};

//=============================================================================
// RasterFont::createImpl
//=============================================================================

Result<RasterFont*> RasterFont::createImpl(const std::string& ttfPath,
                                           float cellWidth,
                                           float cellHeight,
                                           bool shared) {
    auto* font = new RasterFontImpl(ttfPath, cellWidth, cellHeight, shared);
    if (auto res = font->init(); !res) {
        yerror("RasterFont creation failed: {}", error_msg(res));
        delete font;
        return Err<RasterFont*>("Failed to initialize RasterFont", res);
    }
    ytest("raster-font-created", "RasterFont created successfully");
    return Ok(font);
}

} // namespace yetty
