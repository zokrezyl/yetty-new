// YPaint Flyweight - creates configured flyweight registry with all handlers

#include <yetty/ypaint/flyweight.h>
#include <yetty/ysdf/handler.h>
#include <yetty/ypaint-core/complex-prim-types.h>

#if YETTY_ENABLE_FEATURE_YPLOT
#include <yetty/yplot/yplot.h>
#endif

struct yetty_ypaint_flyweight_registry_ptr_result
yetty_ypaint_flyweight_create(void)
{
    struct yetty_ypaint_flyweight_registry_ptr_result res =
        yetty_ypaint_flyweight_registry_create();
    if (YETTY_IS_ERR(res))
        return res;

    struct yetty_ypaint_flyweight_registry *reg = res.value;

    // Default handler for SDF primitives (fast path)
    yetty_ypaint_flyweight_registry_set_default(reg, yetty_ysdf_handler);

#if YETTY_ENABLE_FEATURE_YPLOT
    // yplot handler
    yetty_ypaint_flyweight_registry_add(reg,
        YETTY_YPAINT_TYPE_YPLOT, YETTY_YPAINT_TYPE_YPLOT,
        yetty_yplot_handler);
#endif

    // TODO: yimage, yvideo handlers

    return res;
}
