// YSDF primitive handler for buffer iteration
#pragma once

#include <yetty/ypaint-core/buffer.h>
#include <yetty/ysdf/types.gen.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline struct yetty_core_size_result
yetty_ysdf_prim_size(const uint32_t *prim) {
  size_t size = yetty_ysdf_primitive_size(prim[0]);
  if (size == 0)
    return YETTY_ERR(yetty_core_size, "unknown SDF type");
  return YETTY_OK(yetty_core_size, size);
}

static inline struct rectangle_result
yetty_ysdf_prim_aabb(const uint32_t *prim) {
  uint32_t word_count = yetty_ysdf_word_count((enum yetty_ysdf_type)prim[0]);
  if (word_count == 0)
    return YETTY_ERR(rectangle, "unknown SDF type");
  return yetty_ysdf_compute_aabb((const float *)prim, word_count);
}

static const struct yetty_ypaint_prim_ops yetty_ysdf_prim_ops = {
    .size = yetty_ysdf_prim_size,
    .aabb = yetty_ysdf_prim_aabb,
};

static inline struct yetty_ypaint_prim_flyweight
yetty_ysdf_handler(const uint32_t *prim) {
  uint32_t type = prim[0];
  if (type < 256 && yetty_ysdf_primitive_size(type) > 0)
    return (struct yetty_ypaint_prim_flyweight){.data = prim,
                                                .ops = &yetty_ysdf_prim_ops};
  return (struct yetty_ypaint_prim_flyweight){.data = NULL, .ops = NULL};
}

#ifdef __cplusplus
}
#endif
