#pragma once

#include <yetty/font/raw-font.hpp>
#include <yetty/core/factory-object.hpp>
#include <yetty/core/result.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace yetty::font {

/// RawFontManager - thread singleton for creating RawFont instances.
/// Manages the thread-local FreeType library internally.
class RawFontManager : public core::FactoryObject<RawFontManager> {
public:
    ~RawFontManager() override = default;

    static Result<RawFontManager*> createImpl();

    /// Create a RawFont from raw TTF/OTF data.
    virtual Result<RawFont*> createFromData(const uint8_t* data, size_t size,
                                             const std::string& name) = 0;

    /// Create a RawFont from a font file path.
    virtual Result<RawFont*> createFromFile(const std::string& path) = 0;

protected:
    RawFontManager() = default;
};

} // namespace yetty::font
