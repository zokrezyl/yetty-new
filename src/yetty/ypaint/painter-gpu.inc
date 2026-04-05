//=========================================================================
// GPU buffer lifecycle
//=========================================================================

// Compute GPU buffer size for primitives
uint32_t primGpuSize() const {
  if (!_canvas) {
    ydebug("primGpuSize: _canvas is null, returning 0");
    return 0;
  }
  return _canvas->primGpuSize();
}

// Write primitives to GPU buffer using canvas staging
Result<void> writePrimsGPU(float *buf, uint32_t bufBytes) {
  if (!_canvas) {
    return Err<void>("writePrimsGPU: _canvas is null");
  }
  if (!buf) {
    return Err<void>("writePrimsGPU: buf is null");
  }
  if (bufBytes == 0) {
    return Err<void>("writePrimsGPU: bufBytes is 0");
  }

  // Use canvas to build prim staging
  std::vector<uint32_t> staging;
  _canvas->buildPrimStaging(staging);

  if (staging.empty()) {
    ydebug("writePrimsGPU: staging is empty, nothing to write");
    return Ok();
  }

  // Copy staging data to GPU buffer
  uint32_t wordCount = std::min(static_cast<uint32_t>(staging.size()),
                                static_cast<uint32_t>(bufBytes / sizeof(uint32_t)));
  std::memcpy(buf, staging.data(), wordCount * sizeof(uint32_t));

  // Build _primWordOffsets from staging
  // Staging format: [offset0][offset1]...[offsetN-1][prim0 data]...
  uint32_t primCount = _canvas->primitiveCount();
  _primWordOffsets.clear();
  _primWordOffsets.reserve(primCount);
  for (uint32_t i = 0; i < primCount && i < staging.size(); i++) {
    _primWordOffsets.push_back(staging[i]);
  }
  return Ok();
}

Result<void> declareBufferNeeds() override {
  if (!_gpuMemoryManager)
    return Err<void>("declareBufferNeeds: no GpuMemoryManager");
  auto bufMgr = _gpuMemoryManager->bufferManager();

  // Reset handles — they will be re-allocated
  _primHandle = BufferHandle::invalid();
  _derivedHandle = BufferHandle::invalid();
  _primWordOffsets.clear();

  bool hasContent = _canvas && !_canvas->empty();
  if (!hasContent && _glyphs.empty()) {
    clearGridStaging();
    return Ok();
  }

  // Reserve compact prim storage
  uint32_t primBytes = primGpuSize();
  if (primBytes > 0) {
    bufMgr->reserve(primBytes);
  }

  // Build packed grid if dirty
  if (_canvas && _canvas->isDirty()) {
    rebuildPackedGrid();
  }

  // Reserve derived size (grid + glyphs + optional atlas metadata)
  uint32_t derivedSize = computeDerivedSize();
  if (derivedSize > 0) {
    bufMgr->reserve(derivedSize);
  }

  return Ok();
}

Result<void> allocateBuffers() override {
  if (!_gpuMemoryManager)
    return Err<void>("allocateBuffers: no GpuMemoryManager");
  auto bufMgr = _gpuMemoryManager->bufferManager();

  // Allocate and write compact prim data
  {
    uint32_t allocBytes = primGpuSize();
    if (allocBytes > 0) {
      auto primResult =
          bufMgr->allocateBuffer(_metaSlotIndex, "prims", allocBytes);
      if (!primResult)
        return Err<void>("allocateBuffers: prim alloc failed", primResult);
      _primHandle = *primResult;

      ydebug("allocateBuffers: primHandle.offset={} size={} words={}",
             _primHandle.offset, _primHandle.size,
             _primHandle.offset / sizeof(float));

      float *buf = reinterpret_cast<float *>(_primHandle.data);
      if (auto res = writePrimsGPU(buf, _primHandle.size); !res) {
        return Err<void>("allocateBuffers: writePrimsGPU failed", res);
      }
      bufMgr->markBufferDirty(_primHandle);
    }
  }

  // Allocate derived storage (grid + glyphs + atlas header)
  uint32_t derivedSize = computeDerivedSize();
  if (derivedSize > 0) {
    auto derivedResult =
        bufMgr->allocateBuffer(_metaSlotIndex, "derived", derivedSize);
    if (!derivedResult)
      return Err<void>("allocateBuffers: derived alloc failed",
                       derivedResult);
    _derivedHandle = *derivedResult;

    ydebug("allocateBuffers: derivedHandle.offset={} size={} words={}",
           _derivedHandle.offset, _derivedHandle.size,
           _derivedHandle.offset / sizeof(float));

    if (auto res = writeDerived(); !res)
      return res;
    bufMgr->markBufferDirty(_derivedHandle);
  }

  _bufferDirty = false;
  _metadataDirty = true;
  return Ok();
}

