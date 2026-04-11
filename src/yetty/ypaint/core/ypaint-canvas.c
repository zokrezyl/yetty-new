// YPaint Canvas - Implementation
// Rolling offset approach for O(1) scrolling

#include <yetty/ypaint/core/ypaint-canvas.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

//=============================================================================
// Internal data structures
//=============================================================================

// Reference to a primitive in another line
typedef struct PrimRef {
    uint16_t linesAhead;  // relative offset to base line (0 = same line)
    uint16_t primIndex;   // index within base line's prims array
} PrimRef;

// Dynamic array of PrimRef
typedef struct PrimRefArray {
    PrimRef* data;
    uint32_t count;
    uint32_t capacity;
} PrimRefArray;

// A single grid cell
typedef struct GridCell {
    PrimRefArray refs;
} GridCell;

// A single primitive's data
typedef struct PrimData {
    float* data;
    uint32_t wordCount;
} PrimData;

// Dynamic array of PrimData
typedef struct PrimDataArray {
    PrimData* data;
    uint32_t count;
    uint32_t capacity;
} PrimDataArray;

// A single row/line in the grid
typedef struct GridLine {
    PrimDataArray prims;   // primitives whose BASE is this line
    GridCell* cells;       // grid cells for this line
    uint32_t cellCount;
    uint32_t cellCapacity;
} GridLine;

// Circular buffer for lines (deque-like)
typedef struct LineBuffer {
    GridLine* lines;
    uint32_t capacity;
    uint32_t head;         // index of first valid line
    uint32_t count;        // number of valid lines
} LineBuffer;

// Canvas structure
struct YPaintCanvas {
    bool scrollingMode;

    // Scene bounds
    float sceneMinX;
    float sceneMinY;
    float sceneMaxX;
    float sceneMaxY;

    // Cell size
    float cellSizeX;
    float cellSizeY;

    // Grid dimensions
    uint32_t gridWidth;
    uint32_t gridHeight;
    uint32_t maxPrimsPerCell;

    // Cursor (scrolling mode)
    uint16_t cursorCol;
    uint16_t cursorRow;

    // Rolling offset: absolute row index of line 0
    uint32_t row0Absolute;

    // Lines
    LineBuffer lines;

    // Packed grid staging
    uint32_t* gridStaging;
    uint32_t gridStagingCount;
    uint32_t gridStagingCapacity;
    bool dirty;

    // Primitive staging
    uint32_t* primStaging;
    uint32_t primStagingCount;
    uint32_t primStagingCapacity;
};

#define DEFAULT_MAX_PRIMS_PER_CELL 16
#define INITIAL_LINE_CAPACITY 64
#define INITIAL_CELL_CAPACITY 128
#define INITIAL_PRIM_CAPACITY 16
#define INITIAL_REF_CAPACITY 8
#define INITIAL_STAGING_CAPACITY 4096

//=============================================================================
// Helper: Dynamic arrays
//=============================================================================

static void primref_array_init(PrimRefArray* arr) {
    arr->data = NULL;
    arr->count = 0;
    arr->capacity = 0;
}

static void primref_array_free(PrimRefArray* arr) {
    free(arr->data);
    arr->data = NULL;
    arr->count = 0;
    arr->capacity = 0;
}

static void primref_array_push(PrimRefArray* arr, PrimRef ref) {
    if (arr->count >= arr->capacity) {
        uint32_t newCap = arr->capacity == 0 ? INITIAL_REF_CAPACITY : arr->capacity * 2;
        arr->data = (PrimRef*)realloc(arr->data, newCap * sizeof(PrimRef));
        arr->capacity = newCap;
    }
    arr->data[arr->count++] = ref;
}

static void primdata_array_init(PrimDataArray* arr) {
    arr->data = NULL;
    arr->count = 0;
    arr->capacity = 0;
}

