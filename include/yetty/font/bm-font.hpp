#pragma once

#include <yetty/font/font.hpp>
#include <yetty/core/result.hpp>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>

namespace yetty {

// Bitmap glyph metadata for GPU (matches shader struct)
struct BitmapGlyphMetadata {
    float uvMinX, uvMinY;
    float uvMaxX, uvMaxY;
    float width, height;
    float _pad1, _pad2;
};

// BmFont - color (RGBA) texture atlas for bitmap font rendering (emojis, etc.)
// CPU only, no GPU code.
class BmFont : public Font {
public:
    // Factory method
    static Result<BmFont*> createImpl(const std::string& fontPath,
                                       uint32_t glyphSize,
                                       bool shared);

    ~BmFont() override;

    // Non-copyable
    BmFont(const BmFont&) = delete;
    BmFont& operator=(const BmFont&) = delete;

    // =========================================================================
    // Font interface
    // =========================================================================

    uint32_t getGlyphIndex(uint32_t codepoint) override;
    uint32_t getGlyphIndex(uint32_t codepoint, Style style) override;
    uint32_t getGlyphIndex(uint32_t codepoint, bool bold, bool italic) override;

    void setCellSize(float cellWidth, float cellHeight) override;

    GpuResourceSet getGpuResourceSet() const override;

    // =========================================================================
    // CPU data accessors
    // =========================================================================

    bool isDirty() const { return _dirty; }
    void clearDirty() { _dirty = false; }
    uint32_t getAtlasSize() const { return _atlasSize; }
    const std::vector<uint8_t>& getAtlasData() const { return _atlasData; }
    const void* getMetadataData() const { return _glyphMetadata.data(); }
    size_t getMetadataDataSize() const { return _glyphMetadata.size() * sizeof(BitmapGlyphMetadata); }

    // =========================================================================
    // Bitmap font specific
    // =========================================================================

    Result<void> loadCommonGlyphs();
    Result<int> loadGlyph(uint32_t codepoint);
    bool hasGlyph(uint32_t codepoint) const;
    uint32_t getGlyphSize() const { return _glyphSize; }
    uint32_t getGlyphCount() const { return static_cast<uint32_t>(_glyphMetadata.size()); }

protected:
    BmFont(const std::string& fontPath, uint32_t glyphSize, bool shared);
    Result<void> init();

private:
    Result<void> findFont();
    void growAtlas();
    Result<void> renderGlyph(uint32_t codepoint, int atlasX, int atlasY);

    std::string _fontPath;
    uint32_t _glyphSize = 64;
    uint32_t _atlasSize = 256;
    uint32_t _glyphsPerRow = 4;
    bool _shared = false;
    bool _dirty = false;

    static constexpr uint32_t ATLAS_MAX_SIZE = 4096;

    // FreeType handles
    void* _ftLibrary = nullptr;
    void* _ftFace = nullptr;

    // Atlas data (RGBA)
    std::vector<uint8_t> _atlasData;

    // Metadata
    std::vector<BitmapGlyphMetadata> _glyphMetadata;
    std::unordered_map<uint32_t, int> _codepointToIndex;

    // Shelf packer state
    int _nextGlyphX = 0;
    int _nextGlyphY = 0;
};

} // namespace yetty
