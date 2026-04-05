#include <yetty/msdf-atlas.hpp>
#include <yetty/cdb-wrapper.hpp>
#include <ytrace/ytrace.hpp>

#include <cstring>
#include <algorithm>
#include <filesystem>
#include <unordered_map>

namespace yetty {

//=============================================================================
// MsdfAtlasImpl - concrete implementation of MsdfAtlas
//=============================================================================

class MsdfAtlasImpl : public MsdfAtlas {
public:
    MsdfAtlasImpl() {
        // Initialize atlas
        _atlasData.resize(_atlasWidth * _atlasHeight * 4, 0);

        // Reserve space for glyphs
        _glyphMetadata.reserve(512);

        // Add placeholder glyph at index 0 (empty 1x1)
        GlyphMetadataGPU placeholder{};
        placeholder._uvMinX = 0;
        placeholder._uvMinY = 0;
        placeholder._uvMaxX = 0;
        placeholder._uvMaxY = 0;
        placeholder._sizeX = 0;
        placeholder._sizeY = 0;
        placeholder._bearingX = 0;
        placeholder._bearingY = 0;
        placeholder._advance = 0;
        placeholder._pad = 0;
        _glyphMetadata.push_back(placeholder);
    }

    ~MsdfAtlasImpl() override {
        closeAllCdbs();
    }

    //=========================================================================
    // CDB file management
    //=========================================================================

    int openCdb(const std::string& path) override {
        auto reader = CdbReader::open(path);
        if (!reader) {
            ywarn("MsdfAtlas: Cannot open CDB file: {}", path);
            return -1;
        }

        int fontId = static_cast<int>(_cdbFiles.size());
        _cdbFiles.push_back(std::move(reader));
        _codepointToIndex.emplace_back();  // New empty map for this fontId

        ydebug("MsdfAtlas: Opened CDB [fontId={}]: {}", fontId, path);
        return fontId;
    }

    void closeCdb(int fontId) override {
        if (fontId < 0 || fontId >= static_cast<int>(_cdbFiles.size())) return;
        _cdbFiles[fontId].reset();
    }

    void closeAllCdbs() override {
        _cdbFiles.clear();
    }

    //=========================================================================
    // Glyph loading
    //=========================================================================

