#include <yetty/ypaint-core/complex-prim-types.h>
#include <string.h>

#define MAX_COMPLEX_PRIM_TYPES 32

static struct yetty_ypaint_complex_prim_type_info registry[MAX_COMPLEX_PRIM_TYPES];
static uint32_t registry_count = 0;

bool yetty_ypaint_is_complex_type(uint32_t type)
{
	return (type >= YETTY_YPAINT_COMPLEX_TYPE_BASE);
}

size_t yetty_ypaint_complex_prim_total_size(const void *data)
{
	const struct yetty_ypaint_complex_prim *prim = data;
	return sizeof(struct yetty_ypaint_complex_prim) + prim->payload_size;
}

struct yetty_core_void_result yetty_ypaint_complex_prim_register(
	const struct yetty_ypaint_complex_prim_type_info *info)
{
	if (!info)
		return YETTY_ERR(yetty_core_void, "info is NULL");
	if (registry_count >= MAX_COMPLEX_PRIM_TYPES)
		return YETTY_ERR(yetty_core_void, "max types reached");

	registry[registry_count++] = *info;
	return YETTY_OK_VOID();
}

static const struct yetty_ypaint_complex_prim_type_info *
find_type_info(uint32_t type_id)
{
	for (uint32_t i = 0; i < registry_count; i++) {
		if (registry[i].type_id == type_id)
			return &registry[i];
	}
	return NULL;
}

struct yetty_ypaint_complex_prim_parse_ops_ptr_result
yetty_ypaint_complex_prim_get_parse_ops(uint32_t type_id)
{
	const struct yetty_ypaint_complex_prim_type_info *info =
		find_type_info(type_id);
	if (!info)
		return YETTY_ERR(yetty_ypaint_complex_prim_parse_ops_ptr,
			"type not registered");
	if (!info->parse_ops)
		return YETTY_ERR(yetty_ypaint_complex_prim_parse_ops_ptr,
			"parse_ops is NULL");
	return YETTY_OK(yetty_ypaint_complex_prim_parse_ops_ptr, info->parse_ops);
}

struct yetty_ypaint_complex_prim_runtime_ops_ptr_result
yetty_ypaint_complex_prim_get_runtime_ops(uint32_t type_id)
{
	const struct yetty_ypaint_complex_prim_type_info *info =
		find_type_info(type_id);
	if (!info)
		return YETTY_ERR(yetty_ypaint_complex_prim_runtime_ops_ptr,
			"type not registered");
	if (!info->runtime_ops)
		return YETTY_ERR(yetty_ypaint_complex_prim_runtime_ops_ptr,
			"runtime_ops is NULL");
	return YETTY_OK(yetty_ypaint_complex_prim_runtime_ops_ptr, info->runtime_ops);
}

