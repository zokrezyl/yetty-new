#pragma once

#include <yetty/msdf-glyph-data.hpp>
#include <yetty/msdf-cdb-provider.hpp>
#include <yetty/core/factory-object.hpp>
#include <yetty/core/result.hpp>

#include <string>
#include <vector>
#include <memory>

namespace yetty {

//=============================================================================
// MsdfAtlas - CPU-side MSDF atlas manager
//
// Manages: dynamic atlas with shelf packing, CDB file loading (fontId-based),
// glyph metadata. CPU-only - provides data for GpuResourceBinder.
//
// Composed by MsMsdfFont (terminal) and YDrawBase (multi-font text).
// Header = interface, implementation in msdf-atlas.cpp (MsdfAtlasImpl).
//=============================================================================
class MsdfAtlas : public core::FactoryObject<MsdfAtlas> {
public:
    static Result<MsdfAtlas*> createImpl();

    ~MsdfAtlas() override = default;

    // --- CDB management ---

    // Open a CDB file, returns fontId (index into internal vector).
    // Returns -1 on error.
    virtual int openCdb(const std::string& path) = 0;

    // Close a CDB file by fontId
    virtual void closeCdb(int fontId) = 0;

    // Close all open CDB files
    virtual void closeAllCdbs() = 0;

    // --- Glyph loading ---

    // Load glyph from a specific CDB (by fontId) into the shared atlas.
    // Per-fontId codepoint cache, no fallback logic.
    // Returns glyph index, or 0 (placeholder) if not found.
    virtual uint32_t loadGlyph(int fontId, uint32_t codepoint) = 0;

    // --- CDB registration with optional auto-generation ---

    // Open (or generate then open) a CDB file. Returns fontId.
    virtual int registerCdb(const std::string& cdbPath,
                            const std::string& ttfPath = "",
                            MsdfCdbProvider::Ptr provider = nullptr) = 0;

    // --- Reverse lookup ---
    virtual uint32_t getCodepoint(uint32_t glyphIndex) const = 0;

    // --- Atlas state ---
    virtual bool isDirty() const = 0;
    virtual void clearDirty() = 0;
    virtual bool hasPendingGlyphs() const = 0;
    virtual uint32_t getAtlasWidth() const = 0;
    virtual uint32_t getAtlasHeight() const = 0;
    virtual const std::vector<uint8_t>& getAtlasData() const = 0;

    // --- Font metrics ---
    virtual float getFontSize() const = 0;
    virtual float getLineHeight() const = 0;
    virtual float getPixelRange() const = 0;

    // --- Glyph metadata ---
    virtual const std::vector<GlyphMetadataGPU>& getGlyphMetadata() const = 0;
    virtual uint32_t getGlyphCount() const = 0;
    virtual uint32_t getResourceVersion() const = 0;
};

} // namespace yetty
