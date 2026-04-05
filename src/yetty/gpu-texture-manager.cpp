#include <yetty/gpu-texture-manager.hpp>
#include <ytrace/ytrace.hpp>
#include <algorithm>
#include <cstring>
#include <vector>
#include <unordered_map>

namespace yetty {

// =============================================================================
// Texture handle tracking data
// =============================================================================

struct TextureHandleData {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t atlasX = 0;
    uint32_t atlasY = 0;
    bool packed = false;
    bool dirty = false;
};

// =============================================================================
// GpuTextureManagerImpl
// =============================================================================

class GpuTextureManagerImpl : public GpuTextureManager {
public:
    explicit GpuTextureManagerImpl(Config config) noexcept;
    ~GpuTextureManagerImpl() override = default;

    Result<void> init() noexcept;

    // Card API
    Result<TextureHandle> allocate(uint32_t width, uint32_t height) override;
    Result<void> write(TextureHandle handle, const uint8_t* pixels) override;
    AtlasPosition getAtlasPosition(TextureHandle handle) const override;

    // gpu-screen / GpuMemoryManager API
    void clearHandles() override;
    Result<void> createAtlas() override;

    // Accessors
    uint32_t atlasWidth() const override { return _currentAtlasSize; }
    uint32_t atlasHeight() const override { return _currentAtlasSize; }
    const uint8_t* atlasData() const override { return _atlasData.data(); }
    uint32_t atlasDataSize() const override { return static_cast<uint32_t>(_atlasData.size()); }

    Stats getStats() const override;

private:
    Config _config;

    // CPU atlas buffer (RGBA8)
    std::vector<uint8_t> _atlasData;
    uint32_t _currentAtlasSize;
    uint32_t _maxAtlasSize;

    // Texture handle tracking
    std::unordered_map<uint32_t, TextureHandleData> _textureHandles;
    uint32_t _nextTextureHandleId = 1;  // 0 = invalid
};

// =============================================================================
// Factory
// =============================================================================

Result<GpuTextureManager*> GpuTextureManager::createImpl(Config config) {
    auto manager = new GpuTextureManagerImpl(config);
    if (auto res = manager->init(); !res) {
        delete manager;
        return Err<GpuTextureManager*>("Failed to initialize GpuTextureManager", res);
    }
    return Ok<GpuTextureManager*>(manager);
}

// =============================================================================
// Implementation
// =============================================================================

GpuTextureManagerImpl::GpuTextureManagerImpl(Config config) noexcept
    : _config(config)
    , _currentAtlasSize(config.initialAtlasSize)
    , _maxAtlasSize(config.maxAtlasSize) {
}

Result<void> GpuTextureManagerImpl::init() noexcept {
    // Atlas is lazily initialized on first createAtlas() call
    return Ok();
}

// =============================================================================
// Card API
// =============================================================================

Result<TextureHandle> GpuTextureManagerImpl::allocate(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return Err<TextureHandle>("allocate: width and height must be > 0");
    }

    uint32_t id = _nextTextureHandleId++;
    TextureHandleData data;
    data.width = width;
    data.height = height;
    _textureHandles[id] = data;

    ydebug("GpuTextureManager: allocated handle id={} {}x{}", id, width, height);
    return Ok(TextureHandle{id});
}

Result<void> GpuTextureManagerImpl::write(TextureHandle handle, const uint8_t* pixels) {
    if (_atlasData.empty()) {
        return Err("write: atlas not created yet (call createAtlas first)");
    }
    auto it = _textureHandles.find(handle.id);
    if (it == _textureHandles.end()) {
        return Err("write: invalid handle");
    }
    if (!it->second.packed) {
        return Err("write: handle not packed (call createAtlas first)");
    }
    if (!pixels) {
        return Err("write: null pixels");
    }

    const auto& data = it->second;

    // Copy pixels into CPU atlas buffer row by row
    uint32_t srcRowBytes = data.width * 4;  // RGBA8
    uint32_t dstRowBytes = _currentAtlasSize * 4;
    for (uint32_t y = 0; y < data.height; ++y) {
        uint32_t srcOffset = y * srcRowBytes;
        uint32_t dstOffset = (data.atlasY + y) * dstRowBytes + data.atlasX * 4;
        std::memcpy(_atlasData.data() + dstOffset, pixels + srcOffset, srcRowBytes);
    }

    it->second.dirty = true;

    ydebug("GpuTextureManager: write handle={} {}x{} at ({},{})",
           handle.id, data.width, data.height, data.atlasX, data.atlasY);
    return Ok();
}

