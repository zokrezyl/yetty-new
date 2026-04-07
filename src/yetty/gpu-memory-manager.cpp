#include <yetty/gpu-memory-manager.hpp>
#include <ytrace/ytrace.hpp>
#include <algorithm>
#include <cstring>
#include <array>
#include <vector>
#include <unordered_map>

namespace yetty {

// =============================================================================
// Helper classes for metadata (moved from card-buffer-manager.cpp)
// =============================================================================

class MetadataPool {
public:
    MetadataPool(uint32_t slotSize, uint32_t baseOffset, uint32_t slotCount);
    Result<uint32_t> allocate();
    Result<void> deallocate(uint32_t offset);
    uint32_t slotSize() const { return _slotSize; }
    uint32_t capacity() const { return _slotCount; }
    uint32_t used() const { return _slotCount - static_cast<uint32_t>(_freeSlots.size()); }

private:
    uint32_t _slotSize;
    uint32_t _baseOffset;
    uint32_t _slotCount;
    std::vector<uint32_t> _freeSlots;
};

class MetadataAllocator {
public:
    static constexpr uint32_t SLOT_32 = 32;
    static constexpr uint32_t SLOT_64 = 64;
    static constexpr uint32_t SLOT_128 = 128;
    static constexpr uint32_t SLOT_256 = 256;

    MetadataAllocator(uint32_t pool32Count, uint32_t pool64Count,
                      uint32_t pool128Count, uint32_t pool256Count);
    Result<MetadataHandle> allocate(uint32_t size);
    Result<void> deallocate(MetadataHandle handle);
    uint32_t totalSize() const { return _totalSize; }
    uint32_t highWaterMark() const { return _highWaterMark; }

private:
    MetadataPool* findPool(uint32_t size);
    MetadataPool* findPoolBySlotSize(uint32_t slotSize);

    MetadataPool _pool32;
    MetadataPool _pool64;
    MetadataPool _pool128;
    MetadataPool _pool256;
    uint32_t _totalSize;
    uint32_t _highWaterMark = 0;
};

class MetadataDirtyTracker {
public:
    void markDirty(uint32_t offset, uint32_t size) { _dirtyRanges.emplace_back(offset, size); }
    void clear() { _dirtyRanges.clear(); }
    bool hasDirty() const { return !_dirtyRanges.empty(); }

