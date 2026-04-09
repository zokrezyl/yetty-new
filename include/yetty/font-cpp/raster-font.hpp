#pragma once

#include <yetty/font/font.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace yetty {

// RasterFont - rasterized glyph atlas font. CPU only, no GPU code.
class RasterFont : public Font {
public:
    // Factory: fonts directory, font name (without face suffix or extension), cell size, shared flag
    // Loads all faces: fontName-Regular.ttf, fontName-Bold.ttf, fontName-Oblique.ttf, fontName-BoldOblique.ttf
    static Result<RasterFont*> createImpl(const std::string& fontsDir,
                                          const std::string& fontName,
                                          float cellWidth,
                                          float cellHeight,
                                          bool shared);

    // Load glyphs for codepoints
    virtual Result<void> loadGlyphs(const std::vector<uint32_t>& codepoints) = 0;

    // Load basic Latin and common symbols
    virtual Result<void> loadBasicLatin() = 0;

    // CPU data accessors for GpuResourceBinder to upload
    virtual bool isDirty() const = 0;
    virtual void clearDirty() = 0;
    virtual uint32_t getAtlasSize() const = 0;
    virtual const std::vector<uint8_t>& getAtlasData() const = 0;
    virtual const void* getGlyphUVData() const = 0;
    virtual size_t getGlyphUVDataSize() const = 0;
};

} // namespace yetty
