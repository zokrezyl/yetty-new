#pragma once

#include <yetty/core/factory-object.hpp>
#include <yetty/core/result.hpp>
#include <yetty/gpu-resource-set.hpp>
#include <cstdint>

namespace yetty {

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
