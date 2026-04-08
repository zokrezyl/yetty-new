// yplot - Plot primitive for ypaint-c
// Builds to yplot.so plugin

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

typedef struct YPlotFunctionConfig {
    char name[64];
    char expression[256];
    uint32_t color;       // ARGB
    bool visible;
} YPlotFunctionConfig;

typedef struct YPlotRangeConfig {
    float xMin;
    float xMax;
    float yMin;
    float yMax;
    bool autoRange;
} YPlotRangeConfig;

typedef struct YPlotAxisConfig {
    bool showGrid;
    bool showAxes;
    bool showLabels;
    uint32_t axisColor;
    uint32_t gridColor;
    uint32_t labelColor;
    float labelFontSize;
    int gridDivisionsX;
    int gridDivisionsY;
} YPlotAxisConfig;

typedef struct YPlotDisplayConfig {
    uint32_t bgColor;
    float zoom;
    float centerX;
    float centerY;
} YPlotDisplayConfig;

//=============================================================================
// YPlotState - opaque handle
//=============================================================================

typedef struct YPlotState* YPlotStateHandle;

//=============================================================================
// Result type
//=============================================================================

typedef struct YPlotResult {
    int error;           // 0 = success
    const char* message; // error message if error != 0
} YPlotResult;

//=============================================================================
// API
//=============================================================================

// Create/destroy
YPlotResult yplot_create(YPlotStateHandle* out);
void yplot_destroy(YPlotStateHandle state);

// Function management
YPlotResult yplot_add_function(YPlotStateHandle state, const char* name,
                                const char* expression, uint32_t color);
YPlotResult yplot_set_function(YPlotStateHandle state, uint32_t index,
                                const char* expression);
YPlotResult yplot_set_function_color(YPlotStateHandle state, uint32_t index,
                                      uint32_t color);
YPlotResult yplot_remove_function(YPlotStateHandle state, uint32_t index);
void yplot_clear_functions(YPlotStateHandle state);
uint32_t yplot_function_count(YPlotStateHandle state);

// Parse multi-expression string: "f=sin(x); g=cos(x)"
YPlotResult yplot_parse_expressions(YPlotStateHandle state, const char* source);

// Compile all functions to yfsvm bytecode
// Returns bytecode in out_bytecode (caller must free with yplot_free_bytecode)
YPlotResult yplot_compile(YPlotStateHandle state, uint32_t** out_bytecode,
                           uint32_t* out_size);
void yplot_free_bytecode(uint32_t* bytecode);

// Configuration access
YPlotRangeConfig* yplot_range(YPlotStateHandle state);
YPlotAxisConfig* yplot_axis(YPlotStateHandle state);
YPlotDisplayConfig* yplot_display(YPlotStateHandle state);

void yplot_set_range(YPlotStateHandle state, float xMin, float xMax,
                      float yMin, float yMax);

// Build decoration (axes, grid, labels) into ypaint buffer
// buffer must be a valid YPaintBufferHandle
YPlotResult yplot_build_decoration(YPlotStateHandle state, void* buffer,
                                    uint32_t widthCells, uint32_t heightCells);

// Color utilities
YPlotResult yplot_parse_color(const char* colorStr, uint32_t* out_color);
uint32_t yplot_default_color(uint32_t index);

//=============================================================================
// Plugin registration (called by runtime when .so is loaded)
//=============================================================================

// Returns shader code as null-terminated string
const char* yplot_get_shader(void);

// Returns primitive type ID
uint32_t yplot_get_type_id(void);

// Plugin info
const char* yplot_get_name(void);
const char* yplot_get_version(void);

#ifdef __cplusplus
}
#endif
