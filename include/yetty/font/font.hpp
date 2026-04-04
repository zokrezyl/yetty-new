#pragma once

#include <yetty/core/factory-object.hpp>
#include <yetty/core/result.hpp>
#include <yetty/gpu-resource-set.hpp>
#include <cstdint>

namespace yetty {

// Space glyph index - ALL font implementations MUST return this value for space (U+0020).
// This allows terminal code to use a single constant for empty/blank cells without
// querying each font. Font implementations must map codepoint 0x20 to this index.
constexpr uint32_t SPACE_GLYPH_INDEX = 0;

// Font render method - must match RENDER_METHOD_* in terminal-screen.wgsl
// Encoded in cell style byte bits 5-7
enum class FontRenderMethod : uint8_t {
    Msdf     = 0, // MSDF text rendering (default)
    Bitmap   = 1, // Bitmap fonts (emoji, color fonts)
    Shader   = 2, // Single-cell shader glyphs
    Card     = 3, // Multi-cell card glyphs
    Vector   = 4, // Vector font (SDF curves)
    Coverage = 5, // Vector font (coverage-based)
    Raster   = 6, // Raster font (texture atlas)
};

// Font - base class for font rendering.
// Provides glyph lookup and GPU resources via GpuResourceSet abstraction.
class Font : public core::FactoryObject<Font> {
public:
    enum class Style : uint8_t {
        Regular = 0,
        Bold = 1,
        Italic = 2,
        BoldItalic = 3
    };

    // Render method for cell encoding
    virtual FontRenderMethod renderMethod() const = 0;

    virtual ~Font() = default;

    // Get glyph index
    virtual uint32_t getGlyphIndex(uint32_t codepoint) = 0;
    virtual uint32_t getGlyphIndex(uint32_t codepoint, Style style) = 0;
    virtual uint32_t getGlyphIndex(uint32_t codepoint, bool bold, bool italic) = 0;

    // Set cell size - triggers re-rasterization/reload if size changed
    virtual void setCellSize(float cellWidth, float cellHeight) = 0;

    // GPU resources for binding
    virtual GpuResourceSet getGpuResourceSet() const = 0;

protected:
    Font() = default;
};

} // namespace yetty
