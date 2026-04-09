#pragma once

#include <yetty/font/font.hpp>
#include <yetty/msdf-atlas.hpp>
#include <yetty/gpu-allocator.hpp>
#include <yetty/core/factory-object.hpp>

#include <string>
#include <memory>

namespace yetty {

//=============================================================================
// MsMsdfFont - multi-style MSDF font for the terminal grid
//
// Composes MsdfAtlas for atlas/CDB/GPU management.
// Implements Font interface with style dispatch (Regular/Bold/Italic/BoldItalic).
//
// Header = interface, implementation in ms-msdf-font.cpp (MsMsdfFontImpl).
//=============================================================================
class MsMsdfFont : public Font,
                   public core::FactoryObject<MsMsdfFont> {
public:
    static Result<MsMsdfFont*> createImpl(const std::string& cdbBasePath,
                                           GpuAllocator* allocator);

    ~MsMsdfFont() override = default;

    // Atlas access (for GPU resources, font registration, etc.)
    virtual MsdfAtlas* atlas() const = 0;

    // Set fallback CDB for CJK characters (loaded lazily on demand)
    virtual void setFallbackCdb(const std::string& cdbPath) = 0;
};

} // namespace yetty