Result<void> allocateTextures() override {
  if (!_gpuMemoryManager) {
    return Err<void>("allocateTextures: no GpuMemoryManager");
  }

  auto texMgr = _gpuMemoryManager->textureManager();
  if (!texMgr) {
    return Err<void>("allocateTextures: textureManager is null");
  }

  // Allocate custom MSDF atlas if present
  if (_customAtlas) {
    uint32_t atlasW = _customAtlas->getAtlasWidth();
    uint32_t atlasH = _customAtlas->getAtlasHeight();
    const auto &atlasData = _customAtlas->getAtlasData();
    if (!atlasData.empty() && atlasW > 0 && atlasH > 0) {
      auto allocResult = texMgr->allocate(atlasW, atlasH);
      if (!allocResult) {
        return Err<void>("allocateTextures: msdf atlas failed", allocResult);
      }
      _atlasTextureHandle = *allocResult;
      _allocatedAtlasW = atlasW;
      _allocatedAtlasH = atlasH;
    } else {
      return Err<void>("allocateTextures: _customAtlas exists but invalid - atlasData.empty=" +
                       std::to_string(atlasData.empty()) + " atlasW=" + std::to_string(atlasW) +
                       " atlasH=" + std::to_string(atlasH));
    }
  }

  // Allocate texture space for each pending image
  for (auto &pi : _pendingImages) {
    if (pi.pixelWidth == 0 || pi.pixelHeight == 0 || pi.pixels.empty()) {
      return Err<void>("allocateTextures: invalid pending image - pixelWidth=" +
                       std::to_string(pi.pixelWidth) + " pixelHeight=" + std::to_string(pi.pixelHeight) +
                       " pixels.empty=" + std::to_string(pi.pixels.empty()));
    }
    auto allocResult = texMgr->allocate(pi.pixelWidth, pi.pixelHeight);
    if (!allocResult) {
      return Err<void>("allocateTextures: image allocation failed for " +
                       std::to_string(pi.pixelWidth) + "x" + std::to_string(pi.pixelHeight),
                       allocResult);
    }
    pi.texHandle = *allocResult;
  }

  return Ok();
}

Result<void> writeTextures() override {
  if (!_gpuMemoryManager) {
    return Err<void>("writeTextures: no GpuMemoryManager");
  }

  auto texMgr = _gpuMemoryManager->textureManager();
  if (!texMgr) {
    return Err<void>("writeTextures: textureManager is null");
  }

  // Write custom MSDF atlas
  if (_atlasTextureHandle.isValid() && _customAtlas) {
    const auto &atlasData = _customAtlas->getAtlasData();
    if (!atlasData.empty()) {
      if (auto res = texMgr->write(_atlasTextureHandle, atlasData.data());
          !res) {
        return Err<void>("writeTextures: msdf atlas write failed", res);
      }
    } else {
      return Err<void>("writeTextures: _atlasTextureHandle valid and _customAtlas exists but atlasData is empty");
    }

    // Now that the atlas is packed, write the atlas header into derived
    // buffer
    if (_derivedHandle.isValid() && _gpuAtlasHeaderOffset > 0) {
      auto atlasPos = texMgr->getAtlasPosition(_atlasTextureHandle);
      uint32_t msdfAtlasW = _customAtlas->getAtlasWidth();
      uint32_t msdfAtlasH = _customAtlas->getAtlasHeight();
      const auto &glyphMeta = _customAtlas->getGlyphMetadata();

      uint32_t headerLocalOff =
          _gpuAtlasHeaderOffset * sizeof(float) - _derivedHandle.offset;
      uint8_t *base = _derivedHandle.data + headerLocalOff;

      uint32_t atlasHeader[4] = {
          (atlasPos.x & 0xFFFF) | ((msdfAtlasW & 0xFFFF) << 16),
          (atlasPos.y & 0xFFFF) | ((msdfAtlasH & 0xFFFF) << 16),
          static_cast<uint32_t>(glyphMeta.size()), 0};
      std::memcpy(base, atlasHeader, sizeof(atlasHeader));

      _gpuMemoryManager->bufferManager()->markBufferDirty(_derivedHandle);
    } else {
      return Err<void>("writeTextures: _derivedHandle.isValid=" + std::to_string(_derivedHandle.isValid()) +
                       " _gpuAtlasHeaderOffset=" + std::to_string(_gpuAtlasHeaderOffset));
    }
  } else if (_customAtlas && !_atlasTextureHandle.isValid()) {
    return Err<void>("writeTextures: _customAtlas exists but _atlasTextureHandle is invalid");
  } else if (_atlasTextureHandle.isValid() && !_customAtlas) {
    return Err<void>("writeTextures: _atlasTextureHandle valid but _customAtlas is null");
  }

  // Write image pixels and update Image primitives with atlas coords
  for (auto &pi : _pendingImages) {
    if (!pi.texHandle.isValid()) {
      return Err<void>("writeTextures: pending image has invalid texHandle");
    }
    if (pi.primId == 0) {
      return Err<void>("writeTextures: pending image has primId=0");
    }

    // Write pixel data to atlas
    if (auto res = texMgr->write(pi.texHandle, pi.pixels.data()); !res) {
      return Err<void>("writeTextures: image write failed", res);
    }

    // Update Image primitive with real atlas coordinates
    // TODO: images need to be stored in canvas and updated here
    auto atlasPos = texMgr->getAtlasPosition(pi.texHandle);
    (void)atlasPos; // Suppress unused warning for now
  }

  // Mark buffer dirty so writeBuffers re-uploads with updated atlas coords
  if (!_pendingImages.empty()) {
    _bufferDirty = true;
  }

  return Ok();
}

