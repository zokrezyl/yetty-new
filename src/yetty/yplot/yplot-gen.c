// Auto-generated from yplot.yaml - DO NOT EDIT

#include "yplot-gen.h"
#include <yetty/yrender/gpu-resource-binder.h>
#include <yetty/yrender/gpu-resource-set.h>
#include <yetty/ypaint-core/complex-prim-types.h>
#include <yetty/ytrace.h>
#include <stdlib.h>
#include <string.h>
#include <yetty/yfsvm/compiler.h>

extern const unsigned char gyplot_shaderData[];
extern const unsigned int gyplot_shaderSize;

struct yplot_factory {
    struct yetty_ypaint_concrete_factory base;
    struct yetty_render_gpu_resource_set rs;
    struct yetty_render_gpu_resource_binder *binder;
};

static struct yplot_factory *yplot_factory_from_base(struct yetty_ypaint_concrete_factory *base)
{
    return (struct yplot_factory *)base;
}

//=============================================================================
// Serialization
//=============================================================================

size_t yetty_yplot_serialized_size(
    const struct yetty_yplot_uniforms *uniforms,
    const struct yetty_yplot_buffers *buffers)
{
    (void)uniforms;
    // Wire format: [type_id][payload_size][uniforms...][buffer_lens...][buffer_data...]
    size_t total_buf_words = buffers->bytecode_len;
    return (2 + 18 + 1 + total_buf_words) * sizeof(uint32_t);
}

struct yetty_core_size_result yetty_yplot_serialize(
    const struct yetty_yplot_uniforms *uniforms,
    const struct yetty_yplot_buffers *buffers,
    uint8_t *out, size_t out_capacity)
{
    if (!uniforms || !buffers)
        return YETTY_ERR(yetty_core_size, "null argument");
    if (!out)
        return YETTY_ERR(yetty_core_size, "out is NULL");

    size_t total_buf_words = buffers->bytecode_len;
    size_t required = (2 + 18 + 1 + total_buf_words) * sizeof(uint32_t);
    if (out_capacity < required)
        return YETTY_ERR(yetty_core_size, "buffer too small");

    uint32_t *p = (uint32_t *)out;
    *p++ = YETTY_YPLOT_TYPE_ID;
    *p++ = (uint32_t)(required - 2 * sizeof(uint32_t));

    // Copy uniforms as raw words
    memcpy(p, uniforms, sizeof(struct yetty_yplot_uniforms));
    p += 18;

    // Write buffer lengths
    *p++ = (uint32_t)buffers->bytecode_len;

    // Copy buffer data
    if (buffers->bytecode && buffers->bytecode_len > 0)
        memcpy(p, buffers->bytecode, buffers->bytecode_len * sizeof(uint32_t));
    p += buffers->bytecode_len;

    return YETTY_OK(yetty_core_size, required);
}

//=============================================================================
// Resource Set Setup
//=============================================================================

