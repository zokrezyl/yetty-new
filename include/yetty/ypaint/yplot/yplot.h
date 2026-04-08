// yplot - Plot primitive for ypaint
// Pure C API - accepts pre-compiled yfsvm bytecode

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Constants
//=============================================================================

#define YPLOT_MAX_FUNCTIONS 8
#define YPLOT_DEFAULT_X_MIN -3.14159f
#define YPLOT_DEFAULT_X_MAX  3.14159f
#define YPLOT_DEFAULT_Y_MIN -1.5f
#define YPLOT_DEFAULT_Y_MAX  1.5f

//=============================================================================
// Configuration structs
//=============================================================================

typedef struct YPlotFunction {
    uint32_t color;       // ARGB
    bool visible;
} YPlotFunction;

typedef struct YPlotRange {
    float xMin;
    float xMax;
    float yMin;
    float yMax;
} YPlotRange;

typedef struct YPlotAxis {
    bool showGrid;
    bool showAxes;
    bool showLabels;
    uint32_t axisColor;
    uint32_t gridColor;
    uint32_t labelColor;
    float labelFontSize;
    int gridDivisionsX;
    int gridDivisionsY;
} YPlotAxis;

typedef struct YPlotDisplay {
    uint32_t bgColor;
    float zoom;
    float centerX;
    float centerY;
} YPlotDisplay;

//=============================================================================
// YPlot - opaque handle
//=============================================================================

typedef struct YPlot* YPlotHandle;

//=============================================================================
// Result type
//=============================================================================

typedef struct YPlotResult {
    int error;           // 0 = success
    const char* message; // error message if error != 0
} YPlotResult;

#define YPLOT_OK ((YPlotResult){0, NULL})
#define YPLOT_ERR(msg) ((YPlotResult){1, (msg)})

//=============================================================================
// Lifecycle
//=============================================================================

YPlotResult yplot_create(YPlotHandle* out);
void yplot_destroy(YPlotHandle plot);

//=============================================================================
// Bytecode management
//=============================================================================

// Set pre-compiled yfsvm bytecode (copies the data)
// The bytecode contains function count in its header
YPlotResult yplot_set_bytecode(YPlotHandle plot, const uint32_t* bytecode, uint32_t wordCount);

// Get bytecode pointer and size (for serialization to ypaint buffer)
const uint32_t* yplot_get_bytecode(YPlotHandle plot, uint32_t* outWordCount);

// Get function count from loaded bytecode
uint32_t yplot_function_count(YPlotHandle plot);

//=============================================================================
// Function styling (colors per function index)
//=============================================================================

YPlotResult yplot_set_function_color(YPlotHandle plot, uint32_t index, uint32_t color);
YPlotResult yplot_set_function_visible(YPlotHandle plot, uint32_t index, bool visible);
uint32_t yplot_get_function_color(YPlotHandle plot, uint32_t index);
bool yplot_get_function_visible(YPlotHandle plot, uint32_t index);

//=============================================================================
// Configuration
//=============================================================================

YPlotRange* yplot_range(YPlotHandle plot);
YPlotAxis* yplot_axis(YPlotHandle plot);
YPlotDisplay* yplot_display(YPlotHandle plot);

void yplot_set_range(YPlotHandle plot, float xMin, float xMax, float yMin, float yMax);

// Initialize configs with defaults
void yplot_init_range(YPlotRange* range);
void yplot_init_axis(YPlotAxis* axis);
void yplot_init_display(YPlotDisplay* display);

//=============================================================================
// Serialization to ypaint buffer
//=============================================================================

// Serialize plot to buffer for GPU rendering
// Returns required word count if buffer is NULL
// Otherwise writes to buffer and returns words written
uint32_t yplot_serialize(YPlotHandle plot, uint32_t* buffer, uint32_t bufferCapacity);

// Serialize decoration (axes, grid, labels) to ypaint buffer
// buffer: YPaintBufferHandle from ypaint-buffer.h
YPlotResult yplot_serialize_decoration(YPlotHandle plot, void* ypaintBuffer,
                                        uint32_t widthCells, uint32_t heightCells);

//=============================================================================
// Color utilities
//=============================================================================

// Parse color from hex string: "#FF0000" or "FF0000"
YPlotResult yplot_parse_color(const char* colorStr, uint32_t* outColor);

// Default color palette for functions
uint32_t yplot_default_color(uint32_t index);

#ifdef __cplusplus
}
#endif
