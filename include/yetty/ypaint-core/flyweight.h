// YPaint Flyweight - primitive handler registry (instance-based)
// Decoupled from buffer, usable by buffer and canvas
#pragma once

#include <stdint.h>
#include <yetty/ycore/result.h>
#include <yetty/ycore/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declare gpu_resource_set result (defined in yrender/gpu-resource-set.h)
struct yetty_render_gpu_resource_set;
struct yetty_render_gpu_resource_set_result;

// Primitive ops vtable - all functions return result types
struct yetty_ypaint_prim_ops {
    // Size in bytes (for buffer iteration)
    struct yetty_core_size_result (*size)(const uint32_t *prim);

    // Bounding box (for spatial grid)
    struct rectangle_result (*aabb)(const uint32_t *prim);

    // Cleanup cached data (optional, may be NULL for simple prims)
    void (*destroy)(void *cache);

    // Get GPU resources for rendering (optional, NULL for simple SDF prims)
    // cache_ptr: pointer to cache storage (caller provides, callee allocates)
    struct yetty_render_gpu_resource_set_result (*get_gpu_resource_set)(
        const uint32_t *prim, void **cache_ptr);
};

// Flyweight - wraps pointer to primitive data + ops
struct yetty_ypaint_prim_flyweight {
    const uint32_t *data;  // type at data[0]
    const struct yetty_ypaint_prim_ops *ops;
};

// Handler function - takes primitive pointer, returns flyweight (ops=NULL if not handled)
typedef struct yetty_ypaint_prim_flyweight (*yetty_ypaint_prim_handler_fn)(
    const uint32_t *prim);

// Flyweight registry instance (opaque)
struct yetty_ypaint_flyweight_registry;

YETTY_RESULT_DECLARE(yetty_ypaint_flyweight_registry_ptr,
    struct yetty_ypaint_flyweight_registry *);

// Create/destroy registry instance
struct yetty_ypaint_flyweight_registry_ptr_result
yetty_ypaint_flyweight_registry_create(void);

void yetty_ypaint_flyweight_registry_destroy(
    struct yetty_ypaint_flyweight_registry *reg);

// Set default handler (SDF) - called first, fast path
void yetty_ypaint_flyweight_registry_set_default(
    struct yetty_ypaint_flyweight_registry *reg,
    yetty_ypaint_prim_handler_fn handler);

// Register additional handler for type range [type_min, type_max]
struct yetty_core_void_result yetty_ypaint_flyweight_registry_add(
    struct yetty_ypaint_flyweight_registry *reg,
    uint32_t type_min,
    uint32_t type_max,
    yetty_ypaint_prim_handler_fn handler);

// Get flyweight for primitive (tries default first, then by type range)
struct yetty_ypaint_prim_flyweight yetty_ypaint_flyweight_registry_get(
    const struct yetty_ypaint_flyweight_registry *reg,
    const uint32_t *prim);

#ifdef __cplusplus
}
#endif