static void primdata_array_free(PrimDataArray* arr) {
    for (uint32_t i = 0; i < arr->count; i++) {
        free(arr->data[i].data);
    }
    free(arr->data);
    arr->data = NULL;
    arr->count = 0;
    arr->capacity = 0;
}

static uint32_t primdata_array_push(PrimDataArray* arr, const float* data, uint32_t wordCount) {
    if (arr->count >= arr->capacity) {
        uint32_t newCap = arr->capacity == 0 ? INITIAL_PRIM_CAPACITY : arr->capacity * 2;
        arr->data = (PrimData*)realloc(arr->data, newCap * sizeof(PrimData));
        arr->capacity = newCap;
    }
    uint32_t idx = arr->count++;
    arr->data[idx].data = (float*)malloc(wordCount * sizeof(float));
    arr->data[idx].wordCount = wordCount;
    memcpy(arr->data[idx].data, data, wordCount * sizeof(float));
    return idx;
}

//=============================================================================
// Helper: GridLine
//=============================================================================

static void gridline_init(GridLine* line, uint32_t initialCells) {
    primdata_array_init(&line->prims);
    line->cells = NULL;
    line->cellCount = 0;
    line->cellCapacity = 0;
    if (initialCells > 0) {
        line->cells = (GridCell*)calloc(initialCells, sizeof(GridCell));
        line->cellCapacity = initialCells;
    }
}

static void gridline_free(GridLine* line) {
    primdata_array_free(&line->prims);
    for (uint32_t i = 0; i < line->cellCount; i++) {
        primref_array_free(&line->cells[i].refs);
    }
    free(line->cells);
    line->cells = NULL;
    line->cellCount = 0;
    line->cellCapacity = 0;
}

static void gridline_ensure_cells(GridLine* line, uint32_t minCells) {
    if (minCells <= line->cellCapacity) {
        if (minCells > line->cellCount) {
            // Initialize new cells
            for (uint32_t i = line->cellCount; i < minCells; i++) {
                primref_array_init(&line->cells[i].refs);
            }
            line->cellCount = minCells;
        }
        return;
    }

    uint32_t newCap = line->cellCapacity == 0 ? INITIAL_CELL_CAPACITY : line->cellCapacity;
    while (newCap < minCells) newCap *= 2;

    line->cells = (GridCell*)realloc(line->cells, newCap * sizeof(GridCell));
    for (uint32_t i = line->cellCapacity; i < newCap; i++) {
        primref_array_init(&line->cells[i].refs);
    }
    line->cellCapacity = newCap;
    line->cellCount = minCells;
}

//=============================================================================
// Helper: LineBuffer (circular buffer)
//=============================================================================

static void linebuffer_init(LineBuffer* buf) {
    buf->lines = NULL;
    buf->capacity = 0;
    buf->head = 0;
    buf->count = 0;
}

static void linebuffer_free(LineBuffer* buf) {
    for (uint32_t i = 0; i < buf->count; i++) {
        uint32_t idx = (buf->head + i) % buf->capacity;
        gridline_free(&buf->lines[idx]);
    }
    free(buf->lines);
    buf->lines = NULL;
    buf->capacity = 0;
    buf->head = 0;
    buf->count = 0;
}

static GridLine* linebuffer_get(LineBuffer* buf, uint32_t index) {
    if (index >= buf->count) return NULL;
    uint32_t idx = (buf->head + index) % buf->capacity;
    return &buf->lines[idx];
}

static void linebuffer_ensure_count(LineBuffer* buf, uint32_t minCount, uint32_t gridWidth) {
    // Grow capacity if needed
    if (minCount > buf->capacity) {
        uint32_t newCap = buf->capacity == 0 ? INITIAL_LINE_CAPACITY : buf->capacity;
        while (newCap < minCount) newCap *= 2;

        GridLine* newLines = (GridLine*)calloc(newCap, sizeof(GridLine));

        // Copy existing lines to new buffer (linearize)
        for (uint32_t i = 0; i < buf->count; i++) {
            uint32_t oldIdx = (buf->head + i) % buf->capacity;
            newLines[i] = buf->lines[oldIdx];
        }

        free(buf->lines);
        buf->lines = newLines;
        buf->capacity = newCap;
        buf->head = 0;
    }

    // Initialize new lines
    while (buf->count < minCount) {
        uint32_t idx = (buf->head + buf->count) % buf->capacity;
        gridline_init(&buf->lines[idx], gridWidth);
        buf->count++;
    }
}

