// YPaint Flyweight - creates configured flyweight registry for ALL primitives
//
// Two handlers:
// 1. SDF handler (default) - for types 0-255
// 2. Complex prim handler - for types >= 0x80000000
//
// Both return base ops (size, aabb) for buffer iteration.

#include <yetty/ypaint/flyweight.h>
#include <yetty/ysdf/handler.h>
#include <yetty/ypaint-core/complex-prim-types.h>
#include <yetty/ytrace.h>

struct yetty_ypaint_flyweight_registry_ptr_result
yetty_ypaint_flyweight_create(void)
{
    struct yetty_ypaint_flyweight_registry_ptr_result res =
        yetty_ypaint_flyweight_registry_create();
    if (YETTY_IS_ERR(res))
        return res;

    struct yetty_ypaint_flyweight_registry *reg = res.value;

    // Default handler for SDF primitives (fast path, types 0-255)
    yetty_ypaint_flyweight_registry_set_default(reg, yetty_ysdf_handler);
    ydebug("flyweight_create: registered SDF default handler");

    // Complex prim handler (types >= 0x80000000)
    yetty_ypaint_flyweight_registry_add(reg,
        YETTY_YPAINT_COMPLEX_TYPE_BASE, 0xFFFFFFFF,
        yetty_ypaint_complex_prim_handler);
    ydebug("flyweight_create: registered complex prim handler");

    return res;
}
