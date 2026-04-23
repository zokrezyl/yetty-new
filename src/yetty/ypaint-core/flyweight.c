// YPaint Flyweight - primitive handler registry implementation (instance-based)

#include <stdlib.h>
#include <yetty/ypaint-core/flyweight.h>
#include <yetty/ytrace.h>

#define FLYWEIGHT_MAX_HANDLERS 8

struct handler_reg {
    uint32_t type_min;
    uint32_t type_max;
    yetty_ypaint_prim_handler_fn handler;
};

struct yetty_ypaint_flyweight_registry {
    yetty_ypaint_prim_handler_fn default_handler;
    struct handler_reg handlers[FLYWEIGHT_MAX_HANDLERS];
    size_t handler_count;
};

struct yetty_ypaint_flyweight_registry_ptr_result
yetty_ypaint_flyweight_registry_create(void)
{
    struct yetty_ypaint_flyweight_registry *reg =
        calloc(1, sizeof(struct yetty_ypaint_flyweight_registry));
    if (!reg)
        return YETTY_ERR(yetty_ypaint_flyweight_registry_ptr, "calloc failed");
    return YETTY_OK(yetty_ypaint_flyweight_registry_ptr, reg);
}

void yetty_ypaint_flyweight_registry_destroy(
    struct yetty_ypaint_flyweight_registry *reg)
{
    free(reg);
}

void yetty_ypaint_flyweight_registry_set_default(
    struct yetty_ypaint_flyweight_registry *reg,
    yetty_ypaint_prim_handler_fn handler)
{
    if (reg)
        reg->default_handler = handler;
}

struct yetty_ycore_void_result yetty_ypaint_flyweight_registry_add(
    struct yetty_ypaint_flyweight_registry *reg,
    uint32_t type_min,
    uint32_t type_max,
    yetty_ypaint_prim_handler_fn handler)
{
    if (!reg)
        return YETTY_ERR(yetty_ycore_void, "reg is NULL");
    if (reg->handler_count >= FLYWEIGHT_MAX_HANDLERS)
        return YETTY_ERR(yetty_ycore_void, "max handlers reached");
    if (!handler)
        return YETTY_ERR(yetty_ycore_void, "handler is NULL");
    reg->handlers[reg->handler_count].type_min = type_min;
    reg->handlers[reg->handler_count].type_max = type_max;
    reg->handlers[reg->handler_count].handler = handler;
    reg->handler_count++;
    return YETTY_OK_VOID();
}

struct yetty_ypaint_prim_flyweight yetty_ypaint_flyweight_registry_get(
    const struct yetty_ypaint_flyweight_registry *reg,
    const uint32_t *prim)
{
    struct yetty_ypaint_prim_flyweight fw = {NULL, NULL};

    if (!reg || !prim)
        return fw;

    uint32_t type = prim[0];
    ydebug("flyweight_registry_get: type=0x%08x handler_count=%zu", type, reg->handler_count);

    // Try default handler first (fast path for SDF types)
    if (reg->default_handler) {
        fw = reg->default_handler(prim);
        if (fw.ops) {
            ydebug("flyweight_registry_get: default handler matched type=0x%08x", type);
            return fw;
        }
    }

    // Fallback to additional handlers by type range
    for (size_t i = 0; i < reg->handler_count; i++) {
        ydebug("flyweight_registry_get: checking handler[%zu] range=[0x%08x, 0x%08x]",
               i, reg->handlers[i].type_min, reg->handlers[i].type_max);
        if (type >= reg->handlers[i].type_min && type <= reg->handlers[i].type_max) {
            fw = reg->handlers[i].handler(prim);
            if (fw.ops) {
                ydebug("flyweight_registry_get: handler[%zu] matched type=0x%08x", i, type);
                return fw;
            }
        }
    }

    ydebug("flyweight_registry_get: NO HANDLER for type=0x%08x", type);
    return fw;  // ops=NULL means unhandled
}