static void linebuffer_pop_front(LineBuffer* buf, uint32_t count) {
    for (uint32_t i = 0; i < count && buf->count > 0; i++) {
        gridline_free(&buf->lines[buf->head]);
        gridline_init(&buf->lines[buf->head], 0); // Reset for reuse
        buf->head = (buf->head + 1) % buf->capacity;
        buf->count--;
    }
}

//=============================================================================
// Canvas implementation
//=============================================================================

YPaintCanvasHandle ypaint_canvas_create(bool scrollingMode) {
    struct YPaintCanvas* canvas = (struct YPaintCanvas*)calloc(1, sizeof(struct YPaintCanvas));
    if (!canvas) return NULL;

    canvas->scrollingMode = scrollingMode;
    canvas->maxPrimsPerCell = DEFAULT_MAX_PRIMS_PER_CELL;
    canvas->dirty = true;
    canvas->row0Absolute = 0;

    linebuffer_init(&canvas->lines);

    return canvas;
}

void ypaint_canvas_destroy(YPaintCanvasHandle canvas) {
    if (!canvas) return;

    linebuffer_free(&canvas->lines);
    free(canvas->gridStaging);
    free(canvas->primStaging);
    free(canvas);
}

//=============================================================================
// Configuration
//=============================================================================

static void update_grid_dimensions(YPaintCanvasHandle canvas) {
    if (canvas->cellSizeX <= 0.0f || canvas->cellSizeY <= 0.0f) {
        canvas->gridWidth = 0;
        canvas->gridHeight = 0;
        return;
    }

    float sceneW = canvas->sceneMaxX - canvas->sceneMinX;
    float sceneH = canvas->sceneMaxY - canvas->sceneMinY;

    if (sceneW <= 0.0f || sceneH <= 0.0f) {
        canvas->gridWidth = 0;
        canvas->gridHeight = 0;
        return;
    }

    canvas->gridWidth = (uint32_t)ceilf(sceneW / canvas->cellSizeX);
    canvas->gridHeight = (uint32_t)ceilf(sceneH / canvas->cellSizeY);
    if (canvas->gridWidth < 1) canvas->gridWidth = 1;
    if (canvas->gridHeight < 1) canvas->gridHeight = 1;
}

void ypaint_canvas_set_scene_bounds(YPaintCanvasHandle canvas,
                                     float minX, float minY,
                                     float maxX, float maxY) {
    if (!canvas) return;
    canvas->sceneMinX = minX;
    canvas->sceneMinY = minY;
    canvas->sceneMaxX = maxX;
    canvas->sceneMaxY = maxY;
    update_grid_dimensions(canvas);
    canvas->dirty = true;
}

void ypaint_canvas_set_cell_size(YPaintCanvasHandle canvas,
                                  float sizeX, float sizeY) {
    if (!canvas) return;
    canvas->cellSizeX = sizeX;
    canvas->cellSizeY = sizeY;
    update_grid_dimensions(canvas);
    canvas->dirty = true;
}

void ypaint_canvas_set_max_prims_per_cell(YPaintCanvasHandle canvas, uint32_t max) {
    if (canvas) canvas->maxPrimsPerCell = max;
}

//=============================================================================
// Accessors
//=============================================================================

bool ypaint_canvas_scrolling_mode(YPaintCanvasHandle canvas) {
    return canvas ? canvas->scrollingMode : false;
}

