// YPaint Complex Primitive Types - lightweight dispatch for buffer-stored prims
//
// Complex primitives stored as FAM in the unified primitive buffer:
//   struct { uint32_t type; uint32_t payload_size; uint8_t data[]; }
//
// All primitives (SDF, fonts, text spans, plots, images) share ONE buffer.
// SDF prims have fixed size per type (via handlers).
// Complex prims have variable size (read from payload_size field).
//
// Ops are looked up by type ID, not stored in the primitive itself.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <yetty/ycore/types.h>
#include <yetty/yrender/gpu-resource-set.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Complex primitive type IDs (256+ to avoid collision with SDF types 0-255)
//=============================================================================

#define YETTY_YPAINT_COMPLEX_TYPE_BASE   0x80000000u
#define YETTY_YPAINT_TYPE_FONT           0x80000001u
#define YETTY_YPAINT_TYPE_TEXT_SPAN      0x80000002u
#define YETTY_YPAINT_TYPE_YPLOT          0x80000003u

//=============================================================================
// Complex primitive header (FAM)
//=============================================================================

struct yetty_ypaint_complex_prim {
	uint32_t type;
	uint32_t payload_size;
	uint8_t data[];
};

// Check if type uses FAM format (returns size from payload_size field)
bool yetty_ypaint_is_complex_type(uint32_t type);

// Get total size of complex prim from data pointer (reads FAM header)
size_t yetty_ypaint_complex_prim_total_size(const void *data);

//=============================================================================
// Text span wire format
// Layout: x(f32), y(f32), font_size(f32), color(u32), layer(u32), font_id(i32),
//         rotation(f32), text_len(u32), text_data[]
//=============================================================================

#define YETTY_YPAINT_TEXT_SPAN_X_OFF        0
#define YETTY_YPAINT_TEXT_SPAN_Y_OFF        4
#define YETTY_YPAINT_TEXT_SPAN_FONT_SIZE_OFF 8
#define YETTY_YPAINT_TEXT_SPAN_COLOR_OFF    12
#define YETTY_YPAINT_TEXT_SPAN_LAYER_OFF    16
#define YETTY_YPAINT_TEXT_SPAN_FONT_ID_OFF  20
#define YETTY_YPAINT_TEXT_SPAN_ROTATION_OFF 24
#define YETTY_YPAINT_TEXT_SPAN_TEXT_LEN_OFF 28
#define YETTY_YPAINT_TEXT_SPAN_TEXT_OFF     32
#define YETTY_YPAINT_TEXT_SPAN_HEADER_SIZE  32

//=============================================================================
// Font wire format
// Layout: font_id(i32), name_len(u32), ttf_len(u32), name[], ttf_data[]
//=============================================================================

#define YETTY_YPAINT_FONT_ID_OFF       0
#define YETTY_YPAINT_FONT_NAME_LEN_OFF 4
#define YETTY_YPAINT_FONT_TTF_LEN_OFF  8
#define YETTY_YPAINT_FONT_DATA_OFF     12
#define YETTY_YPAINT_FONT_HEADER_SIZE  12

//=============================================================================
// Parse-time ops (stateless, reads raw data)
//=============================================================================

struct yetty_ypaint_complex_prim_parse_ops {
	struct rectangle_result (*get_aabb)(const uint8_t *data,
					    uint32_t payload_size);
};

//=============================================================================
// Runtime ops (needs cache for GPU resources)
//=============================================================================

struct yetty_ypaint_complex_prim_runtime_ops {
	struct yetty_yrender_gpu_resource_set_result (*get_gpu_resource_set)(
		const uint8_t *data, uint32_t payload_size, void **cache);

	void (*destroy_cache)(void *cache);
};

//=============================================================================
// Type registry
//=============================================================================

struct yetty_ypaint_complex_prim_type_info {
	uint32_t type_id;
	const char *name;
	const struct yetty_ypaint_complex_prim_parse_ops *parse_ops;
	const struct yetty_ypaint_complex_prim_runtime_ops *runtime_ops;
};

// Register a complex primitive type (call at init)
void yetty_ypaint_complex_prim_register(
	const struct yetty_ypaint_complex_prim_type_info *info);

// Lookup ops by type ID (returns NULL if not registered)
const struct yetty_ypaint_complex_prim_parse_ops *
yetty_ypaint_complex_prim_get_parse_ops(uint32_t type_id);

const struct yetty_ypaint_complex_prim_runtime_ops *
yetty_ypaint_complex_prim_get_runtime_ops(uint32_t type_id);

#ifdef __cplusplus
}
#endif