    uint32_t loadGlyph(int fontId, uint32_t codepoint) override {
        if (fontId < 0 || fontId >= static_cast<int>(_cdbFiles.size())) {
            return 0;
        }

        // Check if already loaded for this fontId
        auto it = _codepointToIndex[fontId].find(codepoint);
        if (it != _codepointToIndex[fontId].end()) {
            return it->second;
        }

        // Check if CDB is available
        auto& reader = _cdbFiles[fontId];
        if (!reader) {
            _codepointToIndex[fontId][codepoint] = 0;
            return 0;
        }

        // Look up in CDB
        auto data = reader->get(codepoint);
        if (!data) {
            _codepointToIndex[fontId][codepoint] = 0;
            return 0;
        }

        // Validate data size
        if (data->size() < sizeof(MsdfGlyphData)) {
            ywarn("Invalid glyph data size for codepoint {}", codepoint);
            _codepointToIndex[fontId][codepoint] = 0;
            return 0;
        }

        // Read header
        MsdfGlyphData header;
        std::memcpy(&header, data->data(), sizeof(header));

        // Create glyph metadata - CDB stores final values, use directly
        GlyphMetadataGPU meta{};
        meta._sizeX = header.sizeX;
        meta._sizeY = header.sizeY;
        meta._bearingX = header.bearingX;
        meta._bearingY = header.bearingY;
        meta._advance = header.advance;
        meta._pad = 0;

        // Handle empty glyphs (like space)
        if (header.width == 0 || header.height == 0) {
            meta._uvMinX = 0;
            meta._uvMinY = 0;
            meta._uvMaxX = 0;
            meta._uvMaxY = 0;

            uint32_t glyphIndex = static_cast<uint32_t>(_glyphMetadata.size());
            _glyphMetadata.push_back(meta);
            _codepointToIndex[fontId][codepoint] = glyphIndex;
            _indexToCodepoint[glyphIndex] = codepoint;
            return glyphIndex;
        }

        // Check if we need to grow atlas or wrap to next shelf
        if (_shelfX + header.width + ATLAS_PADDING > _atlasWidth) {
            _shelfX = _shelfMinX + ATLAS_PADDING;
            _shelfY += _shelfHeight + ATLAS_PADDING;
            _shelfHeight = 0;
        }

        // Check if we need more height - grow atlas if needed
        if (_shelfY + header.height + ATLAS_PADDING > _atlasHeight) {
            growAtlas();
        }

        // Validate pixel data size
        size_t pixelDataSize = header.width * header.height * 4;
        size_t expectedSize = sizeof(MsdfGlyphData) + pixelDataSize;
        if (data->size() < expectedSize) {
            ywarn("Glyph data truncated for codepoint {}", codepoint);
            _codepointToIndex[fontId][codepoint] = 0;
            return 0;
        }

        // Get pixel data pointer
        const uint8_t* pixels = data->data() + sizeof(MsdfGlyphData);

        // Copy to atlas
        uint32_t atlasX = _shelfX;
        uint32_t atlasY = _shelfY;

        for (uint32_t y = 0; y < header.height; ++y) {
            for (uint32_t x = 0; x < header.width; ++x) {
                size_t srcIdx = (y * header.width + x) * 4;
                size_t dstIdx = ((atlasY + y) * _atlasWidth + (atlasX + x)) * 4;

                _atlasData[dstIdx + 0] = pixels[srcIdx + 0];
                _atlasData[dstIdx + 1] = pixels[srcIdx + 1];
                _atlasData[dstIdx + 2] = pixels[srcIdx + 2];
                _atlasData[dstIdx + 3] = pixels[srcIdx + 3];
            }
        }

        // Update UV coordinates (normalized 0-1)
        meta._uvMinX = static_cast<float>(atlasX) / _atlasWidth;
        meta._uvMinY = static_cast<float>(atlasY) / _atlasHeight;
        meta._uvMaxX = static_cast<float>(atlasX + header.width) / _atlasWidth;
        meta._uvMaxY = static_cast<float>(atlasY + header.height) / _atlasHeight;

        // Update shelf packer
        _shelfX = atlasX + header.width + ATLAS_PADDING;
        _shelfHeight = std::max(_shelfHeight, static_cast<uint32_t>(header.height));

        // Add to metadata and index
        uint32_t glyphIndex = static_cast<uint32_t>(_glyphMetadata.size());
        _glyphMetadata.push_back(meta);
        _codepointToIndex[fontId][codepoint] = glyphIndex;
        _indexToCodepoint[glyphIndex] = codepoint;

        _dirty = true;
        return glyphIndex;
    }

    //=========================================================================
    // CDB registration with optional auto-generation
    //=========================================================================

    int registerCdb(const std::string& cdbPath,
                    const std::string& ttfPath,
                    MsdfCdbProvider::Ptr cdbProvider) override {
        // Generate CDB if missing
        if (!std::filesystem::exists(cdbPath) && !ttfPath.empty() && cdbProvider) {
            MsdfCdbConfig cfg;
            cfg.ttfPath = ttfPath;
            cfg.cdbPath = cdbPath;
            if (auto res = cdbProvider->generate(cfg); !res) {
                yerror("CDB generation failed for {}: {}", cdbPath, res.error().message());
                return -1;
            }
        }

        return openCdb(cdbPath);
    }

    //=========================================================================
    // Reverse lookup
    //=========================================================================

    uint32_t getCodepoint(uint32_t glyphIndex) const override {
        auto it = _indexToCodepoint.find(glyphIndex);
        if (it != _indexToCodepoint.end()) {
            return it->second;
        }
        return 0;
    }

    //=========================================================================
    // Accessors
    //=========================================================================

    bool isDirty() const override { return _dirty; }
    void clearDirty() override { _dirty = false; }
    bool hasPendingGlyphs() const override { return _dirty; }
    uint32_t getAtlasWidth() const override { return _atlasWidth; }
    uint32_t getAtlasHeight() const override { return _atlasHeight; }
    const std::vector<uint8_t>& getAtlasData() const override { return _atlasData; }

    float getFontSize() const override { return _fontSize; }
    float getLineHeight() const override { return _lineHeight; }
    float getPixelRange() const override { return _pixelRange; }

    const std::vector<GlyphMetadataGPU>& getGlyphMetadata() const override { return _glyphMetadata; }
    uint32_t getGlyphCount() const override { return static_cast<uint32_t>(_glyphMetadata.size()); }
    uint32_t getResourceVersion() const override { return _resourceVersion; }

private:
    //=========================================================================
    // Atlas growth
    //=========================================================================