float ypaint_canvas_scene_min_x(YPaintCanvasHandle canvas) {
    return canvas ? canvas->sceneMinX : 0.0f;
}

float ypaint_canvas_scene_min_y(YPaintCanvasHandle canvas) {
    return canvas ? canvas->sceneMinY : 0.0f;
}

float ypaint_canvas_scene_max_x(YPaintCanvasHandle canvas) {
    return canvas ? canvas->sceneMaxX : 0.0f;
}

float ypaint_canvas_scene_max_y(YPaintCanvasHandle canvas) {
    return canvas ? canvas->sceneMaxY : 0.0f;
}

float ypaint_canvas_cell_size_x(YPaintCanvasHandle canvas) {
    return canvas ? canvas->cellSizeX : 0.0f;
}

float ypaint_canvas_cell_size_y(YPaintCanvasHandle canvas) {
    return canvas ? canvas->cellSizeY : 0.0f;
}

uint32_t ypaint_canvas_grid_width(YPaintCanvasHandle canvas) {
    return canvas ? canvas->gridWidth : 0;
}

uint32_t ypaint_canvas_grid_height(YPaintCanvasHandle canvas) {
    return canvas ? canvas->gridHeight : 0;
}

uint32_t ypaint_canvas_max_prims_per_cell(YPaintCanvasHandle canvas) {
    return canvas ? canvas->maxPrimsPerCell : 0;
}

uint32_t ypaint_canvas_line_count(YPaintCanvasHandle canvas) {
    return canvas ? canvas->lines.count : 0;
}

uint32_t ypaint_canvas_height_in_lines(YPaintCanvasHandle canvas) {
    if (!canvas || canvas->cellSizeY <= 0.0f) return 0;
    float sceneH = canvas->sceneMaxY - canvas->sceneMinY;
    return (uint32_t)ceilf(sceneH / canvas->cellSizeY);
}

//=============================================================================
// Cursor
//=============================================================================

void ypaint_canvas_set_cursor(YPaintCanvasHandle canvas, uint16_t col, uint16_t row) {
    if (canvas) {
        canvas->cursorCol = col;
        canvas->cursorRow = row;
    }
}

uint16_t ypaint_canvas_cursor_col(YPaintCanvasHandle canvas) {
    return canvas ? canvas->cursorCol : 0;
}

uint16_t ypaint_canvas_cursor_row(YPaintCanvasHandle canvas) {
    return canvas ? canvas->cursorRow : 0;
}

//=============================================================================
// Rolling offset
//=============================================================================

uint32_t ypaint_canvas_row0_absolute(YPaintCanvasHandle canvas) {
    return canvas ? canvas->row0Absolute : 0;
}

//=============================================================================
// Primitive management
//=============================================================================

static uint32_t cell_x_from_world(YPaintCanvasHandle canvas, float worldX) {
    if (canvas->gridWidth == 0 || canvas->cellSizeX <= 0.0f) return 0;
    float normalized = (worldX - canvas->sceneMinX) / canvas->cellSizeX;
    if (normalized < 0.0f) return 0;
    if (normalized >= (float)canvas->gridWidth) return canvas->gridWidth - 1;
    return (uint32_t)normalized;
}

