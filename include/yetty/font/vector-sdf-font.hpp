#pragma once

#include <yetty/font/font.hpp>
#include <yetty/gpu-allocator.hpp>
#include <yetty/gpu-context.hpp>
#include <yetty/core/factory-object.hpp>

#include <string>
#include <memory>

namespace yetty {

/**
 * VectorSdfFont - SDF curve-based font rendering
 *
 * Instead of pre-rendered MSDF textures, stores actual Bezier curves
 * and evaluates signed distance analytically in the shader.
 *
 * Benefits:
 *   - ~25x smaller than MSDF atlas (curves vs pixels)
 *   - Resolution independent
 *   - No atlas texture management
 *
 * Font render method: FontRenderMethod::Vector
 *
 * GPU buffer layout (per glyph):
 *   [u32: curveCount | flags]
 *   [u32: p0] [u32: p1] [u32: p2]  <- curve 0 (12 bytes)
 *   [u32: p0] [u32: p1] [u32: p2]  <- curve 1
 *   ... (curveCount curves)
 *
 * Each point packed as: x[16] | y[16] normalized to cell space [0,1]
 */
class VectorSdfFont : public Font,
                      public core::FactoryObject<VectorSdfFont> {
public:
    // Factory: loads font from TTF path
    static Result<VectorSdfFont*> createImpl(const GPUContext& gpu,
                                              GpuAllocator* allocator,
                                              const std::string& ttfPath);

    ~VectorSdfFont() override = default;

    // Load glyphs for codepoints (extracts curves from TTF)
    virtual Result<void> loadGlyphs(const std::vector<uint32_t>& codepoints) = 0;

    // Load ASCII printable range (0x20 - 0x7E)
    virtual Result<void> loadBasicLatin() = 0;

    // GPU buffer access
    virtual WGPUBuffer getGlyphBuffer() const = 0;
    virtual WGPUBuffer getOffsetBuffer() const = 0;

    // Statistics
    virtual size_t glyphCount() const = 0;
    virtual size_t totalCurves() const = 0;
    virtual size_t bufferSize() const = 0;
    virtual size_t offsetTableSize() const = 0;

};

} // namespace yetty
