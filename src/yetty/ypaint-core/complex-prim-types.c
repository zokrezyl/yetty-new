// YPaint Complex Primitive Types - factory implementation

#include <yetty/ypaint-core/complex-prim-types.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COMPLEX_PRIM_TYPES 32

//=============================================================================
// Factory internal structure
//=============================================================================

struct yetty_ypaint_complex_prim_factory {
	struct yetty_ypaint_complex_prim_shared_state states[MAX_COMPLEX_PRIM_TYPES];
	uint32_t count;
};

//=============================================================================
// Global registry (for legacy API and default factory)
//=============================================================================

static struct yetty_ypaint_complex_prim_type_info g_registry[MAX_COMPLEX_PRIM_TYPES];
static uint32_t g_registry_count = 0;

//=============================================================================
// Helper functions
//=============================================================================

bool yetty_ypaint_is_complex_type(uint32_t type)
{
	return (type >= YETTY_YPAINT_COMPLEX_TYPE_BASE);
}

size_t yetty_ypaint_complex_prim_total_size(const void *data)
{
	const struct yetty_ypaint_complex_prim *prim = data;
	return sizeof(struct yetty_ypaint_complex_prim) + prim->payload_size;
}

size_t yetty_ypaint_complex_prim_size(const void *data)
{
	return yetty_ypaint_complex_prim_total_size(data);
}

//=============================================================================
// Factory lifecycle
//=============================================================================

struct yetty_ypaint_complex_prim_factory_ptr_result
yetty_ypaint_complex_prim_factory_create(void)
{
	struct yetty_ypaint_complex_prim_factory *factory =
		calloc(1, sizeof(struct yetty_ypaint_complex_prim_factory));
	if (!factory)
		return YETTY_ERR(yetty_ypaint_complex_prim_factory_ptr,
			"allocation failed");
	return YETTY_OK(yetty_ypaint_complex_prim_factory_ptr, factory);
}

void yetty_ypaint_complex_prim_factory_destroy(
	struct yetty_ypaint_complex_prim_factory *factory)
{
	if (!factory)
		return;
	/* Shared states' gpu_rs are static, no need to free */
	free(factory);
}

//=============================================================================
// Factory registration
//=============================================================================

struct yetty_core_void_result yetty_ypaint_complex_prim_factory_register(
	struct yetty_ypaint_complex_prim_factory *factory,
	const struct yetty_ypaint_complex_prim_type_info *info)
{
	if (!factory)
		return YETTY_ERR(yetty_core_void, "factory is NULL");
	if (!info)
		return YETTY_ERR(yetty_core_void, "info is NULL");
	if (!info->ops)
		return YETTY_ERR(yetty_core_void, "ops is NULL");
	if (factory->count >= MAX_COMPLEX_PRIM_TYPES)
		return YETTY_ERR(yetty_core_void, "max types reached");

	/* Check for duplicate */
	for (uint32_t i = 0; i < factory->count; i++) {
		if (factory->states[i].type_id == info->type_id)
			return YETTY_ERR(yetty_core_void, "type already registered");
	}

	struct yetty_ypaint_complex_prim_shared_state *state =
		&factory->states[factory->count];

	state->type_id = info->type_id;
	state->ops = info->ops;
	state->gpu_rs = info->create_shared_gpu_rs ?
		info->create_shared_gpu_rs() : NULL;
	state->type_data = NULL;

	factory->count++;
	return YETTY_OK_VOID();
}

//=============================================================================
// Factory lookup
//=============================================================================

struct yetty_ypaint_complex_prim_shared_state *
yetty_ypaint_complex_prim_factory_get_shared(
	struct yetty_ypaint_complex_prim_factory *factory,
	uint32_t type_id)
{
	if (!factory)
		return NULL;

	for (uint32_t i = 0; i < factory->count; i++) {
		if (factory->states[i].type_id == type_id)
			return &factory->states[i];
	}
	return NULL;
}

//=============================================================================
// Instance lifecycle
//=============================================================================

