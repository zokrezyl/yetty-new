//=========================================================================
// GPU helpers (private)
//=========================================================================

Result<void> flushMetadata() {
  if (!_gpuMemoryManager) {
    return Err<void>("flushMetadata: _gpuMemoryManager is null");
  }

  // Pack zoom as f16 in upper 16 bits of flags
  uint32_t zoomBits;
  {
    uint32_t f32bits;
    std::memcpy(&f32bits, &_viewZoom, sizeof(float));
    uint32_t sign = (f32bits >> 16) & 0x8000;
    int32_t exp = ((f32bits >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = (f32bits >> 13) & 0x3FF;
    if (exp <= 0) {
      exp = 0;
      mant = 0;
    } else if (exp >= 31) {
      exp = 31;
      mant = 0;
    }
    zoomBits = sign | (static_cast<uint32_t>(exp) << 10) | mant;
  }

  float contentW = _sceneMaxX - _sceneMinX;
  float contentH = _sceneMaxY - _sceneMinY;
  int16_t panXi16 = static_cast<int16_t>(std::clamp(
      _viewPanX / std::max(contentW, 1e-6f) * 16384.0f, -32768.0f, 32767.0f));
  // In scrolling mode, panY field is repurposed to hold cursorRow for cell lookup
  int16_t panYi16;
  if (_scrollingMode) {
    panYi16 = static_cast<int16_t>(_cursorRow);
  } else {
    panYi16 = static_cast<int16_t>(std::clamp(
        _viewPanY / std::max(contentH, 1e-6f) * 16384.0f, -32768.0f, 32767.0f));
  }

  uint32_t primCount = primitiveCount();
  uint32_t primOffset = 0;
  if (primCount > 0) {
    if (!_primHandle.isValid()) {
      return Err<void>("flushMetadata: have " + std::to_string(primCount) +
                       " primitives but _primHandle is invalid");
    }
    primOffset = _primHandle.offset / sizeof(float);
  }

  // Validate glyph state
  uint32_t glyphCount = static_cast<uint32_t>(_glyphs.size());
  if (glyphCount > 0 && _gpuGlyphOffset == 0) {
    return Err<void>("flushMetadata: have " + std::to_string(glyphCount) +
                     " glyphs but _gpuGlyphOffset is 0");
  }

  // Validate grid state
  if ((_gridWidth > 0 || _gridHeight > 0) && _gpuGridOffset == 0) {
    return Err<void>("flushMetadata: grid is " + std::to_string(_gridWidth) + "x" +
                     std::to_string(_gridHeight) + " but _gpuGridOffset is 0");
  }

  YPaintMetadata meta = {};
  meta.primitiveOffset = primOffset;
  meta.primitiveCount = primCount;
  meta.gridOffset = _gpuGridOffset;
  meta.gridWidth = _gridWidth;
  meta.gridHeight = _gridHeight;
  meta.cellSizeXY = YPaintMetadata::packCellSize(_cellSizeX, _cellSizeY);
  meta.glyphOffset = _gpuGlyphOffset;
  meta.glyphCount = glyphCount;
  std::memcpy(&meta.sceneMinX, &_sceneMinX, sizeof(float));
  std::memcpy(&meta.sceneMinY, &_sceneMinY, sizeof(float));
  std::memcpy(&meta.sceneMaxX, &_sceneMaxX, sizeof(float));
  std::memcpy(&meta.sceneMaxY, &_sceneMaxY, sizeof(float));
  meta.widthCells =
      (_viewWidthCells & 0xFFFF) |
      (static_cast<uint32_t>(static_cast<uint16_t>(panXi16)) << 16);
  meta.heightCells =
      (_viewHeightCells & 0xFFFF) |
      (static_cast<uint32_t>(static_cast<uint16_t>(panYi16)) << 16);
  uint32_t metaFlags = _flags;
  if (_scrollingMode) {
    metaFlags |= FLAG_SCROLLING_MODE;
  }
  meta.flags = (metaFlags & 0xFFFF) | (zoomBits << 16);
  meta.bgColor = _bgColor;

  ydebug("flushMetadata: primOff={} primCnt={} gridOff={} grid={}x{} "
         "glyphOff={} glyphCnt={} scene=[{},{}]-[{},{}]",
         meta.primitiveOffset, meta.primitiveCount, meta.gridOffset,
         meta.gridWidth, meta.gridHeight, meta.glyphOffset, meta.glyphCount,
         _sceneMinX, _sceneMinY, _sceneMaxX, _sceneMaxY);

  return _gpuMemoryManager->writeMetadata(MetadataHandle{_metaSlotIndex * 64, 64},
                                 &meta, sizeof(meta));
}

uint32_t computeDerivedSize() const {
  if (!_canvas) {
    yerror("computeDerivedSize: _canvas is null - returning 0");
    return 0;
  }
  const auto &gridStaging = _canvas->gridStaging();
  uint32_t gridBytes =
      static_cast<uint32_t>(gridStaging.size()) * sizeof(uint32_t);
  uint32_t glyphBytes =
      static_cast<uint32_t>(_glyphs.size() * sizeof(YPaintGlyph));
  uint32_t total = gridBytes + glyphBytes;
  if (_customAtlas) {
    uint32_t atlasHeaderBytes = 4 * sizeof(uint32_t);
    uint32_t glyphMetaBytes = static_cast<uint32_t>(
        _customAtlas->getGlyphMetadata().size() * sizeof(GlyphMetadataGPU));
    total += atlasHeaderBytes + glyphMetaBytes;
  }
  return total;
}

Result<void> writeDerived() {
  if (!_derivedHandle.isValid()) {
    return Err<void>("writeDerived: derived handle not allocated");
  }
  if (!_derivedHandle.data) {
    return Err<void>("writeDerived: _derivedHandle.data is null");
  }

  if (!_canvas) {
    return Err<void>("writeDerived: _canvas is null");
  }
  const auto &gridStaging = _canvas->gridStaging();

  uint8_t *base = _derivedHandle.data;
  uint32_t offset = 0;

  // Copy grid and translate prim indices to word offsets
  uint32_t gridBytes =
      static_cast<uint32_t>(gridStaging.size()) * sizeof(uint32_t);
  _gpuGridOffset = (_derivedHandle.offset + offset) / sizeof(float);
  ydebug("writeDerived: gridSize={} gridOffset={} primWordOffsets.size={}",
         gridStaging.size(), _gpuGridOffset, _primWordOffsets.size());
  // Debug: dump grid structure and glyph connections
  uint32_t numCells = _gridWidth * _gridHeight;
  ydebug("=== GPU BUFFER DUMP ===");
  ydebug("gridOffset={} glyphOffset={} numCells={}", _gpuGridOffset,
         (_derivedHandle.offset + gridBytes) / sizeof(float), numCells);

  // Find first few cells with glyph entries
  uint32_t cellsWithGlyphs = 0;
  for (uint32_t ci = 0; ci < numCells && cellsWithGlyphs < 5; ci++) {
    uint32_t packedOff = gridStaging[ci];
    if (packedOff >= gridStaging.size()) continue;
    uint32_t cnt = gridStaging[packedOff];
    bool hasGlyph = false;
    for (uint32_t j = 0; j < cnt; j++) {
      uint32_t idx = packedOff + 1 + j;
      if (idx >= gridStaging.size()) break;
      if ((gridStaging[idx] & 0x80000000u) != 0) { hasGlyph = true; break; }
    }
    if (hasGlyph) {
      uint32_t cellX = ci % _gridWidth;
      uint32_t cellY = ci / _gridWidth;
      ydebug("  cell[{},{}] packedOff={} cnt={}:", cellX, cellY, packedOff, cnt);
      for (uint32_t j = 0; j < cnt && j < 5; j++) {
        uint32_t rawVal = gridStaging[packedOff + 1 + j];
        if ((rawVal & 0x80000000u) != 0) {
          uint32_t gi = rawVal & 0x7FFFFFFFu;
          ydebug("    entry[{}]: GLYPH gi={}", j, gi);
        } else {
          ydebug("    entry[{}]: PRIM idx={}", j, rawVal);
        }
      }
      cellsWithGlyphs++;
    }
  }

  // Dump first 3 glyphs
  ydebug("  GLYPH DATA (first 3):");
  for (size_t i = 0; i < std::min(size_t(3), _glyphs.size()); i++) {
    const auto &g = _glyphs[i];
    uint16_t gIdx = g.glyphLayerFlags & 0xFFFF;
    ydebug("    glyph[{}]: x={:.1f} y={:.1f} w={:.1f} h={:.1f} atlasIdx={} color=0x{:08x}",
           i, g.x, g.y, g.width(), g.height(), gIdx, g.color);
  }
  ydebug("=== END GPU BUFFER DUMP ===");
  if (!gridStaging.empty()) {
    std::memcpy(base + offset, gridStaging.data(), gridBytes);
    // Translate prim indices in grid entries to word offsets
    uint32_t *gridPtr = reinterpret_cast<uint32_t *>(base + offset);
    uint32_t gridSize = static_cast<uint32_t>(gridStaging.size());
    uint32_t numCells = _gridWidth * _gridHeight;
    if (numCells > gridSize) {
      return Err<void>("writeDerived: numCells=" + std::to_string(numCells) +
                       " > gridSize=" + std::to_string(gridSize));
    }
    if (!_primWordOffsets.empty()) {
      for (uint32_t ci = 0; ci < numCells; ci++) {
        uint32_t packedOff = gridPtr[ci];
        if (packedOff >= gridSize) {
          return Err<void>("writeDerived: cell " + std::to_string(ci) +
                           " has packedOff=" + std::to_string(packedOff) +
                           " >= gridSize=" + std::to_string(gridSize));
        }
        uint32_t cnt = gridPtr[packedOff];
        for (uint32_t j = 0; j < cnt; j++) {
          uint32_t idx = packedOff + 1 + j;
          if (idx >= gridSize) {
            return Err<void>("writeDerived: cell " + std::to_string(ci) +
                             " entry " + std::to_string(j) + " idx=" + std::to_string(idx) +
                             " >= gridSize=" + std::to_string(gridSize));
          }
          uint32_t rawVal = gridPtr[idx];
          if ((rawVal & 0x80000000u) != 0) {
            // Glyph reference, not a primitive - skip translation
            continue;
          }
          if (rawVal >= static_cast<uint32_t>(_primWordOffsets.size())) {
            return Err<void>("writeDerived: cell " + std::to_string(ci) +
                             " entry " + std::to_string(j) + " rawVal=" + std::to_string(rawVal) +
                             " >= _primWordOffsets.size=" + std::to_string(_primWordOffsets.size()));
          }
          gridPtr[idx] = _primWordOffsets[rawVal];
        }
      }
    }
  }
  offset += gridBytes;

  // Copy glyphs
  uint32_t glyphBytes =
      static_cast<uint32_t>(_glyphs.size() * sizeof(YPaintGlyph));
  _gpuGlyphOffset = (_derivedHandle.offset + offset) / sizeof(float);
  ydebug("writeDerived: glyphOffset={} glyphCount={} glyphBytes={}",
         _gpuGlyphOffset, _glyphs.size(), glyphBytes);
  if (!_glyphs.empty()) {
    std::memcpy(base + offset, _glyphs.data(), glyphBytes);
    // Dump first 3 glyphs
    for (size_t i = 0; i < std::min(size_t(3), _glyphs.size()); i++) {
      const auto &g = _glyphs[i];
      uint16_t gIdx = g.glyphLayerFlags & 0xFFFF;
      ydebug("  glyph[{}]: x={:.1f} y={:.1f} w={:.1f} h={:.1f} idx={} color=0x{:08x}",
             i, g.x, g.y, g.width(), g.height(), gIdx, g.color);
    }
  }
  offset += glyphBytes;

  // Custom atlas header + glyph metadata
  if (_customAtlas) {
    _gpuAtlasHeaderOffset = (_derivedHandle.offset + offset) / sizeof(float);
    uint32_t atlasHeaderBytes = 4 * sizeof(uint32_t);

    if (_atlasTextureHandle.isValid()) {
      // Texture already allocated — write actual atlas position
      if (!_gpuMemoryManager) {
        return Err<void>("writeDerived: _gpuMemoryManager is null");
      }
      auto texMgr = _gpuMemoryManager->textureManager();
      if (!texMgr) {
        return Err<void>("writeDerived: textureManager is null");
      }
      auto atlasPos = texMgr->getAtlasPosition(_atlasTextureHandle);
      uint32_t msdfAtlasW = _customAtlas->getAtlasWidth();
      uint32_t msdfAtlasH = _customAtlas->getAtlasHeight();
      const auto &glyphMeta = _customAtlas->getGlyphMetadata();
      uint32_t atlasHeader[4] = {
          (atlasPos.x & 0xFFFF) | ((msdfAtlasW & 0xFFFF) << 16),
          (atlasPos.y & 0xFFFF) | ((msdfAtlasH & 0xFFFF) << 16),
          static_cast<uint32_t>(glyphMeta.size()), 0};
      std::memcpy(base + offset, atlasHeader, sizeof(atlasHeader));
    } else {
      // Texture not yet allocated - ERROR: allocateTextures() should have been called first
      return Err<void>("writeDerived: _customAtlas exists but _atlasTextureHandle is invalid - "
                       "allocateTextures() must be called before writeDerived()");
    }
    offset += atlasHeaderBytes;

    const auto &glyphMeta = _customAtlas->getGlyphMetadata();
    if (!glyphMeta.empty()) {
      uint32_t metaBytes =
          static_cast<uint32_t>(glyphMeta.size() * sizeof(GlyphMetadataGPU));
      std::memcpy(base + offset, glyphMeta.data(), metaBytes);
    }
  }

  return Ok();
}

//=========================================================================
// Data (private members)
//=========================================================================

// Empty grid staging fallback for const reference
const std::vector<uint32_t> _emptyGridStaging;

// Prim staging cache (rebuilt when _bufferDirty)
std::vector<uint32_t> _primStagingCache;

std::vector<YPaintGlyph> _glyphs;
std::vector<uint32_t> _glyphSortedOrder;

// Grid dimensions
uint32_t _gridWidth = 0;
uint32_t _gridHeight = 0;

// Scene bounds (initialized to extreme values, extended by addYpaintBuffer)
float _sceneMinX = 1e10f, _sceneMinY = 1e10f;
float _sceneMaxX = -1e10f, _sceneMaxY = -1e10f;
bool _hasExplicitBounds = false;

// Grid parameters
float _cellSizeX = DEFAULT_CELL_SIZE;
float _cellSizeY = DEFAULT_CELL_SIZE;
uint32_t _maxPrimsPerCell = DEFAULT_MAX_PRIMS_PER_CELL;
float _paddingX = 0.05f;  // Scene padding as fraction of extent (default 5%)
float _paddingY = 0.05f;

// Appearance
uint32_t _bgColor = 0xFF2E1A1A;
uint32_t _flags = 0;

// Font state
font::FontManager* _fontManager = nullptr;
font::MsMsdfFont* _font = nullptr;
MsdfAtlas* _atlas = nullptr;
std::unordered_map<std::string, int> _fontIdCache;

// Custom atlas
MsdfAtlas* _customAtlas = nullptr;

// TTF font metrics cache (fontId -> hhea ascender/descender)
std::unordered_map<int, TtfMetrics> _ttfMetricsCache;

// User-specified font ID mapping (user fontId -> atlas fontId)
std::unordered_map<int, int> _userFontIdMap;

// Buffer font ID -> atlas font ID mapping (populated during calculate())
std::unordered_map<int, int> _bufferFontIdMap;

// GPU state (managed by lifecycle methods)
GpuMemoryManager* _gpuMemoryManager = nullptr;
uint32_t _metaSlotIndex = 0;
BufferHandle _primHandle = BufferHandle::invalid();
BufferHandle _derivedHandle = BufferHandle::invalid();
std::vector<uint32_t> _primWordOffsets;
uint32_t _gpuGridOffset = 0;
uint32_t _gpuGlyphOffset = 0;
uint32_t _gpuAtlasHeaderOffset = 0;
TextureHandle _atlasTextureHandle = TextureHandle::invalid();
uint32_t _allocatedAtlasW = 0;
uint32_t _allocatedAtlasH = 0;

// Pending images (from buffer, to be allocated in texture atlas)
struct PendingImage {
  float x, y, w, h;
  uint32_t pixelWidth, pixelHeight;
  uint32_t layer;
  std::vector<uint8_t> pixels;
  TextureHandle texHandle = TextureHandle::invalid();
  uint32_t primId = 0; // ID of Image primitive in buffer
};
std::vector<PendingImage> _pendingImages;

bool _bufferDirty = true;
bool _metadataDirty = true;

// View state (set by card via setViewport/setView)
uint32_t _viewWidthCells = 0;
uint32_t _viewHeightCells = 0;
float _viewZoom = 1.0f;
float _viewPanX = 0.0f;
float _viewPanY = 0.0f;

// Scrolling mode state (terminal-style scrolling)
bool _scrollingMode = false;
uint16_t _cursorCol = 0;
uint16_t _cursorRow = 0;

//=========================================================================
// Canvas - handles grid and primitive storage
//=========================================================================

Canvas::Ptr _canvas;
