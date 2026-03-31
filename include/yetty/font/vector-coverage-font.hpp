#pragma once

#include <yetty/font/font.hpp>
#include <yetty/gpu-allocator.hpp>
#include <yetty/gpu-context.hpp>
#include <yetty/core/factory-object.hpp>

#include <string>
#include <memory>

namespace yetty {

/**
 * VectorCoverageFont - Coverage-based curve font rendering
 *
 * Instead of SDF evaluation, uses analytic coverage calculation:
 * - Cast rays from each pixel
 * - Count curve intersections for winding number
 * - Integrate coverage over pixel area
 *
 * Benefits:
 *   - More accurate anti-aliasing at all sizes
 *   - Correct handling of overlapping curves
 *   - No distance field artifacts
 *
 * Tradeoffs vs VectorSdfFont:
 *   - Slightly more GPU computation per pixel
 *   - Better quality at small sizes
 *
 * Font render method: FontRenderMethod::Coverage
 *
 * GPU buffer layout identical to VectorSdfFont:
 *   [u32: curveCount | flags]
 *   [u32: p0] [u32: p1] [u32: p2]  <- curve 0 (12 bytes)
 *   ... (curveCount curves)
 *
 * Each point packed as: x[16] | y[16] normalized to cell space [0,1]
 */
class VectorCoverageFont : public Font,
                           public core::FactoryObject<VectorCoverageFont> {
public:
    // Factory: loads font from TTF path
    static Result<VectorCoverageFont*> createImpl(const GPUContext& gpu,
                                                   GpuAllocator* allocator,
                                                   const std::string& ttfPath);

    ~VectorCoverageFont() override = default;

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
