// Prevent Windows min/max macros from conflicting with std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "triangulate.hpp"
#include "ypaint-buffer.hpp"
#include "ypaint-types.gen.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <yetty/gpu-texture-manager.h>
#include <yetty/msdf-glyph-data.h> // For GlyphMetadataGPU
#include <yetty/ypaint/canvas.h>
#include <yetty/ypaint/painter.h>
#include <ytrace/ytrace.hpp>

namespace yetty::ypaint {

// Free functions: TTF metrics reading
#include "painter-ttf.inc"

// Free functions: AABB computation
#include "painter-aabb.inc"

//=============================================================================
// PainterImpl
//=============================================================================

class PainterImpl : public Painter {
public:
  PainterImpl(font::FontManager* fontManager,
              GpuMemoryManager* gpuMemoryManager, uint32_t metaSlotIndex,
              bool scrollingMode = false)
      : _fontManager(fontManager),
        _gpuMemoryManager(gpuMemoryManager),
        _metaSlotIndex(metaSlotIndex), _scrollingMode(scrollingMode) {
    // Create canvas for grid management
    auto canvasResult = Canvas::create(scrollingMode);
    if (canvasResult) {
      _canvas = *canvasResult;
    }

    if (_fontManager) {
      _font = _fontManager->getDefaultMsMsdfFont();
      if (_font) {
        _atlas = _font->atlas();
      }
    }
  }

  ~PainterImpl() override = default;

  // Text API (addText, addRotatedText, measureTextWidth, fontAscent, fontDescent)
#include "painter-text.inc"

  // Font API (registerFont, addFont, addFontData, mapFontId, atlas access)
#include "painter-font.inc"

  // State management (primitiveCount, setBgColor, setSceneBounds, etc.)
  // Staging data access (gridStaging, glyphs, buildPrimStaging, hasContent)
  // Scrolling feature (scrollingMode, setCursorPosition, etc.)
  // Grid computation (sceneMinX, sceneMaxX, cellSizeX, gridWidth, etc.)
#include "painter-state.inc"

  // Buffer management (addYpaintBuffer, clear)
#include "painter-buffer.inc"

  // Grid helper methods (cellX, cellY, scrollLines, rebuildPackedGrid)
#include "painter-grid.inc"

  // GPU buffer lifecycle (primGpuSize, writePrimsGPU, declareBufferNeeds,
  // allocateBuffers, allocateTextures, writeTextures, writeBuffers,
  // needsBufferRealloc, needsTextureRealloc, setViewport, setView)
#include "painter-gpu.inc"

  // Text selection (buildGlyphSortedOrder, findNearestGlyphSorted,
  // setSelectionRange, getSelectedText)
#include "painter-selection.inc"

private:
  // Private helper methods (flushMetadata, computeDerivedSize, writeDerived)
  // Private member variables and internal structures
#include "painter-private.inc"
};

//=============================================================================
// Factory
//=============================================================================

Result<Painter*> Painter::createImpl(font::FontManager* fontManager,
                                     GpuMemoryManager* gpuMemoryManager,
                                     uint32_t metaSlotIndex,
                                     bool scrollingMode) {
  ytest("painter-created", "Painter created successfully");
  return Ok<Painter*>(new PainterImpl(fontManager, gpuMemoryManager,
                                      metaSlotIndex, scrollingMode));
}

} // namespace yetty::ypaint
