# Terminal Screen Design

## Overview

TerminalScreen is the core rendering component of yetty. It manages a layered rendering
pipeline where each layer composites on top of the cached output of the layer below,
enabling fast scrolling by only re-rendering layers that changed.

## Layer Architecture

```
Layer 3: Static YPaint     — screen-fixed SDF/MSDF overlay (dialogs, HUD)
Layer 2: Cards              — independent SDF/MSDF sub-grids, scroll with terminal
Layer 1: Scrolling YPaint   — SDF/MSDF primitives, 1-to-1 with text grid, scroll together
Layer 0: Text Grid          — standard terminal text (TextCell buffer)
```

Each layer N is rendered on top of the cached texture of layer N-1. When only layer N
changes, layers 0..N-1 are already a cached texture — only N and above are re-rendered.

## Scrolling Model

### The Problem with Current Approach

Current canvas.cpp `scrollLines()` iterates ALL remaining lines and ALL their primitives
to decrement `gridOffset.row` — O(lines * prims_per_line) on every scroll event.

### Solution: Scroll Counter

A global `scrollCounter` (uint32) lives in the shared spatial structure. It increments
on every scroll. Primitives store the counter value at insertion time as their
`insertScroll` value.

```
On scroll:
  scrollCounter += numLines
  pop numLines from front of deque
  done — O(1), no iteration

On shader render:
  effectiveRow = prim.insertScroll - scrollCounter + prim.localRow
```

The shader subtracts to get the actual screen position. Zero CPU work on scroll
beyond the deque pop.

The `scrollCounter` is uploaded as part of the per-frame uniform data. Primitives
carry their `insertScroll` as word 0 (replaces the current `gridOffset` packed value).

## Text Grid Memory Layout

The text grid is a **flat contiguous `vector<TextCell>`** sized to `rows * cols`.
This is critical for performance:

- **GPU upload**: `getCellData()` returns a raw pointer — zero-copy to GPU buffer
- **Scrolling**: single `memmove` of `(rows-1) * cols * sizeof(TextCell)` bytes,
  hardware-optimized, cache-friendly
- **Cell access**: `buffer[row * cols + col]` — no indirection

A `vector<vector<TextCell>>` (row-per-vector) would scatter rows across heap,
require `rows` separate memcpy calls to linearize for GPU upload, and destroy
cache locality. The flat buffer is strictly better for the framebuffer path.

```cpp
// Visible grid — flat, contiguous, GPU-ready
vector<TextCell> _primaryBuffer;   // rows * cols
vector<TextCell> _altBuffer;       // alternate screen

// Scroll = memmove + clear last row
void scroll(int lines) {
    size_t lineBytes = cols * sizeof(TextCell);
    memmove(buf.data(), buf.data() + lines * cols, (rows - lines) * lineBytes);
    memset(buf.data() + (rows - lines) * cols, 0, lines * lineBytes);
}
```

### Scrollback

Lines pushed off the top go to a `deque<ScrollbackLine>` for copy-mode and
scroll-back viewing. Scrollback lines are compressed/trimmed (trailing spaces
removed). When scrolled back, a `viewBuffer` is composed from scrollback +
visible lines for GPU upload.

## YPaint Spatial Structure

Separate from the text grid but sharing the same row/col coordinate system
and scroll counter. The ypaint spatial data is a `deque<PrimLine>` where each
line holds primitives and per-cell references.

### Primitive Storage

Primitives are stored at the last (bottom-most) row they overlap. When that row
scrolls off, the primitive is destroyed. Grid cells in rows above hold `PrimRef`
entries pointing down to the storage row via relative offset.

```
struct PrimRef {
  uint16_t rowsAhead;   // relative offset to storage row
  uint16_t primIndex;   // index within storage row's prim list
};

struct PrimLine {
  vector<vector<float>> prims;    // primitives whose base is this line
  vector<vector<PrimRef>> cells;  // per-column prim references
};

deque<PrimLine> _primLines;       // parallel to text grid rows
```

Cards follow the same rule — anchored to their last overlapping row.

### Scroll Synchronization

