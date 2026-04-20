#include <yetty/ypaint-core/complex-prim-types.h>
#include <string.h>

#define MAX_COMPLEX_PRIM_TYPES 32

static struct yetty_ypaint_complex_prim_type_info registry[MAX_COMPLEX_PRIM_TYPES];
static uint32_t registry_count = 0;

bool yetty_ypaint_is_complex_type(uint32_t type)
{
	/* Types >= 0x80000000 are complex prims (FAM format) */
	return (type >= YETTY_YPAINT_COMPLEX_TYPE_BASE);
}

size_t yetty_ypaint_complex_prim_total_size(const void *data)
{
	const struct yetty_ypaint_complex_prim *prim = data;
	return sizeof(struct yetty_ypaint_complex_prim) + prim->payload_size;
}

void yetty_ypaint_complex_prim_register(
	const struct yetty_ypaint_complex_prim_type_info *info)
{
	if (!info || registry_count >= MAX_COMPLEX_PRIM_TYPES)
		return;

	registry[registry_count++] = *info;
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

const struct yetty_ypaint_complex_prim_parse_ops *
yetty_ypaint_complex_prim_get_parse_ops(uint32_t type_id)
{
	const struct yetty_ypaint_complex_prim_type_info *info =
		find_type_info(type_id);
	return info ? info->parse_ops : NULL;
}

const struct yetty_ypaint_complex_prim_runtime_ops *
yetty_ypaint_complex_prim_get_runtime_ops(uint32_t type_id)
{
	const struct yetty_ypaint_complex_prim_type_info *info =
		find_type_info(type_id);
	return info ? info->runtime_ops : NULL;
}