struct yetty_ypaint_complex_prim_instance_ptr_result
yetty_ypaint_complex_prim_factory_create_instance(
	struct yetty_ypaint_complex_prim_factory *factory,
	const void *buffer_data, size_t size,
	uint32_t rolling_row)
{
	if (!factory)
		return YETTY_ERR(yetty_ypaint_complex_prim_instance_ptr,
			"factory is NULL");
	if (!buffer_data || size < sizeof(struct yetty_ypaint_complex_prim))
		return YETTY_ERR(yetty_ypaint_complex_prim_instance_ptr,
			"invalid buffer data");

	const struct yetty_ypaint_complex_prim *prim = buffer_data;
	uint32_t type_id = prim->type;

	struct yetty_ypaint_complex_prim_shared_state *shared =
		yetty_ypaint_complex_prim_factory_get_shared(factory, type_id);
	if (!shared)
		return YETTY_ERR(yetty_ypaint_complex_prim_instance_ptr,
			"type not registered");

	struct yetty_ypaint_complex_prim_instance *instance =
		calloc(1, sizeof(struct yetty_ypaint_complex_prim_instance));
	if (!instance)
		return YETTY_ERR(yetty_ypaint_complex_prim_instance_ptr,
			"allocation failed");

	instance->buffer_data = malloc(size);
	if (!instance->buffer_data) {
		free(instance);
		return YETTY_ERR(yetty_ypaint_complex_prim_instance_ptr,
			"buffer allocation failed");
	}

	memcpy(instance->buffer_data, buffer_data, size);
	instance->buffer_size = size;
	instance->type = type_id;
	instance->shared = shared;
	instance->dirty = true;
	instance->texture = NULL;
	instance->instance_data = NULL;
	instance->rolling_row = rolling_row;

	/* Compute bounds using generic aabb function (reads standard offset) */
	struct rectangle_result aabb_res =
		yetty_ypaint_complex_prim_aabb(buffer_data);
	if (YETTY_IS_OK(aabb_res))
		instance->bounds = aabb_res.value;

	return YETTY_OK(yetty_ypaint_complex_prim_instance_ptr, instance);
}

void yetty_ypaint_complex_prim_instance_destroy(
	struct yetty_ypaint_complex_prim_instance *instance)
{
	if (!instance)
		return;

	/* Call type-specific destroy if provided */
	if (instance->shared && instance->shared->ops &&
	    instance->shared->ops->destroy_instance)
		instance->shared->ops->destroy_instance(instance);

	free(instance->buffer_data);
	free(instance);
}

//=============================================================================
// Generic AABB - reads bounds from standard offset 0-15 in payload
// Wire format: [bounds_x:f32][bounds_y:f32][bounds_w:f32][bounds_h:f32][...]
//=============================================================================

#define COMPLEX_PRIM_BOUNDS_SIZE 16  /* 4 floats */

struct rectangle_result yetty_ypaint_complex_prim_aabb(const void *data)
{
	if (!data)
		return YETTY_ERR(rectangle, "NULL data");

	const struct yetty_ypaint_complex_prim *prim = data;
	if (prim->payload_size < COMPLEX_PRIM_BOUNDS_SIZE)
		return YETTY_ERR(rectangle, "payload too small for bounds");

	const uint8_t *payload = prim->data;
	float x, y, w, h;
	memcpy(&x, payload + 0, sizeof(float));
	memcpy(&y, payload + 4, sizeof(float));
	memcpy(&w, payload + 8, sizeof(float));
	memcpy(&h, payload + 12, sizeof(float));

	struct rectangle rect = {
		.min = { .x = x, .y = y },
		.max = { .x = x + w, .y = y + h }
	};
	return YETTY_OK(rectangle, rect);
}

//=============================================================================
// Legacy global registry API (for backward compatibility)
//=============================================================================

static const struct yetty_ypaint_complex_prim_type_info *
find_type_info(uint32_t type_id)
{
	for (uint32_t i = 0; i < g_registry_count; i++) {
		if (g_registry[i].type_id == type_id)
			return &g_registry[i];
	}
	return NULL;
}

struct yetty_core_void_result yetty_ypaint_complex_prim_register(
	const struct yetty_ypaint_complex_prim_type_info *info)
{
	if (!info)
		return YETTY_ERR(yetty_core_void, "info is NULL");
	if (g_registry_count >= MAX_COMPLEX_PRIM_TYPES)
		return YETTY_ERR(yetty_core_void, "max types reached");

	g_registry[g_registry_count++] = *info;
	return YETTY_OK_VOID();
}

struct yetty_ypaint_complex_prim_ops_ptr_result
yetty_ypaint_complex_prim_get_ops(uint32_t type_id)
{
	const struct yetty_ypaint_complex_prim_type_info *info =
		find_type_info(type_id);
	if (!info)
		return YETTY_ERR(yetty_ypaint_complex_prim_ops_ptr,
			"type not registered");
	if (!info->ops)
		return YETTY_ERR(yetty_ypaint_complex_prim_ops_ptr,
			"ops is NULL");
	return YETTY_OK(yetty_ypaint_complex_prim_ops_ptr, info->ops);
}