Both the flat text buffer and the prim deque scroll together, driven by
the same scroll counter. The text buffer uses `memmove`, the prim deque
uses `pop_front`. Both are O(1) per scroll event (no primitive iteration).

## Data Structure Separation

YPaint data structures are independent of rendering mode. The same structures serve:

| Usage             | Grid size      | Scrolls? | Lifecycle              |
|-------------------|---------------|----------|------------------------|
| Scrolling ypaint  | terminal grid | yes      | rows scroll off        |
| Cards             | sub-grid NxM  | yes      | anchored to last row   |
| Static ypaint     | terminal grid | no       | explicit clear/replace |
| YPaint card       | sub-grid NxM  | yes      | anchored to last row   |

### YPaintBuffer (input)

Pure data container for SDF primitives, text spans, images, font blobs.
This is the input format — users build a YPaintBuffer and submit it.
No rendering logic, no GPU awareness.

### SpatialGrid (storage + spatial indexing)

Manages the deque of rows, primitive storage, grid cell references, scroll counter.
Shared between text and ypaint — single scroll mechanism.

Reusable for any grid size (full terminal or card sub-grid).

### Renderer (GPU lifecycle)

Sits on top of SpatialGrid. Handles:
- GPU buffer allocation (declare/allocate/write)
- Packed grid generation for GPU upload
- Metadata management
- Font/glyph processing

Same renderer code works for scrolling, static, and card modes — it just reads
from a SpatialGrid and writes to GPU buffers.

## TextCell

```cpp
struct TextCell {
    uint32_t glyph;                    // UTF codepoint or glyph index
    uint8_t fgR, fgG, fgB, alpha;     // Foreground RGBA
    uint8_t bgR, bgG, bgB, style;     // Background RGB + style byte
};
static_assert(sizeof(TextCell) == 12);
```

The primary buffer is `vector<TextCell>` sized to rows * cols. This is the GPU-friendly
representation — close to what the shader consumes.

## File Decomposition

Avoid a monolithic terminal-screen.cpp. Each concern gets its own file:

```
include/yetty/term/
  text-cell.hpp              — TextCell struct
  spatial-grid.hpp           — SpatialGrid (rows, prims, scroll counter, grid refs)
  terminal-screen.hpp        — TerminalScreen interface (FactoryObject)

src/yetty/term/
  spatial-grid.cpp           — SpatialGrid implementation
  terminal-screen.cpp        — TerminalScreenImpl (orchestrates layers, compositing)

src/yetty/ypaint/
  ypaint-buffer.h            — YPaintBuffer (pure data input, unchanged)
  canvas.cpp                 — replaced by SpatialGrid
  painter.cpp                — rendering logic, uses SpatialGrid
```

## GPU Data Flow

```
TextCell buffer ──────────────────────────> Layer 0 texture
                                                │
SpatialGrid (scrolling ypaint) ──> Renderer ──> Layer 1 on cached Layer 0
                                                │
Cards (sub-grid SpatialGrids) ──> Renderer ──> Layer 2 on cached Layer 1
                                                │
SpatialGrid (static ypaint) ──> Renderer ──> Layer 3 on cached Layer 2
                                                │
                                           Final frame
```

## Scroll Counter Detail

```
insertScroll stored per-primitive (word 0):
  bits [15:0]  = insertCol (cursor column at insertion)
  bits [31:16] = insertScrollCounter (scrollCounter value at insertion)

Per-frame uniform:
  scrollCounter (uint32) — current global scroll position

Shader computation:
  screenRow = prim.insertScrollCounter - uniforms.scrollCounter + prim.localRow
  screenCol = prim.insertCol + prim.localCol
```

When `screenRow < 0` the primitive is above the viewport (already scrolled past but
not yet popped from deque). When the storage row scrolls off the deque, the primitive
and all its refs are destroyed.

## Implementation Order

1. `TextCell` in `text-cell.hpp` (rename from GridCell)
2. `SpatialGrid` — unified row structure with scroll counter, replaces Canvas
3. `TerminalScreen` interface — layer orchestration
4. Adapt Painter to use SpatialGrid instead of Canvas
5. Card integration with SpatialGrid
6. Layer caching (render-to-texture compositing)