void ypaint_canvas_add_primitive(YPaintCanvasHandle canvas,
                                  const float* primData, uint32_t wordCount,
                                  float aabbMinX, float aabbMinY,
                                  float aabbMaxX, float aabbMaxY) {
    if (!canvas || !primData || wordCount == 0 || wordCount > 32) return;

    // Offset AABB Y by cursor position for grid placement
    float yOffset = canvas->cursorRow * canvas->cellSizeY;
    aabbMinY += yOffset;
    aabbMaxY += yOffset;

    // Primitive data Y coords stay relative to the line (handled in shader via rolling_row)

    float baseY = (canvas->sceneMinY > 1e9f) ? 0.0f : canvas->sceneMinY;

    // Row range within primitive's AABB
    int32_t localMinRow = (int32_t)floorf((aabbMinY - baseY) / canvas->cellSizeY);
    int32_t localMaxRow = (int32_t)floorf((aabbMaxY - baseY) / canvas->cellSizeY);
    if (localMinRow < 0) localMinRow = 0;
    if (localMaxRow < 0) localMaxRow = 0;

    uint32_t primMinRow = (uint32_t)localMinRow;
    uint32_t primMaxRow = (uint32_t)localMaxRow;

    // Ensure lines exist
    linebuffer_ensure_count(&canvas->lines, primMaxRow + 1, canvas->gridWidth);

    // Prepend anchorRow (cursor's absolute row) to primitive data
    // Shader uses this for y_offset calculation
    uint32_t anchorRow = canvas->row0Absolute + canvas->cursorRow;
    float dataWithAnchor[33];
    memcpy(&dataWithAnchor[0], &anchorRow, sizeof(uint32_t));
    memcpy(&dataWithAnchor[1], primData, wordCount * sizeof(float));

    // Store primitive at primMaxRow (bottom of AABB - for scroll deletion)
    GridLine* baseLine = linebuffer_get(&canvas->lines, primMaxRow);
    uint32_t primIndex = primdata_array_push(&baseLine->prims, dataWithAnchor, wordCount + 1);

    // Add grid cell references
    uint32_t cellMinX = cell_x_from_world(canvas, aabbMinX);
    uint32_t cellMaxX = cell_x_from_world(canvas, aabbMaxX);

    for (uint32_t row = primMinRow; row <= primMaxRow; row++) {
        GridLine* line = linebuffer_get(&canvas->lines, row);
        gridline_ensure_cells(line, cellMaxX + 1);

        uint16_t linesAhead = (uint16_t)(primMaxRow - row);
        for (uint32_t cx = cellMinX; cx <= cellMaxX; cx++) {
            PrimRef ref = { linesAhead, (uint16_t)primIndex };
            primref_array_push(&line->cells[cx].refs, ref);
        }
    }

    canvas->dirty = true;
}

//=============================================================================
// Scrolling
//=============================================================================

void ypaint_canvas_scroll_lines(YPaintCanvasHandle canvas, uint16_t numLines) {
    if (!canvas || numLines == 0 || canvas->lines.count == 0) return;

    // Pop lines from front - primitives in those lines are deleted
    linebuffer_pop_front(&canvas->lines, numLines);

    // Update rolling offset - this is the key: O(1) update!
    canvas->row0Absolute += numLines;

    // Update cursor
    if (canvas->cursorRow >= numLines) {
        canvas->cursorRow -= numLines;
    } else {
        canvas->cursorRow = 0;
    }

    canvas->dirty = true;
}

//=============================================================================
// Packed GPU format
//=============================================================================

void ypaint_canvas_mark_dirty(YPaintCanvasHandle canvas) {
    if (canvas) canvas->dirty = true;
}

bool ypaint_canvas_is_dirty(YPaintCanvasHandle canvas) {
    return canvas ? canvas->dirty : false;
}

static void ensure_grid_staging(YPaintCanvasHandle canvas, uint32_t minSize) {
    if (minSize <= canvas->gridStagingCapacity) return;

    uint32_t newCap = canvas->gridStagingCapacity == 0 ? INITIAL_STAGING_CAPACITY : canvas->gridStagingCapacity;
    while (newCap < minSize) newCap *= 2;

    canvas->gridStaging = (uint32_t*)realloc(canvas->gridStaging, newCap * sizeof(uint32_t));
    canvas->gridStagingCapacity = newCap;
}

void ypaint_canvas_rebuild_grid(YPaintCanvasHandle canvas) {
    ypaint_canvas_rebuild_grid_with_glyphs(canvas, 0, NULL, NULL);
}

