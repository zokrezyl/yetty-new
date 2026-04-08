// yplot - Plot primitive for ypaint
// Pure C implementation

#include <yetty/ypaint/yplot/yplot.h>
#include <yetty/ypaint/yfsvm/yfsvm.gen.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

//=============================================================================
// Default color palette
//=============================================================================

static const uint32_t COLOR_PALETTE[8] = {
    0xFFFF6B6B,  // Red
    0xFF4ECDC4,  // Teal
    0xFFFFE66D,  // Yellow
    0xFF95E1D3,  // Mint
    0xFFF38181,  // Coral
    0xFFAA96DA,  // Purple
    0xFF72D6C9,  // Cyan
    0xFFFCBF49,  // Orange
};

uint32_t yplot_default_color(uint32_t index) {
    return COLOR_PALETTE[index % 8];
}

//=============================================================================
// Internal state
//=============================================================================

struct YPlot {
    // Bytecode
    uint32_t* bytecode;
    uint32_t bytecodeWordCount;
    uint32_t functionCount;

    // Per-function styling
    YPlotFunction functions[YPLOT_MAX_FUNCTIONS];

    // Configuration
    YPlotRange range;
    YPlotAxis axis;
    YPlotDisplay display;
};

//=============================================================================
// Config initialization
//=============================================================================

void yplot_init_range(YPlotRange* range) {
    range->xMin = YPLOT_DEFAULT_X_MIN;
    range->xMax = YPLOT_DEFAULT_X_MAX;
    range->yMin = YPLOT_DEFAULT_Y_MIN;
    range->yMax = YPLOT_DEFAULT_Y_MAX;
}

void yplot_init_axis(YPlotAxis* axis) {
    axis->showGrid = true;
    axis->showAxes = true;
    axis->showLabels = true;
    axis->axisColor = 0xFFAAAAAA;
    axis->gridColor = 0xFF444444;
    axis->labelColor = 0xFFFFFFFF;
    axis->labelFontSize = 10.0f;
    axis->gridDivisionsX = 10;
    axis->gridDivisionsY = 10;
}

void yplot_init_display(YPlotDisplay* display) {
    display->bgColor = 0xFF1A1A2E;
    display->zoom = 1.0f;
    display->centerX = 0.5f;
    display->centerY = 0.5f;
}

//=============================================================================
// Lifecycle
//=============================================================================

YPlotResult yplot_create(YPlotHandle* out) {
    if (!out) {
        return YPLOT_ERR("null output pointer");
    }

    struct YPlot* plot = (struct YPlot*)calloc(1, sizeof(struct YPlot));
    if (!plot) {
        return YPLOT_ERR("allocation failed");
    }

    // Initialize configs
    yplot_init_range(&plot->range);
    yplot_init_axis(&plot->axis);
    yplot_init_display(&plot->display);

    // Initialize function colors
    for (int i = 0; i < YPLOT_MAX_FUNCTIONS; i++) {
        plot->functions[i].color = yplot_default_color((uint32_t)i);
        plot->functions[i].visible = true;
    }

    *out = plot;
    return YPLOT_OK;
}

void yplot_destroy(YPlotHandle plot) {
    if (plot) {
        free(plot->bytecode);
        free(plot);
    }
}

//=============================================================================
// Bytecode management
//=============================================================================

YPlotResult yplot_set_bytecode(YPlotHandle plot, const uint32_t* bytecode, uint32_t wordCount) {
    if (!plot) {
        return YPLOT_ERR("null plot handle");
    }
    if (!bytecode || wordCount < 4) {
        return YPLOT_ERR("invalid bytecode");
    }

    // Validate magic
    if (bytecode[0] != YFSVM_MAGIC) {
        return YPLOT_ERR("invalid bytecode magic");
    }

    // Free existing
    free(plot->bytecode);

    // Copy bytecode
    plot->bytecode = (uint32_t*)malloc(wordCount * sizeof(uint32_t));
    if (!plot->bytecode) {
        plot->bytecodeWordCount = 0;
        plot->functionCount = 0;
        return YPLOT_ERR("allocation failed");
    }

    memcpy(plot->bytecode, bytecode, wordCount * sizeof(uint32_t));
    plot->bytecodeWordCount = wordCount;

    // Extract function count from header (word 2)
    plot->functionCount = bytecode[2];
    if (plot->functionCount > YPLOT_MAX_FUNCTIONS) {
        plot->functionCount = YPLOT_MAX_FUNCTIONS;
    }

    return YPLOT_OK;
}

const uint32_t* yplot_get_bytecode(YPlotHandle plot, uint32_t* outWordCount) {
    if (!plot) {
        if (outWordCount) *outWordCount = 0;
        return NULL;
    }
    if (outWordCount) {
        *outWordCount = plot->bytecodeWordCount;
    }
    return plot->bytecode;
}

uint32_t yplot_function_count(YPlotHandle plot) {
    return plot ? plot->functionCount : 0;
}

//=============================================================================
// Function styling
//=============================================================================

YPlotResult yplot_set_function_color(YPlotHandle plot, uint32_t index, uint32_t color) {
    if (!plot) {
        return YPLOT_ERR("null plot handle");
    }
    if (index >= YPLOT_MAX_FUNCTIONS) {
        return YPLOT_ERR("function index out of range");
    }
    plot->functions[index].color = color;
    return YPLOT_OK;
}