    std::vector<std::pair<uint32_t, uint32_t>> getCoalescedRanges(uint32_t maxGap = 64) {
        if (_dirtyRanges.empty()) return {};
        std::sort(_dirtyRanges.begin(), _dirtyRanges.end());
        std::vector<std::pair<uint32_t, uint32_t>> coalesced;
        uint32_t start = _dirtyRanges[0].first;
        uint32_t end = start + _dirtyRanges[0].second;
        for (size_t i = 1; i < _dirtyRanges.size(); ++i) {
            uint32_t rStart = _dirtyRanges[i].first;
            uint32_t rEnd = rStart + _dirtyRanges[i].second;
            if (rStart <= end + maxGap) {
                end = std::max(end, rEnd);
            } else {
                coalesced.emplace_back(start, end - start);
                start = rStart;
                end = rEnd;
            }
        }
        coalesced.emplace_back(start, end - start);
        return coalesced;
    }

private:
    std::vector<std::pair<uint32_t, uint32_t>> _dirtyRanges;
};

// =============================================================================
// MetadataPool / MetadataAllocator implementations
// =============================================================================

MetadataPool::MetadataPool(uint32_t slotSize, uint32_t baseOffset, uint32_t slotCount)
    : _slotSize(slotSize), _baseOffset(baseOffset), _slotCount(slotCount) {
    _freeSlots.reserve(slotCount);
    for (uint32_t i = 0; i < slotCount; ++i) {
        _freeSlots.push_back(baseOffset + (slotCount - 1 - i) * slotSize);
    }
}

Result<uint32_t> MetadataPool::allocate() {
    if (_freeSlots.empty()) return Err("MetadataPool: no free slots");
    uint32_t offset = _freeSlots.back();
    _freeSlots.pop_back();
    return Ok(offset);
}

Result<void> MetadataPool::deallocate(uint32_t offset) {
    if (offset < _baseOffset || offset >= _baseOffset + _slotCount * _slotSize) {
        return Err("MetadataPool: offset out of range");
    }
    if ((offset - _baseOffset) % _slotSize != 0) {
        return Err("MetadataPool: misaligned offset");
    }
    _freeSlots.push_back(offset);
    return Ok();
}

MetadataAllocator::MetadataAllocator(uint32_t pool32Count, uint32_t pool64Count,
                                     uint32_t pool128Count, uint32_t pool256Count)
    : _pool32(SLOT_32, 0, pool32Count)
    , _pool64(SLOT_64, pool32Count * SLOT_32, pool64Count)
    , _pool128(SLOT_128, pool32Count * SLOT_32 + pool64Count * SLOT_64, pool128Count)
    , _pool256(SLOT_256, pool32Count * SLOT_32 + pool64Count * SLOT_64 + pool128Count * SLOT_128, pool256Count)
    , _totalSize(pool32Count * SLOT_32 + pool64Count * SLOT_64 +
                 pool128Count * SLOT_128 + pool256Count * SLOT_256) {
}

MetadataPool* MetadataAllocator::findPool(uint32_t size) {
    if (size <= SLOT_32) return &_pool32;
    if (size <= SLOT_64) return &_pool64;
    if (size <= SLOT_128) return &_pool128;
    if (size <= SLOT_256) return &_pool256;
    return nullptr;
}

MetadataPool* MetadataAllocator::findPoolBySlotSize(uint32_t slotSize) {
    if (slotSize == SLOT_32) return &_pool32;
    if (slotSize == SLOT_64) return &_pool64;
    if (slotSize == SLOT_128) return &_pool128;
    if (slotSize == SLOT_256) return &_pool256;
    return nullptr;
}

Result<MetadataHandle> MetadataAllocator::allocate(uint32_t size) {
    MetadataPool* pool = findPool(size);
    if (!pool) return Err("MetadataAllocator: requested size too large");
    auto result = pool->allocate();
    if (!result) return Err("MetadataAllocator: pool exhausted");
    uint32_t offset = result.value();
    uint32_t slotSize = pool->slotSize();
    uint32_t endOffset = offset + slotSize;
    if (endOffset > _highWaterMark) _highWaterMark = endOffset;
    return Ok(MetadataHandle{offset, slotSize});
}

Result<void> MetadataAllocator::deallocate(MetadataHandle handle) {
    MetadataPool* pool = findPoolBySlotSize(handle.size);
    if (!pool) return Err("MetadataAllocator: invalid handle size");
    return pool->deallocate(handle.offset);
}

// =============================================================================
// GpuMemoryManagerImpl
// =============================================================================

class GpuMemoryManagerImpl : public GpuMemoryManager {
public:
    explicit GpuMemoryManagerImpl(Config config)
        : _config(config)
        , _metadataAllocator(config.metadata.pool32Count, config.metadata.pool64Count,
                             config.metadata.pool128Count, config.metadata.pool256Count) {}

    ~GpuMemoryManagerImpl() override = default;
    Result<void> init() noexcept;

    // Metadata
    Result<MetadataHandle> allocateMetadata(uint32_t size) override;
    Result<void> deallocateMetadata(MetadataHandle handle) override;
    Result<void> writeMetadata(MetadataHandle handle, const void* data, uint32_t size) override;
    Result<void> writeMetadataAt(MetadataHandle handle, uint32_t offset, const void* data, uint32_t size) override;

    // Manager accessors
    CardBufferManager* bufferManager() const override { return _bufferMgr; }
    GpuTextureManager* textureManager() const override { return _textureMgr; }