void ypaint_canvas_rebuild_grid_with_glyphs(YPaintCanvasHandle canvas,
                                             uint32_t glyphCount,
                                             YPaintGlyphBoundsFunc boundsFunc,
                                             void* userData) {
    if (!canvas) return;
    if (!canvas->dirty && canvas->gridStagingCount > 0) return;

    // Build line base prim index mapping
    uint32_t totalPrims = 0;
    uint32_t* lineBasePrimIdx = NULL;
    if (canvas->lines.count > 0) {
        lineBasePrimIdx = (uint32_t*)malloc(canvas->lines.count * sizeof(uint32_t));
        for (uint32_t i = 0; i < canvas->lines.count; i++) {
            lineBasePrimIdx[i] = totalPrims;
            GridLine* line = linebuffer_get(&canvas->lines, i);
            totalPrims += line->prims.count;
        }
    }

    // Compute grid dimensions
    uint32_t gridH = ypaint_canvas_height_in_lines(canvas);
    uint32_t gridW = 0;
    if (canvas->cellSizeX > 0.0f && canvas->sceneMaxX > canvas->sceneMinX) {
        gridW = (uint32_t)ceilf((canvas->sceneMaxX - canvas->sceneMinX) / canvas->cellSizeX);
    }

    // Extend for actual lines
    if (canvas->lines.count > gridH) gridH = canvas->lines.count;
    for (uint32_t i = 0; i < canvas->lines.count; i++) {
        GridLine* line = linebuffer_get(&canvas->lines, i);
        if (line->cellCount > gridW) gridW = line->cellCount;
    }

    // Extend for glyphs
    if (glyphCount > 0 && boundsFunc && canvas->cellSizeX > 0 && canvas->cellSizeY > 0) {
        for (uint32_t gi = 0; gi < glyphCount; gi++) {
            float gMinX, gMinY, gMaxX, gMaxY;
            boundsFunc(userData, gi, &gMinX, &gMinY, &gMaxX, &gMaxY);
            uint32_t maxCellX = (uint32_t)fmaxf(0.0f, floorf((gMaxX - canvas->sceneMinX) / canvas->cellSizeX)) + 1;
            uint32_t maxCellY = (uint32_t)fmaxf(0.0f, floorf((gMaxY - canvas->sceneMinY) / canvas->cellSizeY)) + 1;
            if (maxCellX > gridW) gridW = maxCellX;
            if (maxCellY > gridH) gridH = maxCellY;
        }
    }

    canvas->gridWidth = gridW;
    canvas->gridHeight = gridH;

    if (gridW == 0 || gridH == 0) {
        canvas->gridStagingCount = 0;
        canvas->dirty = false;
        free(lineBasePrimIdx);
        return;
    }

    uint32_t numCells = gridW * gridH;

    // Build glyph->cell mapping
    uint32_t** cellGlyphs = NULL;
    uint32_t* cellGlyphCounts = NULL;
    if (glyphCount > 0 && boundsFunc) {
        cellGlyphs = (uint32_t**)calloc(numCells, sizeof(uint32_t*));
        cellGlyphCounts = (uint32_t*)calloc(numCells, sizeof(uint32_t));

        for (uint32_t gi = 0; gi < glyphCount; gi++) {
            float gMinX, gMinY, gMaxX, gMaxY;
            boundsFunc(userData, gi, &gMinX, &gMinY, &gMaxX, &gMaxY);

            int32_t cMinX = (int32_t)floorf((gMinX - canvas->sceneMinX) / canvas->cellSizeX);
            int32_t cMinY = (int32_t)floorf((gMinY - canvas->sceneMinY) / canvas->cellSizeY);
            int32_t cMaxX = (int32_t)floorf((gMaxX - canvas->sceneMinX) / canvas->cellSizeX);
            int32_t cMaxY = (int32_t)floorf((gMaxY - canvas->sceneMinY) / canvas->cellSizeY);

            if (cMinX < 0) cMinX = 0;
            if (cMinY < 0) cMinY = 0;
            if (cMaxX >= (int32_t)gridW) cMaxX = gridW - 1;
            if (cMaxY >= (int32_t)gridH) cMaxY = gridH - 1;

            for (int32_t cy = cMinY; cy <= cMaxY; cy++) {
                for (int32_t cx = cMinX; cx <= cMaxX; cx++) {
                    uint32_t cellIdx = cy * gridW + cx;
                    uint32_t cnt = cellGlyphCounts[cellIdx];
                    cellGlyphs[cellIdx] = (uint32_t*)realloc(cellGlyphs[cellIdx], (cnt + 1) * sizeof(uint32_t));
                    cellGlyphs[cellIdx][cnt] = gi;
                    cellGlyphCounts[cellIdx]++;
                }
            }
        }
    }

    // Build staging: offset table then appended entries
    ensure_grid_staging(canvas, numCells * 4); // rough estimate
    canvas->gridStagingCount = numCells;

    for (uint32_t y = 0; y < gridH; y++) {
        bool hasLine = y < canvas->lines.count;
        GridLine* line = hasLine ? linebuffer_get(&canvas->lines, y) : NULL;
        uint32_t lineCellCount = line ? line->cellCount : 0;

        for (uint32_t x = 0; x < gridW; x++) {
            uint32_t cellIdx = y * gridW + x;

            ensure_grid_staging(canvas, canvas->gridStagingCount + 2);
            canvas->gridStaging[cellIdx] = canvas->gridStagingCount;

            uint32_t countPos = canvas->gridStagingCount++;
            ensure_grid_staging(canvas, canvas->gridStagingCount + 1);
            canvas->gridStaging[countPos] = 0;
            uint32_t count = 0;

            // Add prim entries
            if (hasLine && x < lineCellCount) {
                GridCell* cell = &line->cells[x];
                for (uint32_t ri = 0; ri < cell->refs.count; ri++) {
                    PrimRef* ref = &cell->refs.data[ri];
                    uint32_t bl = y + ref->linesAhead;
                    if (bl < canvas->lines.count && lineBasePrimIdx) {
                        ensure_grid_staging(canvas, canvas->gridStagingCount + 1);
                        canvas->gridStaging[canvas->gridStagingCount++] = lineBasePrimIdx[bl] + ref->primIndex;
                        count++;
                    }
                }
            }

            // Add glyph entries with GLYPH_BIT
            if (cellGlyphs && cellGlyphCounts) {
                for (uint32_t gi = 0; gi < cellGlyphCounts[cellIdx]; gi++) {
                    ensure_grid_staging(canvas, canvas->gridStagingCount + 1);
                    canvas->gridStaging[canvas->gridStagingCount++] = cellGlyphs[cellIdx][gi] | YPAINT_GLYPH_BIT;
                    count++;
                }
            }

            canvas->gridStaging[countPos] = count;
        }
    }

    // Cleanup
    free(lineBasePrimIdx);
    if (cellGlyphs) {
        for (uint32_t i = 0; i < numCells; i++) {
            free(cellGlyphs[i]);
        }
        free(cellGlyphs);
        free(cellGlyphCounts);
    }

    canvas->dirty = false;
}

