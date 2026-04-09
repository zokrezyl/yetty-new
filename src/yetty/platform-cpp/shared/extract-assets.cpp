// Extract embedded assets to cache directory.
// Called by all desktop platforms after Config is created.

#include <yetty/platform/extract-assets.hpp>
#include <yetty/incbin-assets.hpp>
#include <yetty/config.hpp>
#include <ytrace/ytrace.hpp>

#include <filesystem>

namespace yetty {
namespace platform {

Result<void> extractAssets(Config* config) {
    // Derive cache base dir from config paths/shaders (parent of shaders dir)
    // e.g. ~/.cache/yetty/shaders -> ~/.cache/yetty
    std::filesystem::path shadersPath = config->get<std::string>("paths/shaders", "");
    if (shadersPath.empty()) {
        ydebug("extractAssets: no shaders path in config, skipping");
        return Ok();
    }
    std::filesystem::path cacheDir = shadersPath.parent_path();

    // Create IncbinAssets
    auto assetsResult = IncbinAssets::create();
    if (!assetsResult) {
        ydebug("extractAssets: no embedded assets (development build?)");
        return Ok();
    }
    auto* assets = *assetsResult;

    auto assetList = assets->list();
    ydebug("extractAssets: {} embedded assets, cacheDir={}", assetList.size(), cacheDir.string());

    if (assetList.empty()) {
        delete assets;
        return Ok();
    }

    // Check if extraction needed: version marker + file existence
    bool needsExtract = IncbinAssets::needsExtraction(cacheDir);

    if (!needsExtract) {
        // Version matches - check if any files are missing
        for (const auto& name : assetList) {
            if (!std::filesystem::exists(cacheDir / name)) {
                ydebug("extractAssets: missing file {}, will re-extract", name);
                needsExtract = true;
                break;
            }
        }
    }

    if (!needsExtract) {
        ydebug("extractAssets: assets already up-to-date");
        delete assets;
        return Ok();
    }

    // Extract all embedded assets to cache dir
    ydebug("extractAssets: extracting to {}", cacheDir.string());
    if (auto res = assets->extractTo(cacheDir); !res) {
        delete assets;
        return Err<void>("Failed to extract assets", res);
    }

    ydebug("extractAssets: done");
    delete assets;
    return Ok();
}

} // namespace platform
} // namespace yetty