YPlotResult yplot_set_function_visible(YPlotHandle plot, uint32_t index, bool visible) {
    if (!plot) {
        return YPLOT_ERR("null plot handle");
    }
    if (index >= YPLOT_MAX_FUNCTIONS) {
        return YPLOT_ERR("function index out of range");
    }
    plot->functions[index].visible = visible;
    return YPLOT_OK;
}

uint32_t yplot_get_function_color(YPlotHandle plot, uint32_t index) {
    if (!plot || index >= YPLOT_MAX_FUNCTIONS) {
        return 0;
    }
    return plot->functions[index].color;
}

bool yplot_get_function_visible(YPlotHandle plot, uint32_t index) {
    if (!plot || index >= YPLOT_MAX_FUNCTIONS) {
        return false;
    }
    return plot->functions[index].visible;
}

//=============================================================================
// Configuration access
//=============================================================================

YPlotRange* yplot_range(YPlotHandle plot) {
    return plot ? &plot->range : NULL;
}

YPlotAxis* yplot_axis(YPlotHandle plot) {
    return plot ? &plot->axis : NULL;
}

YPlotDisplay* yplot_display(YPlotHandle plot) {
    return plot ? &plot->display : NULL;
}

void yplot_set_range(YPlotHandle plot, float xMin, float xMax, float yMin, float yMax) {
    if (plot) {
        plot->range.xMin = xMin;
        plot->range.xMax = xMax;
        plot->range.yMin = yMin;
        plot->range.yMax = yMax;
    }
}

//=============================================================================
// Serialization
//=============================================================================

// Plot primitive type ID (matches shader)
#define YPLOT_TYPE_ID 0x0009

// Serialization format:
// [type:32][zOrder:32][functionCount:32][reserved:32]
// [xMin:f32][xMax:f32][yMin:f32][yMax:f32]
// [color0:32][color1:32]...[color7:32]
// [visibilityMask:32][reserved:32][reserved:32][reserved:32]
// [bytecode...]

#define YPLOT_HEADER_WORDS 20

uint32_t yplot_serialize(YPlotHandle plot, uint32_t* buffer, uint32_t bufferCapacity) {
    if (!plot) {
        return 0;
    }

    uint32_t totalWords = YPLOT_HEADER_WORDS + plot->bytecodeWordCount;

    // Query mode - return required size
    if (!buffer) {
        return totalWords;
    }

    if (bufferCapacity < totalWords) {
        return 0;  // Buffer too small
    }

    uint32_t i = 0;

    // Header
    buffer[i++] = YPLOT_TYPE_ID;
    buffer[i++] = 0;  // zOrder
    buffer[i++] = plot->functionCount;
    buffer[i++] = 0;  // reserved

    // Range (as float bits)
    union { float f; uint32_t u; } conv;
    conv.f = plot->range.xMin; buffer[i++] = conv.u;
    conv.f = plot->range.xMax; buffer[i++] = conv.u;
    conv.f = plot->range.yMin; buffer[i++] = conv.u;
    conv.f = plot->range.yMax; buffer[i++] = conv.u;

    // Colors
    for (int j = 0; j < YPLOT_MAX_FUNCTIONS; j++) {
        buffer[i++] = plot->functions[j].color;
    }

    // Visibility mask (bit per function)
    uint32_t visMask = 0;
    for (int j = 0; j < YPLOT_MAX_FUNCTIONS; j++) {
        if (plot->functions[j].visible) {
            visMask |= (1u << j);
        }
    }
    buffer[i++] = visMask;
    buffer[i++] = 0;  // reserved
    buffer[i++] = 0;  // reserved
    buffer[i++] = 0;  // reserved

    // Bytecode
    if (plot->bytecode && plot->bytecodeWordCount > 0) {
        memcpy(&buffer[i], plot->bytecode, plot->bytecodeWordCount * sizeof(uint32_t));
    }

    return totalWords;
}

//=============================================================================
// Decoration serialization (axes, grid, labels)
// This would write to a YPaintBuffer - stub for now
//=============================================================================

YPlotResult yplot_serialize_decoration(YPlotHandle plot, void* ypaintBuffer,
                                        uint32_t widthCells, uint32_t heightCells) {
    if (!plot || !ypaintBuffer) {
        return YPLOT_ERR("null argument");
    }

    // TODO: Implement using ypaint buffer API
    // This requires ypaint-buffer.h to be available
    // For now, return success - decoration can be added later

    (void)widthCells;
    (void)heightCells;

    return YPLOT_OK;
}

//=============================================================================
// Color utilities
//=============================================================================

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

YPlotResult yplot_parse_color(const char* colorStr, uint32_t* outColor) {
    if (!colorStr || !outColor) {
        return YPLOT_ERR("null argument");
    }

    const char* p = colorStr;

    // Skip leading '#'
    if (*p == '#') {
        p++;
    }

    // Count hex digits
    size_t len = 0;
    const char* start = p;
    while (*p && hex_digit(*p) >= 0) {
        len++;
        p++;
    }

    if (len != 6 && len != 8) {
        return YPLOT_ERR("color must be 6 or 8 hex digits");
    }

    // Parse hex value
    uint32_t value = 0;
    p = start;
    for (size_t i = 0; i < len; i++) {
        int d = hex_digit(*p++);
        if (d < 0) {
            return YPLOT_ERR("invalid hex digit");
        }
        value = (value << 4) | (uint32_t)d;
    }

    // If 6 digits, add full alpha
    if (len == 6) {
        value = 0xFF000000 | value;
    }

    *outColor = value;
    return YPLOT_OK;
}