const uint32_t* ypaint_canvas_grid_data(YPaintCanvasHandle canvas) {
    return canvas ? canvas->gridStaging : NULL;
}

uint32_t ypaint_canvas_grid_word_count(YPaintCanvasHandle canvas) {
    return canvas ? canvas->gridStagingCount : 0;
}

void ypaint_canvas_clear_staging(YPaintCanvasHandle canvas) {
    if (canvas) {
        canvas->gridStagingCount = 0;
        canvas->primStagingCount = 0;
    }
}

//=============================================================================
// Primitive staging
//=============================================================================

static void ensure_prim_staging(YPaintCanvasHandle canvas, uint32_t minSize) {
    if (minSize <= canvas->primStagingCapacity) return;

    uint32_t newCap = canvas->primStagingCapacity == 0 ? INITIAL_STAGING_CAPACITY : canvas->primStagingCapacity;
    while (newCap < minSize) newCap *= 2;

    canvas->primStaging = (uint32_t*)realloc(canvas->primStaging, newCap * sizeof(uint32_t));
    canvas->primStagingCapacity = newCap;
}

const uint32_t* ypaint_canvas_build_prim_staging(YPaintCanvasHandle canvas, uint32_t* wordCount) {
    if (!canvas) {
        if (wordCount) *wordCount = 0;
        return NULL;
    }

    // Count primitives and total words (anchorRow already included in wordCount)
    uint32_t primCount = 0;
    uint32_t totalWords = 0;
    for (uint32_t i = 0; i < canvas->lines.count; i++) {
        GridLine* line = linebuffer_get(&canvas->lines, i);
        for (uint32_t p = 0; p < line->prims.count; p++) {
            primCount++;
            totalWords += line->prims.data[p].wordCount;
        }
    }

    if (primCount == 0) {
        canvas->primStagingCount = 0;
        if (wordCount) *wordCount = 0;
        return NULL;
    }

    // Layout: [prim0_offset, prim1_offset, ...][prim0_data...][prim1_data...]
    // Each prim data starts with anchorRow (already in data)
    uint32_t totalSize = primCount + totalWords;
    ensure_prim_staging(canvas, totalSize);

    uint32_t dataOffset = 0;
    uint32_t primIdx = 0;
    for (uint32_t i = 0; i < canvas->lines.count; i++) {
        GridLine* line = linebuffer_get(&canvas->lines, i);

        for (uint32_t p = 0; p < line->prims.count; p++) {
            PrimData* prim = &line->prims.data[p];
            canvas->primStaging[primIdx] = dataOffset;

            // Copy primitive data (anchorRow is first word)
            for (uint32_t w = 0; w < prim->wordCount; w++) {
                uint32_t val;
                memcpy(&val, &prim->data[w], sizeof(uint32_t));
                canvas->primStaging[primCount + dataOffset + w] = val;
            }

            dataOffset += prim->wordCount;
            primIdx++;
        }
    }

    canvas->primStagingCount = totalSize;
    if (wordCount) *wordCount = totalSize;
    return canvas->primStaging;
}

