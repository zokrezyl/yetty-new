#pragma once

#include <yetty/core/factory-object.hpp>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace yetty {

class IncbinAssets : public core::FactoryObject<IncbinAssets> {
public:
    static Result<IncbinAssets*> createImpl();

    virtual ~IncbinAssets() = default;

    // Get raw asset data by name (e.g., "shaders/terminal-screen.wgsl")
    virtual std::span<const uint8_t> get(std::string_view name) const = 0;

    // Get asset as string view (for text assets like shaders)
    virtual std::string_view getString(std::string_view name) const = 0;

    // Check if asset exists
    virtual bool has(std::string_view name) const = 0;

    // List all embedded asset names
    virtual std::vector<std::string> list() const = 0;

    // Extract all assets to directory
    virtual Result<void> extractTo(const std::filesystem::path& dir) const = 0;

    // Extract single asset to file
    virtual Result<void> extractAsset(std::string_view name,
                                       const std::filesystem::path& path) const = 0;

    // Check if assets need extraction (first run or version upgrade)
    static bool needsExtraction(const std::filesystem::path& cacheDir);

protected:
    IncbinAssets() = default;
};

} // namespace yetty
