#pragma once

#include <yetty/font/font.hpp>
#include <webgpu/webgpu.h>

#include <cstdint>
#include <string>
#include <vector>

namespace yetty {

class GpuAllocator;

/**
 * RasterFont - Rasterized glyph atlas font rendering
 *
 * Uses FreeType to rasterize glyphs from TTF/OTF fonts into a grayscale
 * texture atlas. Similar to how Alacritty renders fonts - direct rasterization
 * rather than MSDF or curve-based approaches.
 *
 * Benefits:
 *   - Exact pixel-perfect rendering at target size
 *   - Subpixel positioning support (future)
 *   - Lower GPU complexity than curve evaluation
 *   - Familiar approach used by most terminal emulators
 *
 * Tradeoffs:
 *   - Size-dependent (need to re-rasterize for different sizes)
 *   - Larger memory for multiple sizes vs MSDF
 *
 * Codepoint range for testing: Plane 15 PUA-A (0xF2000 - 0xF2FFF)
 *
 * GPU resources:
 *   - R8 texture atlas (grayscale)
 *   - Sampler
 *   - Metadata buffer (UV coords, size, bearing, advance per glyph)
 */
class RasterFont : public Font {
public:
    // Factory: loads font from TTF path
    // device/queue: WebGPU handles for GPU resource creation
    // allocator: for buffer/texture allocation tracking
    // shared: whether this font's GpuResourceSet should be marked shared
    static Result<RasterFont*> createImpl(WGPUDevice device,
                                          WGPUQueue queue,
                                          GpuAllocator* allocator,
                                          const std::string& ttfPath,
                                          uint32_t cellWidth,
                                          uint32_t cellHeight,
                                          bool shared = false);

    ~RasterFont() override = default;

    // Set cell size - triggers re-rasterization if size changed
    virtual void setCellSize(uint32_t cellWidth, uint32_t cellHeight) = 0;
    virtual uint32_t getCellWidth() const = 0;
    virtual uint32_t getCellHeight() const = 0;

    // Load glyphs for codepoints (rasterizes from TTF)
    virtual Result<void> loadGlyphs(const std::vector<uint32_t>& codepoints) = 0;

    // Load ASCII printable range (0x20 - 0x7E)
    virtual Result<void> loadBasicLatin() = 0;

    // Statistics
    virtual size_t glyphCount() const = 0;
    virtual uint32_t getFontSize() const = 0;

    // PUA-A range for raster font (0xF2000 - 0xF2FFF)
    static constexpr uint32_t CODEPOINT_BASE = 0xF2000;
    static constexpr uint32_t CODEPOINT_END  = 0xF2FFF;

    // Map ASCII to raster range: 'A' (65) -> 0xF2041
    static constexpr uint32_t toCodepoint(uint32_t ascii) {
        return CODEPOINT_BASE + ascii;
    }
};

} // namespace yetty