Result<void> writeBuffers() override {
  if (!_gpuMemoryManager)
    return Err<void>("writeBuffers: no GpuMemoryManager");

  ydebug("writeBuffers: bufferDirty={} metaDirty={} primHandle={} "
         "derivedHandle={} scrollingMode={}",
         _bufferDirty, _metadataDirty, _primHandle.isValid(),
         _derivedHandle.isValid(), _scrollingMode);

  if (_bufferDirty) {
    auto bufMgr = _gpuMemoryManager->bufferManager();

    // Check if we have primitives
    bool hasPrims = _canvas && !_canvas->empty();

    if (_primHandle.isValid() && hasPrims) {
      ydebug("writeBuffers: re-writing prims");
      float *buf = reinterpret_cast<float *>(_primHandle.data);
      if (auto res = writePrimsGPU(buf, _primHandle.size); !res) {
        return Err<void>("writeBuffers: writePrimsGPU failed", res);
      }
      bufMgr->markBufferDirty(_primHandle);
    } else if (hasPrims && !_primHandle.isValid()) {
      return Err<void>("writeBuffers: hasPrims but _primHandle is invalid");
    }

    // Rebuild packed grid if dirty (important after scrolling!)
    if (_canvas && _canvas->isDirty()) {
      ydebug("writeBuffers: rebuilding packed grid");
      rebuildPackedGrid();
    }

    // Re-write derived (grid + glyphs + atlas header)
    if (_derivedHandle.isValid()) {
      ydebug("writeBuffers: re-writing derived");
      if (auto res = writeDerived(); !res)
        return res;
      bufMgr->markBufferDirty(_derivedHandle);
    }

    _bufferDirty = false;
    _metadataDirty = true;
  }

  if (_metadataDirty) {
    if (auto res = flushMetadata(); !res)
      return res;
    _metadataDirty = false;
  }

  return Ok();
}

bool needsBufferRealloc() const override {
  if (!_gpuMemoryManager)
    return false;
  uint32_t primNeeded = primGpuSize();
  if (_primHandle.isValid()) {
    if (primNeeded != _primHandle.size)
      return true;
  } else if (primNeeded > 0) {
    return true;
  }
  uint32_t derivedNeeded = computeDerivedSize();
  if (_derivedHandle.isValid()) {
    if (derivedNeeded > _derivedHandle.size)
      return true;
  } else if (derivedNeeded > 0) {
    return true;
  }
  return false;
}

bool needsTextureRealloc() const override {
  // Check custom atlas
  if (_customAtlas) {
    uint32_t atlasW = _customAtlas->getAtlasWidth();
    uint32_t atlasH = _customAtlas->getAtlasHeight();
    const auto &atlasData = _customAtlas->getAtlasData();
    if (!_atlasTextureHandle.isValid()) {
      if (!atlasData.empty())
        return true;
    } else if (atlasW != _allocatedAtlasW || atlasH != _allocatedAtlasH) {
      return true;
    }
  }

  // Check pending images
  for (const auto &pi : _pendingImages) {
    if (!pi.pixels.empty() && !pi.texHandle.isValid())
      return true;
  }

  return false;
}

void setViewport(uint32_t widthCells, uint32_t heightCells) override {
  if (_viewWidthCells != widthCells || _viewHeightCells != heightCells) {
    _viewWidthCells = widthCells;
    _viewHeightCells = heightCells;
    _metadataDirty = true;
  }
}

void setView(float zoom, float panX, float panY) override {
  if (_viewZoom != zoom || _viewPanX != panX || _viewPanY != panY) {
    _viewZoom = zoom;
    _viewPanX = panX;
    _viewPanY = panY;
    _metadataDirty = true;
  }
}