static void yplot_init_rs(struct yplot_factory *factory)
{
    struct yetty_render_gpu_resource_set *rs = &factory->rs;
    memset(rs, 0, sizeof(*rs));
    strncpy(rs->namespace, "yplot", YETTY_RENDER_NAME_MAX - 1);
    yetty_render_shader_code_set(&rs->shader,
        (const char *)gyplot_shaderData, gyplot_shaderSize);

    // Library: yfsvm
    const struct yetty_render_gpu_resource_set *yfsvm_rs =
        yetty_yfsvm_get_shader_resource_set();
    if (yfsvm_rs) {
        rs->children[0] = (struct yetty_render_gpu_resource_set *)yfsvm_rs;
        rs->children_count = 1;
    }

    // Setup uniforms (values set later during render)
    strncpy(rs->uniforms[0].name, "bounds_x", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[0].type = YETTY_RENDER_UNIFORM_F32;
    rs->uniforms[0].u32 = 0;
    strncpy(rs->uniforms[1].name, "bounds_y", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[1].type = YETTY_RENDER_UNIFORM_F32;
    rs->uniforms[1].u32 = 0;
    strncpy(rs->uniforms[2].name, "bounds_w", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[2].type = YETTY_RENDER_UNIFORM_F32;
    rs->uniforms[2].u32 = 0;
    strncpy(rs->uniforms[3].name, "bounds_h", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[3].type = YETTY_RENDER_UNIFORM_F32;
    rs->uniforms[3].u32 = 0;
    strncpy(rs->uniforms[4].name, "x_min", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[4].type = YETTY_RENDER_UNIFORM_F32;
    rs->uniforms[4].u32 = 0;
    strncpy(rs->uniforms[5].name, "x_max", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[5].type = YETTY_RENDER_UNIFORM_F32;
    rs->uniforms[5].u32 = 0;
    strncpy(rs->uniforms[6].name, "y_min", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[6].type = YETTY_RENDER_UNIFORM_F32;
    rs->uniforms[6].u32 = 0;
    strncpy(rs->uniforms[7].name, "y_max", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[7].type = YETTY_RENDER_UNIFORM_F32;
    rs->uniforms[7].u32 = 0;
    strncpy(rs->uniforms[8].name, "flags", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[8].type = YETTY_RENDER_UNIFORM_U32;
    rs->uniforms[8].u32 = 0;
    strncpy(rs->uniforms[9].name, "function_count", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[9].type = YETTY_RENDER_UNIFORM_U32;
    rs->uniforms[9].u32 = 0;
    strncpy(rs->uniforms[10].name, "colors_0", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[10].type = YETTY_RENDER_UNIFORM_U32;
    rs->uniforms[10].u32 = 0;
    strncpy(rs->uniforms[11].name, "colors_1", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[11].type = YETTY_RENDER_UNIFORM_U32;
    rs->uniforms[11].u32 = 0;
    strncpy(rs->uniforms[12].name, "colors_2", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[12].type = YETTY_RENDER_UNIFORM_U32;
    rs->uniforms[12].u32 = 0;
    strncpy(rs->uniforms[13].name, "colors_3", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[13].type = YETTY_RENDER_UNIFORM_U32;
    rs->uniforms[13].u32 = 0;
    strncpy(rs->uniforms[14].name, "colors_4", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[14].type = YETTY_RENDER_UNIFORM_U32;
    rs->uniforms[14].u32 = 0;
    strncpy(rs->uniforms[15].name, "colors_5", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[15].type = YETTY_RENDER_UNIFORM_U32;
    rs->uniforms[15].u32 = 0;
    strncpy(rs->uniforms[16].name, "colors_6", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[16].type = YETTY_RENDER_UNIFORM_U32;
    rs->uniforms[16].u32 = 0;
    strncpy(rs->uniforms[17].name, "colors_7", YETTY_RENDER_NAME_MAX - 1);
    rs->uniforms[17].type = YETTY_RENDER_UNIFORM_U32;
    rs->uniforms[17].u32 = 0;
    rs->uniform_count = 18;

    // Setup storage buffer for buffer data
    rs->buffer_count = 1;
    strncpy(rs->buffers[0].name, "buffer", YETTY_RENDER_NAME_MAX - 1);
    strncpy(rs->buffers[0].wgsl_type, "array<u32>", YETTY_RENDER_WGSL_TYPE_MAX - 1);
    rs->buffers[0].readonly = 1;
}

//=============================================================================
// Instance Rendering
//=============================================================================

static struct yetty_core_void_result
yplot_instance_render(struct yetty_ypaint_complex_prim_instance *self,
                       struct yetty_render_target *target, float x, float y)
{
    if (!self || !self->buffer_data || !self->factory)
        return YETTY_ERR(yetty_core_void, "invalid instance");

    struct yplot_factory *factory = yplot_factory_from_base(self->factory);
    if (!factory->binder)
        return YETTY_ERR(yetty_core_void, "binder not initialized");

    struct yetty_render_gpu_resource_set *rs = &factory->rs;

    // Parse wire format: [type_id][payload_size][uniforms...][buffer_lens...][buffer_data...]
    const uint32_t *data = (const uint32_t *)self->buffer_data;
    const uint32_t *payload = data + 2;  // skip type_id and payload_size

    // Update uniforms from wire format
    rs->uniforms[0].f32 = *(float *)&payload[0];
    rs->uniforms[1].f32 = *(float *)&payload[1];
    rs->uniforms[2].f32 = *(float *)&payload[2];
    rs->uniforms[3].f32 = *(float *)&payload[3];
    rs->uniforms[4].f32 = *(float *)&payload[4];
    rs->uniforms[5].f32 = *(float *)&payload[5];
    rs->uniforms[6].f32 = *(float *)&payload[6];
    rs->uniforms[7].f32 = *(float *)&payload[7];
    rs->uniforms[8].u32 = payload[8];
    rs->uniforms[9].u32 = payload[9];
    rs->uniforms[10].u32 = payload[10];
    rs->uniforms[11].u32 = payload[11];
    rs->uniforms[12].u32 = payload[12];
    rs->uniforms[13].u32 = payload[13];
    rs->uniforms[14].u32 = payload[14];
    rs->uniforms[15].u32 = payload[15];
    rs->uniforms[16].u32 = payload[16];
    rs->uniforms[17].u32 = payload[17];

    // Get buffer data (after uniforms and length fields)
    const uint32_t *buffer_data = payload + 19;
    size_t buffer_words = payload[18];  // first buffer length

    // Update storage buffer
    rs->buffers[0].data = (uint8_t *)buffer_data;
    rs->buffers[0].size = buffer_words * sizeof(uint32_t);
    rs->buffers[0].dirty = 1;

    (void)target;
    (void)x;
    (void)y;

    struct yetty_core_void_result res = factory->binder->ops->update(factory->binder);
    if (YETTY_IS_ERR(res))
        return res;

    return YETTY_OK_VOID();
}

//=============================================================================
// Factory Implementation
//=============================================================================

static struct yetty_core_void_result
yplot_compile_pipeline(struct yetty_ypaint_concrete_factory *self,
                        WGPUDevice device, WGPUQueue queue,
                        WGPUTextureFormat target_format)
{
    struct yplot_factory *factory = yplot_factory_from_base(self);

    if (factory->binder) {
        ydebug("yplot: factory already initialized");
        return YETTY_OK_VOID();
    }

    yplot_init_rs(factory);

    struct yetty_render_gpu_resource_binder_result binder_res =
        yetty_render_gpu_resource_binder_create(device, queue, target_format, NULL);
    if (YETTY_IS_ERR(binder_res))
        return YETTY_ERR(yetty_core_void, binder_res.error.msg);

    factory->binder = binder_res.value;

    struct yetty_core_void_result submit_res =
        factory->binder->ops->submit(factory->binder, &factory->rs);
    if (YETTY_IS_ERR(submit_res)) {
        factory->binder->ops->destroy(factory->binder);
        factory->binder = NULL;
        return submit_res;
    }

    struct yetty_core_void_result finalize_res =
        factory->binder->ops->finalize(factory->binder);
    if (YETTY_IS_ERR(finalize_res)) {
        factory->binder->ops->destroy(factory->binder);
        factory->binder = NULL;
        return finalize_res;
    }

    yinfo("yplot: pipeline compiled (once for lifetime)");
    return YETTY_OK_VOID();
}

static WGPURenderPipeline yplot_get_pipeline(struct yetty_ypaint_concrete_factory *self)
{
    struct yplot_factory *factory = yplot_factory_from_base(self);
    if (!factory->binder)
        return NULL;
    return factory->binder->ops->get_pipeline(factory->binder);
}

static struct yetty_ypaint_complex_prim_instance_ptr_result
yplot_create_instance(struct yetty_ypaint_concrete_factory *self,
                       const void *buffer_data, size_t size, uint32_t rolling_row)
{
    if (!buffer_data || size < sizeof(struct yetty_ypaint_complex_prim))
        return YETTY_ERR(yetty_ypaint_complex_prim_instance_ptr, "invalid buffer data");

    struct yetty_ypaint_complex_prim_instance *instance =
        calloc(1, sizeof(struct yetty_ypaint_complex_prim_instance));
    if (!instance)
        return YETTY_ERR(yetty_ypaint_complex_prim_instance_ptr, "allocation failed");

    instance->buffer_data = malloc(size);
    if (!instance->buffer_data) {
        free(instance);
        return YETTY_ERR(yetty_ypaint_complex_prim_instance_ptr, "buffer alloc failed");
    }

    memcpy(instance->buffer_data, buffer_data, size);
    instance->buffer_size = size;
    instance->type = YETTY_YPLOT_TYPE_ID;
    instance->factory = self;
    instance->rolling_row = rolling_row;
    instance->render = yplot_instance_render;

    struct rectangle_result aabb_res = yetty_ypaint_complex_prim_aabb(buffer_data);
    if (YETTY_IS_OK(aabb_res))
        instance->bounds = aabb_res.value;

    return YETTY_OK(yetty_ypaint_complex_prim_instance_ptr, instance);
}

static void yplot_destroy_instance(struct yetty_ypaint_concrete_factory *self,
                                    struct yetty_ypaint_complex_prim_instance *instance)
{
    (void)self;
    if (!instance)
        return;
    free(instance->buffer_data);
    free(instance);
}

static struct yetty_render_gpu_resource_set *yplot_get_shared_rs(
    struct yetty_ypaint_concrete_factory *self)
{
    struct yplot_factory *factory = yplot_factory_from_base(self);
    return &factory->rs;
}

struct yetty_ypaint_concrete_factory *yetty_yplot_factory_create(void)
{
    struct yplot_factory *factory = calloc(1, sizeof(struct yplot_factory));
    if (!factory)
        return NULL;

    factory->base.type_id = YETTY_YPLOT_TYPE_ID;
    factory->base.compile_pipeline = yplot_compile_pipeline;
    factory->base.get_pipeline = yplot_get_pipeline;
    factory->base.create_instance = yplot_create_instance;
    factory->base.destroy_instance = yplot_destroy_instance;
    factory->base.get_shared_rs = yplot_get_shared_rs;

    return &factory->base;
}

void yetty_yplot_factory_destroy(struct yetty_ypaint_concrete_factory *self)
{
    if (!self)
        return;

    struct yplot_factory *factory = yplot_factory_from_base(self);

    if (factory->binder)
        factory->binder->ops->destroy(factory->binder);

    free(factory);
}

