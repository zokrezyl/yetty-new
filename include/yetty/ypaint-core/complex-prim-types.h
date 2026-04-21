// YPaint Complex Primitive Types - factory pattern for complex primitives
//
// Complex primitives stored as FAM in the unified primitive buffer:
//   struct { uint32_t type; uint32_t payload_size; uint8_t data[]; }
//
// All primitives (SDF, fonts, text spans, plots, images) share ONE buffer.
// SDF prims have fixed size per type (via ysdf handlers).
// Complex prims have variable size (read from payload_size field).
//
// Factory pattern:
// - Base ops (size, aabb) work for all primitives
// - Complex ops extend base with render op
// - Factory manages shared state (compiled pipeline) per type
// - Instances hold: shared state pointer, texture, dirty flag, buffer data copy

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <yetty/ycore/result.h>
#include <yetty/ycore/types.h>
#include <yetty/yrender/gpu-resource-set.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Complex primitive type IDs (0x80000000+ to avoid collision with SDF 0-255)
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
// Complex prim ops - only type-specific operations
//
// size() and aabb() are NOT in ops - they are generic functions that work
// for all complex prims because:
//   - size: reads from FAM header (type + payload_size)
//   - aabb: reads bounds from standard offset 0-15 in payload
//
// The code generator ensures this layout. See docs/ypaint.md.
//=============================================================================

struct yetty_render_context;
struct yetty_ypaint_complex_prim_instance;

struct yetty_ypaint_complex_prim_ops {
	// Get GPU resource set for this primitive type (shader, pipeline)
	struct yetty_render_gpu_resource_set_result (*get_gpu_resource_set)(
		struct yetty_ypaint_complex_prim_instance *instance);

	// Render instance to texture
	struct yetty_core_void_result (*render)(
		struct yetty_ypaint_complex_prim_instance *instance,
		struct yetty_render_context *ctx);

	// Destroy instance-specific data (optional, may be NULL)
	void (*destroy_instance)(struct yetty_ypaint_complex_prim_instance *instance);
};

//=============================================================================
// Shared state - per type, owned by factory (compiled pipeline, shaders)
//=============================================================================

struct yetty_ypaint_complex_prim_shared_state {
	uint32_t type_id;
	const struct yetty_ypaint_complex_prim_ops *ops;
	struct yetty_render_gpu_resource_set *gpu_rs;  // compiled pipeline (static)
	void *type_data;  // type-specific shared data
};

//=============================================================================
// Instance - per primitive occurrence
//=============================================================================

struct yetty_ypaint_complex_prim_instance {
	uint32_t type;
	struct yetty_ypaint_complex_prim_shared_state *shared;
	void *texture;              // rendered texture (cached)
	bool dirty;                 // re-render when true
	uint8_t *buffer_data;       // copy of primitive data from ypaint buffer
	size_t buffer_size;
	struct rectangle bounds;
	uint32_t rolling_row;       // rolling row at insertion (for scrolling)
	void *instance_data;        // type-specific instance data
};

YETTY_RESULT_DECLARE(yetty_ypaint_complex_prim_instance_ptr,
	struct yetty_ypaint_complex_prim_instance *);

//=============================================================================
// Type info - for registration
//=============================================================================

struct yetty_ypaint_complex_prim_type_info {
	uint32_t type_id;
	const char *name;
	const struct yetty_ypaint_complex_prim_ops *ops;
	// Called once at registration to create shared state (shader, pipeline)
	struct yetty_render_gpu_resource_set *(*create_shared_gpu_rs)(void);
};

//=============================================================================
// Factory - manages shared state per type, creates instances
//=============================================================================

struct yetty_ypaint_complex_prim_factory;

YETTY_RESULT_DECLARE(yetty_ypaint_complex_prim_factory_ptr,
	struct yetty_ypaint_complex_prim_factory *);

// Create/destroy factory
struct yetty_ypaint_complex_prim_factory_ptr_result
yetty_ypaint_complex_prim_factory_create(void);

void yetty_ypaint_complex_prim_factory_destroy(
	struct yetty_ypaint_complex_prim_factory *factory);

// Register a complex primitive type (call at init)
struct yetty_core_void_result yetty_ypaint_complex_prim_factory_register(
	struct yetty_ypaint_complex_prim_factory *factory,
	const struct yetty_ypaint_complex_prim_type_info *info);

// Get shared state for a type (returns NULL if not registered)
struct yetty_ypaint_complex_prim_shared_state *
yetty_ypaint_complex_prim_factory_get_shared(
	struct yetty_ypaint_complex_prim_factory *factory,
	uint32_t type_id);

// Create instance from buffer data (copies data, marks dirty)
struct yetty_ypaint_complex_prim_instance_ptr_result
yetty_ypaint_complex_prim_factory_create_instance(
	struct yetty_ypaint_complex_prim_factory *factory,
	const void *buffer_data, size_t size,
	uint32_t rolling_row);

// Destroy instance
void yetty_ypaint_complex_prim_instance_destroy(
	struct yetty_ypaint_complex_prim_instance *instance);

//=============================================================================
// Generic functions for all complex primitives
// These work for ALL complex prims - no per-type ops needed
//=============================================================================

// Get size of complex prim (reads from FAM header)
// Same as yetty_ypaint_complex_prim_total_size()
size_t yetty_ypaint_complex_prim_size(const void *data);

// Get AABB of complex prim (reads bounds from standard offset 0-15 in payload)
// Wire format: [bounds_x:f32][bounds_y:f32][bounds_w:f32][bounds_h:f32][...]
// Code generator ensures all complex prims have bounds at this offset.
struct rectangle_result yetty_ypaint_complex_prim_aabb(const void *data);

//=============================================================================
// Legacy registry API (for backward compatibility during migration)
//=============================================================================

// Result types for ops lookup (deprecated - use factory)
YETTY_RESULT_DECLARE(yetty_ypaint_complex_prim_ops_ptr,
	const struct yetty_ypaint_complex_prim_ops *);

// Register type to global registry (deprecated - use factory)
struct yetty_core_void_result yetty_ypaint_complex_prim_register(
	const struct yetty_ypaint_complex_prim_type_info *info);

// Lookup ops by type ID (deprecated - use factory)
struct yetty_ypaint_complex_prim_ops_ptr_result
yetty_ypaint_complex_prim_get_ops(uint32_t type_id);

#ifdef __cplusplus
}
#endif