    // GpuResourceSet output
    GpuResourceSet getGpuResourceSet() const override;

private:
    Config _config;

    CardBufferManager* _bufferMgr = nullptr;
    GpuTextureManager* _textureMgr = nullptr;

    // Metadata (owned by GpuMemoryManager)
    MetadataAllocator _metadataAllocator;
    std::vector<uint32_t> _metadataCpuBuffer;
    MetadataDirtyTracker _metadataDirty;
};

// =============================================================================
// Factory
// =============================================================================

Result<GpuMemoryManager*> GpuMemoryManager::createImpl(Config config) {
    auto manager = new GpuMemoryManagerImpl(config);
    if (auto res = manager->init(); !res) {
        delete manager;
        return Err<GpuMemoryManager*>("GpuMemoryManager init failed", res);
    }
    return Ok<GpuMemoryManager*>(manager);
}

// =============================================================================
// Init
// =============================================================================

Result<void> GpuMemoryManagerImpl::init() noexcept {
    // Create texture manager
    auto texRes = GpuTextureManager::create(_config.texture);
    if (!texRes) return Err<void>("Failed to create GpuTextureManager", texRes);
    _textureMgr = texRes.value();

    // Create buffer manager
    auto bufRes = CardBufferManager::create();
    if (!bufRes) return Err<void>("Failed to create CardBufferManager", bufRes);
    _bufferMgr = bufRes.value();

    // Allocate metadata CPU buffer
    // Ceiling division: convert bytes to uint32_t count
    uint32_t u32Count = (_metadataAllocator.totalSize() + 3) / 4;
    _metadataCpuBuffer.resize(u32Count, 0);

    ydebug("GpuMemoryManager: initialized with metadata, buffer, and texture managers");
    return Ok();
}

// =============================================================================
// Metadata operations
// =============================================================================

Result<MetadataHandle> GpuMemoryManagerImpl::allocateMetadata(uint32_t size) {
    return _metadataAllocator.allocate(size);
}

Result<void> GpuMemoryManagerImpl::deallocateMetadata(MetadataHandle handle) {
    return _metadataAllocator.deallocate(handle);
}

Result<void> GpuMemoryManagerImpl::writeMetadata(MetadataHandle handle, const void* data, uint32_t size) {
    return writeMetadataAt(handle, 0, data, size);
}

Result<void> GpuMemoryManagerImpl::writeMetadataAt(MetadataHandle handle, uint32_t offset,
                                               const void* data, uint32_t size) {
    if (!handle.isValid()) return Err("writeMetadataAt: invalid handle");
    if (offset + size > handle.size) return Err("writeMetadataAt: write exceeds slot size");

    uint32_t bufferOffset = handle.offset + offset;
    uint8_t* bufferBytes = reinterpret_cast<uint8_t*>(_metadataCpuBuffer.data());
    std::memcpy(bufferBytes + bufferOffset, data, size);
    _metadataDirty.markDirty(bufferOffset, size);
    return Ok();
}

// =============================================================================
// GpuResourceSet output
// =============================================================================

GpuResourceSet GpuMemoryManagerImpl::getGpuResourceSet() const {
    GpuResourceSet resourceSet;
    resourceSet.name = "cardStorage";

    // Metadata buffer
    resourceSet.uniformData = reinterpret_cast<const uint8_t*>(_metadataCpuBuffer.data());
    resourceSet.uniformDataSize = static_cast<uint32_t>(_metadataCpuBuffer.size() * sizeof(uint32_t));

    // Storage buffer from CardBufferManager
    resourceSet.bufferData = _bufferMgr->bufferData();
    resourceSet.bufferSize = _bufferMgr->bufferSize();

    // Atlas texture from GpuTextureManager
    resourceSet.textureData = _textureMgr->atlasData();
    resourceSet.textureWidth = _textureMgr->atlasWidth();
    resourceSet.textureHeight = _textureMgr->atlasHeight();

    return resourceSet;
}

}  // namespace yetty