AtlasPosition GpuTextureManagerImpl::getAtlasPosition(TextureHandle handle) const {
    auto it = _textureHandles.find(handle.id);
    if (it == _textureHandles.end() || !it->second.packed) {
        return {0, 0};
    }
    return {it->second.atlasX, it->second.atlasY};
}

// =============================================================================
// gpu-screen / GpuMemoryManager API
// =============================================================================

void GpuTextureManagerImpl::clearHandles() {
    _textureHandles.clear();
    _nextTextureHandleId = 1;
}

Result<void> GpuTextureManagerImpl::createAtlas() {
    // No textures — release atlas and reset size
    if (_textureHandles.empty()) {
        if (!_atlasData.empty()) {
            ydebug("GpuTextureManager: no textures, releasing atlas (was {}x{})",
                  _currentAtlasSize, _currentAtlasSize);
            _atlasData.clear();
            _atlasData.shrink_to_fit();
            _currentAtlasSize = _config.initialAtlasSize;
        }
        return Ok();
    }

    // Collect handles for packing
    struct PackEntry {
        uint32_t id;
        uint32_t width;
        uint32_t height;
    };
    std::vector<PackEntry> entries;
    entries.reserve(_textureHandles.size());

    for (const auto& [id, data] : _textureHandles) {
        if (data.width > 0 && data.height > 0) {
            entries.push_back({id, data.width, data.height});
        }
    }

    if (entries.empty()) {
        return Ok();
    }

    // Sort by height descending for better strip packing
    std::sort(entries.begin(), entries.end(), [](const PackEntry& a, const PackEntry& b) {
        return a.height > b.height;
    });

    // Row-based strip packing — assigns atlas positions to each handle
    auto packWithSize = [&](uint32_t atlasSize) -> uint32_t {
        uint32_t currentX = 0;
        uint32_t currentY = 0;
        uint32_t rowHeight = 0;

        for (const auto& entry : entries) {
            if (currentX + entry.width > atlasSize) {
                currentY += rowHeight;
                currentX = 0;
                rowHeight = 0;
            }

            auto& data = _textureHandles[entry.id];
            data.atlasX = currentX;
            data.atlasY = currentY;
            data.packed = true;
            data.dirty = true;

            currentX += entry.width;
            rowHeight = std::max(rowHeight, entry.height);
        }

        return currentY + rowHeight;
    };

    // Find smallest power-of-2 atlas size that fits
    uint32_t maxWidth = 0;
    for (const auto& entry : entries) {
        maxWidth = std::max(maxWidth, entry.width);
    }
    uint32_t neededSize = std::max(_config.initialAtlasSize, maxWidth);
    while (neededSize <= _maxAtlasSize) {
        uint32_t h = packWithSize(neededSize);
        if (h <= neededSize) break;
        neededSize *= 2;
    }

    if (neededSize > _maxAtlasSize) {
        yerror("GpuTextureManager: atlas overflow, cannot fit textures in {}x{}", _maxAtlasSize, _maxAtlasSize);
        // Mark all handles as not packed to prevent writes to invalid coords
        for (auto& [id, data] : _textureHandles) {
            data.packed = false;
        }
        return Err<void>("Atlas overflow - too many/large textures");
    }

    // Reallocate CPU atlas buffer if size changed
    if (neededSize != _currentAtlasSize || _atlasData.empty()) {
        ydebug("GpuTextureManager: atlas {}x{} -> {}x{}", _currentAtlasSize, _currentAtlasSize, neededSize, neededSize);
        _currentAtlasSize = neededSize;
        _atlasData.resize(_currentAtlasSize * _currentAtlasSize * 4, 0);  // RGBA8
        // Re-pack with final size (positions may differ after resize)
        packWithSize(_currentAtlasSize);
    }

    ydebug("GpuTextureManager: packed {} textures into {}x{} atlas",
          entries.size(), _currentAtlasSize, _currentAtlasSize);

    return Ok();
}

GpuTextureManager::Stats GpuTextureManagerImpl::getStats() const {
    uint32_t usedPixels = 0;
    for (const auto& [id, data] : _textureHandles) {
        if (data.packed) {
            usedPixels += data.width * data.height;
        }
    }

    return Stats{
        .textureCount = static_cast<uint32_t>(_textureHandles.size()),
        .atlasWidth = _currentAtlasSize,
        .atlasHeight = _currentAtlasSize,
        .usedPixels = usedPixels
    };
}

}  // namespace yetty
