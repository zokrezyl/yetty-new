#pragma once

#include <yetty/font/ms-msdf-font.hpp>
#include <yetty/font/bm-font.hpp>
#include <yetty/font/shader-font.hpp>
#include <yetty/font/vector-sdf-font.hpp>
#include <yetty/font/vector-coverage-font.hpp>
#include <yetty/font/raster-font.hpp>
#include <yetty/gpu-allocator.hpp>
#include <yetty/gpu-context.hpp>
#include <yetty/shader-manager.hpp>
#include <yetty/msdf-cdb-provider.hpp>
#include <yetty/core/result.hpp>
#include <yetty/core/factory-object.hpp>
#include <string>
#include <memory>

namespace yetty {

/**
 * FontManager manages all font types for the terminal.
 *
 * Created via ObjectFactory with GPUContext and ShaderManager.
 * Stored in YettyContext for access by renderers.
 */
class FontManager : public core::FactoryObject<FontManager> {
public:
    virtual ~FontManager() = default;

    // Factory - creates FontManagerImpl
    // msdfFontsDir: Directory for MSDF font CDB files (from Platform::getMsdfFontsDir())
    // fontsDir: Directory for TTF font files (from Platform::getFontsDir())
    // shadersDir: Directory for WGSL shader files (from Platform::getShadersDir())
    // preloadCardShaders: Shader names to preload for cards (format: "0xNNNN-name")
    // preloadGlyphShaders: Shader names to preload for glyphs (format: "0xNNNN-name")
    static Result<FontManager*> createImpl(const GPUContext& gpu,
                                            GpuAllocator* allocator,
                                            ShaderManager* shaderMgr,
                                            const std::string& msdfFontsDir,
                                            const std::string& fontsDir,
                                            const std::string& shadersDir,
                                            MsdfCdbProvider* cdbProvider = nullptr,
                                            const std::vector<std::string>& preloadCardShaders = {},
                                            const std::vector<std::string>& preloadGlyphShaders = {}) noexcept;

    virtual Result<MsMsdfFont*> getMsMsdfFont(const std::string& fontName) noexcept = 0;
    virtual MsMsdfFont* getDefaultMsMsdfFont() noexcept = 0;
    virtual BmFont* getDefaultBmFont() noexcept = 0;
    virtual ShaderFont* getDefaultShaderGlyphFont() noexcept = 0;
    virtual ShaderFont* getDefaultCardFont() noexcept = 0;

    // Vector font (SDF curve-based rendering)
    virtual Result<VectorSdfFont*> getVectorSdfFont(const std::string& ttfPath) noexcept = 0;
    virtual VectorSdfFont* getDefaultVectorSdfFont() noexcept = 0;

    // Vector font (coverage-based rendering)
    virtual Result<VectorCoverageFont*> getVectorCoverageFont(const std::string& ttfPath) noexcept = 0;
    virtual VectorCoverageFont* getDefaultVectorCoverageFont() noexcept = 0;

    // Raster font (texture atlas rendering)
    // Cell size defaults are placeholder - caller should use setCellSize() once actual size is known
    virtual Result<RasterFont*> getRasterFont(const std::string& fontName,
                                               uint32_t cellWidth = 16,
                                               uint32_t cellHeight = 32) noexcept = 0;
    virtual RasterFont* getDefaultRasterFont() noexcept = 0;

    virtual void setDefaultFont(const std::string& fontName) noexcept = 0;
    virtual bool hasFont(const std::string& fontName) const noexcept = 0;
    virtual const std::string& getCacheDir() const noexcept = 0;
    virtual MsdfCdbProvider* getCdbProvider() const noexcept = 0;

protected:
    FontManager() = default;
};

} // namespace yetty