    void growAtlas() {
        uint32_t oldWidth = _atlasWidth;
        uint32_t oldHeight = _atlasHeight;
        uint32_t newWidth = _atlasWidth;
        uint32_t newHeight = _atlasHeight;

        bool canGrowWidth = (newWidth + ATLAS_GROW_INCREMENT <= ATLAS_MAX_DIMENSION);
        bool canGrowHeight = (newHeight + ATLAS_GROW_INCREMENT <= ATLAS_MAX_DIMENSION);

        if (!canGrowWidth && !canGrowHeight) {
            yerror("MSDF atlas at maximum size {}x{}, cannot grow further",
                   _atlasWidth, _atlasHeight);
            return;
        }

        // Grow both dimensions when possible
        if (canGrowHeight) newHeight += ATLAS_GROW_INCREMENT;
        if (canGrowWidth)  newWidth  += ATLAS_GROW_INCREMENT;

        // Width-only growth (height maxed): pack into the new right column
        if (canGrowWidth && !canGrowHeight) {
            _shelfX = oldWidth;
            _shelfY = 0;
            _shelfHeight = 0;
            _shelfMinX = oldWidth;  // prevent wrapping back into old data
        }

        ydebug("Growing MSDF atlas from {}x{} to {}x{}", oldWidth, oldHeight, newWidth, newHeight);

        std::vector<uint8_t> newAtlasData(newWidth * newHeight * 4, 0);

        for (uint32_t y = 0; y < oldHeight; ++y) {
            std::memcpy(
                newAtlasData.data() + y * newWidth * 4,
                _atlasData.data() + y * oldWidth * 4,
                oldWidth * 4
            );
        }

        _atlasData = std::move(newAtlasData);
        _atlasWidth = newWidth;
        _atlasHeight = newHeight;

        // Recalculate UV coordinates for all existing glyphs if dimensions changed
        float scaleX = static_cast<float>(oldWidth) / static_cast<float>(newWidth);
        float scaleY = static_cast<float>(oldHeight) / static_cast<float>(newHeight);
        for (auto& meta : _glyphMetadata) {
            if (oldWidth != newWidth) {
                meta._uvMinX *= scaleX;
                meta._uvMaxX *= scaleX;
            }
            if (oldHeight != newHeight) {
                meta._uvMinY *= scaleY;
                meta._uvMaxY *= scaleY;
            }
        }

        _dirty = true;
    }

    //=========================================================================
    // Private data
    //=========================================================================

    // CDB readers (indexed by fontId)
    std::vector<CdbReader::Ptr> _cdbFiles;

    // Per-font codepoint-to-glyph-index cache (indexed by fontId)
    std::vector<std::unordered_map<uint32_t, uint32_t>> _codepointToIndex;

    // Reverse mapping: glyph index -> codepoint (shared across all fonts)
    std::unordered_map<uint32_t, uint32_t> _indexToCodepoint;

    // Glyph metadata for all loaded glyphs
    std::vector<GlyphMetadataGPU> _glyphMetadata;

    // Dirty flag
    bool _dirty = false;

    // Font metrics
    float _fontSize = 32.0f;
    float _lineHeight = 40.0f;
    float _pixelRange = 4.0f;

    // Runtime atlas (built on demand from CDB, grows dynamically)
    std::vector<uint8_t> _atlasData;
    uint32_t _atlasWidth = 1024;
    uint32_t _atlasHeight = 512;
    static constexpr uint32_t ATLAS_GROW_INCREMENT = 512;
    static constexpr uint32_t ATLAS_MAX_DIMENSION = 16384;

    // Shelf packer state for atlas
    uint32_t _shelfX = 0;
    uint32_t _shelfY = 0;
    uint32_t _shelfHeight = 0;
    uint32_t _shelfMinX = 0;  // Left bound for shelf wrapping (advances on width-only growth)
    static constexpr uint32_t ATLAS_PADDING = 2;

    // Resource version (for tracking changes)
    uint32_t _resourceVersion = 0;
};

//=============================================================================
// MsdfAtlas::createImpl - FactoryObject entry point
//=============================================================================

Result<MsdfAtlas*> MsdfAtlas::createImpl() {
    ytest("msdf-atlas-created", "MsdfAtlas created successfully");
    return Ok<MsdfAtlas*>(new MsdfAtlasImpl());
}

} // namespace yetty