uint32_t ypaint_canvas_prim_gpu_size(YPaintCanvasHandle canvas) {
    if (!canvas) return 0;

    uint32_t totalWords = 0;
    for (uint32_t i = 0; i < canvas->lines.count; i++) {
        GridLine* line = linebuffer_get(&canvas->lines, i);
        for (uint32_t p = 0; p < line->prims.count; p++) {
            totalWords += line->prims.data[p].wordCount;
        }
    }
    return totalWords * sizeof(float);
}

//=============================================================================
// State management
//=============================================================================

void ypaint_canvas_clear(YPaintCanvasHandle canvas) {
    if (!canvas) return;

    linebuffer_free(&canvas->lines);
    linebuffer_init(&canvas->lines);

    canvas->gridStagingCount = 0;
    canvas->primStagingCount = 0;
    canvas->cursorCol = 0;
    canvas->cursorRow = 0;
    canvas->row0Absolute = 0;
    canvas->dirty = true;
}

bool ypaint_canvas_empty(YPaintCanvasHandle canvas) {
    if (!canvas) return true;

    for (uint32_t i = 0; i < canvas->lines.count; i++) {
        GridLine* line = linebuffer_get(&canvas->lines, i);
        if (line->prims.count > 0) return false;
    }
    return true;
}

uint32_t ypaint_canvas_primitive_count(YPaintCanvasHandle canvas) {
    if (!canvas) return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < canvas->lines.count; i++) {
        GridLine* line = linebuffer_get(&canvas->lines, i);
        count += line->prims.count;
    }
    return count;
}
