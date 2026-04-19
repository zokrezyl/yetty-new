/*
 * render-target-texture.c - Texture render target implementation
 *
 * Renders to a GPU texture. Used for:
 * - Layer targets (render_layer)
 * - Terminal compositing (blend layers)
 * - Big yetty texture (blend terminals)
 */

#include <yetty/yrender/render-target.h>
#include <yetty/yrender/gpu-allocator.h>
#include <yetty/yrender/gpu-resource-binder.h>
#include <yetty/yterm/terminal.h>
#include <yetty/ytrace.h>
#include <stdlib.h>

#define INCBIN_STYLE 1
#include <incbin.h>

INCBIN(blend_shader, BLEND_SHADER_PATH);

#define MAX_BLEND_SOURCES 4

struct render_target_texture {
	struct yetty_render_target base;
	WGPUDevice device;
	WGPUQueue queue;
	WGPUTextureFormat format;
	struct yetty_render_gpu_allocator *allocator;

	/* Owned texture */
	WGPUTexture texture;
	WGPUTextureView view;
	uint32_t width;
	uint32_t height;

	/* Optional surface for present() - NULL for layer/terminal targets */
	WGPUSurface surface;

	/* Binder for render_layer */
	struct yetty_render_gpu_resource_binder *binder;

	/* Blend pipeline resources (also used for present) */
	WGPUShaderModule blend_shader;
	WGPURenderPipeline blend_pipeline;
	WGPURenderPipeline present_pipeline;  /* For presenting to surface */
	WGPUBindGroupLayout blend_layout;
	WGPUSampler sampler;
	WGPUBuffer uniform_buffer;
	WGPUTexture placeholder_texture;
	WGPUTextureView placeholder_view;
};

/*=============================================================================
 * Destroy
 *===========================================================================*/

static void render_target_texture_destroy(struct yetty_render_target *self)
{
	struct render_target_texture *rt = (struct render_target_texture *)self;

	if (rt->view) {
		wgpuTextureViewRelease(rt->view);
		rt->view = NULL;
	}
	if (rt->texture) {
		if (rt->allocator)
			rt->allocator->ops->release_texture(rt->allocator, rt->texture);
		else {
			wgpuTextureDestroy(rt->texture);
			wgpuTextureRelease(rt->texture);
		}
		rt->texture = NULL;
	}
	if (rt->binder) {
		rt->binder->ops->destroy(rt->binder);
		rt->binder = NULL;
	}
	if (rt->blend_pipeline) {
		wgpuRenderPipelineRelease(rt->blend_pipeline);
		rt->blend_pipeline = NULL;
	}
	if (rt->blend_layout) {
		wgpuBindGroupLayoutRelease(rt->blend_layout);
		rt->blend_layout = NULL;
	}
	if (rt->blend_shader) {
		wgpuShaderModuleRelease(rt->blend_shader);
		rt->blend_shader = NULL;
	}
	if (rt->sampler) {
		wgpuSamplerRelease(rt->sampler);
		rt->sampler = NULL;
	}
	if (rt->uniform_buffer) {
		wgpuBufferDestroy(rt->uniform_buffer);
		wgpuBufferRelease(rt->uniform_buffer);
		rt->uniform_buffer = NULL;
	}
	if (rt->placeholder_view) {
		wgpuTextureViewRelease(rt->placeholder_view);
		rt->placeholder_view = NULL;
	}
	if (rt->placeholder_texture) {
		wgpuTextureDestroy(rt->placeholder_texture);
		wgpuTextureRelease(rt->placeholder_texture);
		rt->placeholder_texture = NULL;
	}

	free(rt);
}

/*=============================================================================
 * Resize
 *===========================================================================*/

static struct yetty_core_void_result
render_target_texture_resize(struct yetty_render_target *self,
			     uint32_t width, uint32_t height)
{
	struct render_target_texture *rt = (struct render_target_texture *)self;

	if (rt->width == width && rt->height == height)
		return YETTY_OK_VOID();

	/* Release old texture */
	if (rt->view) {
		wgpuTextureViewRelease(rt->view);
		rt->view = NULL;
	}
	if (rt->texture) {
		if (rt->allocator)
			rt->allocator->ops->release_texture(rt->allocator, rt->texture);
		else {
			wgpuTextureDestroy(rt->texture);
			wgpuTextureRelease(rt->texture);
		}
		rt->texture = NULL;
	}

	rt->width = width;
	rt->height = height;

	/* Create new texture */
	WGPUTextureDescriptor tex_desc = {0};
	tex_desc.label = (WGPUStringView){.data = "render_target", .length = 13};
	tex_desc.usage = WGPUTextureUsage_RenderAttachment |
			 WGPUTextureUsage_TextureBinding;
	tex_desc.dimension = WGPUTextureDimension_2D;
	tex_desc.size.width = width;
	tex_desc.size.height = height;
	tex_desc.size.depthOrArrayLayers = 1;
	tex_desc.format = rt->format;
	tex_desc.mipLevelCount = 1;
	tex_desc.sampleCount = 1;

	if (rt->allocator)
		rt->texture = rt->allocator->ops->create_texture(rt->allocator, &tex_desc);
	else
		rt->texture = wgpuDeviceCreateTexture(rt->device, &tex_desc);

	if (!rt->texture)
		return YETTY_ERR(yetty_core_void, "failed to create texture");

	rt->view = wgpuTextureCreateView(rt->texture, NULL);
	if (!rt->view) {
		if (rt->allocator)
			rt->allocator->ops->release_texture(rt->allocator, rt->texture);
		else {
			wgpuTextureDestroy(rt->texture);
			wgpuTextureRelease(rt->texture);
		}
		rt->texture = NULL;
		return YETTY_ERR(yetty_core_void, "failed to create view");
	}

	ydebug("render_target_texture: resized to %ux%u", width, height);
	return YETTY_OK_VOID();
}

/*=============================================================================
 * Accessors
 *===========================================================================*/

static WGPUTextureView
render_target_texture_get_view(const struct yetty_render_target *self)
{
	const struct render_target_texture *rt =
		(const struct render_target_texture *)self;
	return rt->view;
}

static uint32_t
render_target_texture_get_width(const struct yetty_render_target *self)
{
	const struct render_target_texture *rt =
		(const struct render_target_texture *)self;
	return rt->width;
}

static uint32_t
render_target_texture_get_height(const struct yetty_render_target *self)
{
	const struct render_target_texture *rt =
		(const struct render_target_texture *)self;
	return rt->height;
}

/*=============================================================================
 * render_layer - render a terminal layer to this target
 *===========================================================================*/

static struct yetty_core_void_result
render_target_texture_render_layer(struct yetty_render_target *self,
				   struct yetty_term_terminal_layer *layer)
{
	struct render_target_texture *rt = (struct render_target_texture *)self;

	/* Early out if not dirty */
	if (!layer->dirty)
		return YETTY_OK_VOID();

	/* Get gpu_resource_set from layer */
	struct yetty_render_gpu_resource_set_result rs_res =
		layer->ops->get_gpu_resource_set(layer);
	if (!YETTY_IS_OK(rs_res))
		return YETTY_ERR(yetty_core_void, rs_res.error.msg);

	const struct yetty_render_gpu_resource_set *rs = rs_res.value;

	/* Submit to binder */
	struct yetty_core_void_result res = rt->binder->ops->submit(rt->binder, rs);
	if (!YETTY_IS_OK(res))
		return res;

	/* Finalize (compile shader if needed) */
	res = rt->binder->ops->finalize(rt->binder);
	if (!YETTY_IS_OK(res))
		return res;

	/* Update uniforms/buffers */
	res = rt->binder->ops->update(rt->binder);
	if (!YETTY_IS_OK(res))
		return res;

	/* Create command encoder */
	WGPUCommandEncoderDescriptor enc_desc = {0};
	WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(rt->device, &enc_desc);
	if (!encoder)
		return YETTY_ERR(yetty_core_void, "failed to create encoder");

	/* Begin render pass */
	WGPURenderPassColorAttachment color_attachment = {0};
	color_attachment.view = rt->view;
	color_attachment.loadOp = WGPULoadOp_Clear;
	color_attachment.storeOp = WGPUStoreOp_Store;
	color_attachment.clearValue = (WGPUColor){0.0, 0.0, 0.0, 0.0};
	color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

	WGPURenderPassDescriptor pass_desc = {0};
	pass_desc.colorAttachmentCount = 1;
	pass_desc.colorAttachments = &color_attachment;

	WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
	if (!pass) {
		wgpuCommandEncoderRelease(encoder);
		return YETTY_ERR(yetty_core_void, "failed to begin render pass");
	}

	/* Bind and draw */
	WGPURenderPipeline pipeline = rt->binder->ops->get_pipeline(rt->binder);
	WGPUBuffer quad_vb = rt->binder->ops->get_quad_vertex_buffer(rt->binder);

	if (pipeline && quad_vb) {
		wgpuRenderPassEncoderSetPipeline(pass, pipeline);
		rt->binder->ops->bind(rt->binder, pass, 0);
		wgpuRenderPassEncoderSetVertexBuffer(pass, 0, quad_vb, 0, WGPU_WHOLE_SIZE);
		wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
	}

	wgpuRenderPassEncoderEnd(pass);
	wgpuRenderPassEncoderRelease(pass);

	/* Submit */
	WGPUCommandBufferDescriptor cmd_desc = {0};
	WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cmd_desc);
	wgpuQueueSubmit(rt->queue, 1, &cmd);
	wgpuCommandBufferRelease(cmd);
	wgpuCommandEncoderRelease(encoder);

	/* Clear dirty flag */
	layer->dirty = 0;

	ydebug("render_target_texture: rendered layer");
	return YETTY_OK_VOID();
}

/*=============================================================================
 * blend - blend multiple source targets into this target
 *===========================================================================*/

static struct yetty_core_void_result create_blend_pipeline(struct render_target_texture *rt)
{
	/* Shader module */
	WGPUShaderSourceWGSL wgsl_src = {0};
	wgsl_src.chain.sType = WGPUSType_ShaderSourceWGSL;
	wgsl_src.code = (WGPUStringView){
		.data = (const char *)gblend_shader_data,
		.length = gblend_shader_size
	};

	WGPUShaderModuleDescriptor shader_desc = {0};
	shader_desc.nextInChain = (WGPUChainedStruct *)&wgsl_src;

	rt->blend_shader = wgpuDeviceCreateShaderModule(rt->device, &shader_desc);
	if (!rt->blend_shader)
		return YETTY_ERR(yetty_core_void, "failed to create blend shader");

	/* Bind group layout */
	WGPUBindGroupLayoutEntry entries[MAX_BLEND_SOURCES + 2] = {0};

	for (int i = 0; i < MAX_BLEND_SOURCES; i++) {
		entries[i].binding = i;
		entries[i].visibility = WGPUShaderStage_Fragment;
		entries[i].texture.sampleType = WGPUTextureSampleType_Float;
		entries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
	}
	entries[MAX_BLEND_SOURCES].binding = MAX_BLEND_SOURCES;
	entries[MAX_BLEND_SOURCES].visibility = WGPUShaderStage_Fragment;
	entries[MAX_BLEND_SOURCES].sampler.type = WGPUSamplerBindingType_Filtering;

	entries[MAX_BLEND_SOURCES + 1].binding = MAX_BLEND_SOURCES + 1;
	entries[MAX_BLEND_SOURCES + 1].visibility = WGPUShaderStage_Fragment;
	entries[MAX_BLEND_SOURCES + 1].buffer.type = WGPUBufferBindingType_Uniform;
	entries[MAX_BLEND_SOURCES + 1].buffer.minBindingSize = 16;

	WGPUBindGroupLayoutDescriptor bgl_desc = {0};
	bgl_desc.entryCount = MAX_BLEND_SOURCES + 2;
	bgl_desc.entries = entries;

	rt->blend_layout = wgpuDeviceCreateBindGroupLayout(rt->device, &bgl_desc);
	if (!rt->blend_layout)
		return YETTY_ERR(yetty_core_void, "failed to create blend layout");

	/* Pipeline layout */
	WGPUPipelineLayoutDescriptor pl_desc = {0};
	pl_desc.bindGroupLayoutCount = 1;
	pl_desc.bindGroupLayouts = &rt->blend_layout;

	WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(rt->device, &pl_desc);
	if (!layout)
		return YETTY_ERR(yetty_core_void, "failed to create pipeline layout");

	/* Render pipeline */
	WGPURenderPipelineDescriptor rp_desc = {0};
	rp_desc.layout = layout;
	rp_desc.vertex.module = rt->blend_shader;
	rp_desc.vertex.entryPoint = (WGPUStringView){.data = "vs_main", .length = 7};

	WGPUColorTargetState color_target = {0};
	color_target.format = rt->format;
	color_target.writeMask = WGPUColorWriteMask_All;

	WGPUFragmentState fragment = {0};
	fragment.module = rt->blend_shader;
	fragment.entryPoint = (WGPUStringView){.data = "fs_main", .length = 7};
	fragment.targetCount = 1;
	fragment.targets = &color_target;
	rp_desc.fragment = &fragment;

	rp_desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
	rp_desc.primitive.frontFace = WGPUFrontFace_CCW;
	rp_desc.primitive.cullMode = WGPUCullMode_None;
	rp_desc.multisample.count = 1;
	rp_desc.multisample.mask = ~0u;

	rt->blend_pipeline = wgpuDeviceCreateRenderPipeline(rt->device, &rp_desc);
	wgpuPipelineLayoutRelease(layout);

	if (!rt->blend_pipeline)
		return YETTY_ERR(yetty_core_void, "failed to create blend pipeline");

	/* Sampler */
	WGPUSamplerDescriptor sampler_desc = {0};
	sampler_desc.minFilter = WGPUFilterMode_Linear;
	sampler_desc.magFilter = WGPUFilterMode_Linear;
	sampler_desc.addressModeU = WGPUAddressMode_ClampToEdge;
	sampler_desc.addressModeV = WGPUAddressMode_ClampToEdge;
	sampler_desc.maxAnisotropy = 1;

	rt->sampler = wgpuDeviceCreateSampler(rt->device, &sampler_desc);
	if (!rt->sampler)
		return YETTY_ERR(yetty_core_void, "failed to create sampler");

	/* Uniform buffer */
	WGPUBufferDescriptor buf_desc = {0};
	buf_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
	buf_desc.size = 16;

	rt->uniform_buffer = wgpuDeviceCreateBuffer(rt->device, &buf_desc);
	if (!rt->uniform_buffer)
		return YETTY_ERR(yetty_core_void, "failed to create uniform buffer");

	/* Placeholder texture */
	WGPUTextureDescriptor tex_desc = {0};
	tex_desc.usage = WGPUTextureUsage_TextureBinding;
	tex_desc.dimension = WGPUTextureDimension_2D;
	tex_desc.size.width = 1;
	tex_desc.size.height = 1;
	tex_desc.size.depthOrArrayLayers = 1;
	tex_desc.format = WGPUTextureFormat_RGBA8Unorm;
	tex_desc.mipLevelCount = 1;
	tex_desc.sampleCount = 1;

	rt->placeholder_texture = wgpuDeviceCreateTexture(rt->device, &tex_desc);
	if (!rt->placeholder_texture)
		return YETTY_ERR(yetty_core_void, "failed to create placeholder texture");

	rt->placeholder_view = wgpuTextureCreateView(rt->placeholder_texture, NULL);
	if (!rt->placeholder_view)
		return YETTY_ERR(yetty_core_void, "failed to create placeholder view");

	ydebug("render_target_texture: blend pipeline created");
	return YETTY_OK_VOID();
}

static struct yetty_core_void_result
render_target_texture_blend(struct yetty_render_target *self,
			    struct yetty_render_target **sources,
			    size_t count)
{
	struct render_target_texture *rt = (struct render_target_texture *)self;

	if (count == 0)
		return YETTY_OK_VOID();

	if (count > MAX_BLEND_SOURCES) {
		yerror("render_target_texture: too many sources (%zu > %d)", count, MAX_BLEND_SOURCES);
		count = MAX_BLEND_SOURCES;
	}

	/* Create blend pipeline on first use */
	if (!rt->blend_pipeline) {
		struct yetty_core_void_result res = create_blend_pipeline(rt);
		if (!YETTY_IS_OK(res))
			return res;
	}

	/* Update uniforms */
	uint32_t uniforms[4] = {(uint32_t)count, 0, 0, 0};
	wgpuQueueWriteBuffer(rt->queue, rt->uniform_buffer, 0, uniforms, sizeof(uniforms));

	/* Collect source views */
	WGPUTextureView source_views[MAX_BLEND_SOURCES];
	for (size_t i = 0; i < MAX_BLEND_SOURCES; i++) {
		if (i < count && sources[i] && sources[i]->ops->get_view)
			source_views[i] = sources[i]->ops->get_view(sources[i]);
		else
			source_views[i] = rt->placeholder_view;
	}

	/* Create bind group */
	WGPUBindGroupEntry bg_entries[MAX_BLEND_SOURCES + 2] = {0};
	for (int i = 0; i < MAX_BLEND_SOURCES; i++) {
		bg_entries[i].binding = i;
		bg_entries[i].textureView = source_views[i];
	}
	bg_entries[MAX_BLEND_SOURCES].binding = MAX_BLEND_SOURCES;
	bg_entries[MAX_BLEND_SOURCES].sampler = rt->sampler;
	bg_entries[MAX_BLEND_SOURCES + 1].binding = MAX_BLEND_SOURCES + 1;
	bg_entries[MAX_BLEND_SOURCES + 1].buffer = rt->uniform_buffer;
	bg_entries[MAX_BLEND_SOURCES + 1].size = 16;

	WGPUBindGroupDescriptor bg_desc = {0};
	bg_desc.layout = rt->blend_layout;
	bg_desc.entryCount = MAX_BLEND_SOURCES + 2;
	bg_desc.entries = bg_entries;

	WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(rt->device, &bg_desc);
	if (!bind_group)
		return YETTY_ERR(yetty_core_void, "failed to create bind group");

	/* Create encoder */
	WGPUCommandEncoderDescriptor enc_desc = {0};
	WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(rt->device, &enc_desc);
	if (!encoder) {
		wgpuBindGroupRelease(bind_group);
		return YETTY_ERR(yetty_core_void, "failed to create encoder");
	}

	/* Render pass */
	WGPURenderPassColorAttachment color_attachment = {0};
	color_attachment.view = rt->view;
	color_attachment.loadOp = WGPULoadOp_Clear;
	color_attachment.storeOp = WGPUStoreOp_Store;
	color_attachment.clearValue = (WGPUColor){0.0, 0.0, 0.0, 1.0};
	color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

	WGPURenderPassDescriptor pass_desc = {0};
	pass_desc.colorAttachmentCount = 1;
	pass_desc.colorAttachments = &color_attachment;

	WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
	if (!pass) {
		wgpuCommandEncoderRelease(encoder);
		wgpuBindGroupRelease(bind_group);
		return YETTY_ERR(yetty_core_void, "failed to begin render pass");
	}

	wgpuRenderPassEncoderSetPipeline(pass, rt->blend_pipeline);
	wgpuRenderPassEncoderSetBindGroup(pass, 0, bind_group, 0, NULL);
	wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);

	wgpuRenderPassEncoderEnd(pass);
	wgpuRenderPassEncoderRelease(pass);

	/* Submit */
	WGPUCommandBufferDescriptor cmd_desc = {0};
	WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cmd_desc);
	wgpuQueueSubmit(rt->queue, 1, &cmd);
	wgpuCommandBufferRelease(cmd);
	wgpuCommandEncoderRelease(encoder);
	wgpuBindGroupRelease(bind_group);

	ydebug("render_target_texture: blended %zu sources", count);
	return YETTY_OK_VOID();
}

/*=============================================================================
 * present - blit texture to surface (if surface was provided at creation)
 *===========================================================================*/

static struct yetty_core_void_result
render_target_texture_present(struct yetty_render_target *self)
{
	struct render_target_texture *rt = (struct render_target_texture *)self;

	if (!rt->surface)
		return YETTY_ERR(yetty_core_void, "no surface configured for present");

	/* Acquire surface texture */
	WGPUSurfaceTexture surface_texture;
	wgpuSurfaceGetCurrentTexture(rt->surface, &surface_texture);

	if (surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
	    surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
		return YETTY_ERR(yetty_core_void, "surface not ready");

	WGPUTextureView surface_view = wgpuTextureCreateView(surface_texture.texture, NULL);
	if (!surface_view)
		return YETTY_ERR(yetty_core_void, "failed to create surface view");

	/* Create blend pipeline if needed (reuse for present) */
	if (!rt->blend_pipeline) {
		struct yetty_core_void_result res = create_blend_pipeline(rt);
		if (!YETTY_IS_OK(res)) {
			wgpuTextureViewRelease(surface_view);
			return res;
		}
	}

	/* Update uniforms - single source */
	uint32_t uniforms[4] = {1, 0, 0, 0};
	wgpuQueueWriteBuffer(rt->queue, rt->uniform_buffer, 0, uniforms, sizeof(uniforms));

	/* Create bind group with this target's texture as source */
	WGPUTextureView source_views[MAX_BLEND_SOURCES];
	source_views[0] = rt->view;
	for (int i = 1; i < MAX_BLEND_SOURCES; i++)
		source_views[i] = rt->placeholder_view;

	WGPUBindGroupEntry bg_entries[MAX_BLEND_SOURCES + 2] = {0};
	for (int i = 0; i < MAX_BLEND_SOURCES; i++) {
		bg_entries[i].binding = i;
		bg_entries[i].textureView = source_views[i];
	}
	bg_entries[MAX_BLEND_SOURCES].binding = MAX_BLEND_SOURCES;
	bg_entries[MAX_BLEND_SOURCES].sampler = rt->sampler;
	bg_entries[MAX_BLEND_SOURCES + 1].binding = MAX_BLEND_SOURCES + 1;
	bg_entries[MAX_BLEND_SOURCES + 1].buffer = rt->uniform_buffer;
	bg_entries[MAX_BLEND_SOURCES + 1].size = 16;

	WGPUBindGroupDescriptor bg_desc = {0};
	bg_desc.layout = rt->blend_layout;
	bg_desc.entryCount = MAX_BLEND_SOURCES + 2;
	bg_desc.entries = bg_entries;

	WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(rt->device, &bg_desc);
	if (!bind_group) {
		wgpuTextureViewRelease(surface_view);
		return YETTY_ERR(yetty_core_void, "failed to create bind group");
	}

	/* Create encoder */
	WGPUCommandEncoderDescriptor enc_desc = {0};
	WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(rt->device, &enc_desc);
	if (!encoder) {
		wgpuBindGroupRelease(bind_group);
		wgpuTextureViewRelease(surface_view);
		return YETTY_ERR(yetty_core_void, "failed to create encoder");
	}

	/* Render pass to surface */
	WGPURenderPassColorAttachment color_attachment = {0};
	color_attachment.view = surface_view;
	color_attachment.loadOp = WGPULoadOp_Clear;
	color_attachment.storeOp = WGPUStoreOp_Store;
	color_attachment.clearValue = (WGPUColor){0.0, 0.0, 0.0, 1.0};
	color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

	WGPURenderPassDescriptor pass_desc = {0};
	pass_desc.colorAttachmentCount = 1;
	pass_desc.colorAttachments = &color_attachment;

	WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
	if (!pass) {
		wgpuCommandEncoderRelease(encoder);
		wgpuBindGroupRelease(bind_group);
		wgpuTextureViewRelease(surface_view);
		return YETTY_ERR(yetty_core_void, "failed to begin render pass");
	}

	wgpuRenderPassEncoderSetPipeline(pass, rt->blend_pipeline);
	wgpuRenderPassEncoderSetBindGroup(pass, 0, bind_group, 0, NULL);
	wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);

	wgpuRenderPassEncoderEnd(pass);
	wgpuRenderPassEncoderRelease(pass);

	/* Submit */
	WGPUCommandBufferDescriptor cmd_desc = {0};
	WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cmd_desc);
	wgpuQueueSubmit(rt->queue, 1, &cmd);
	wgpuCommandBufferRelease(cmd);
	wgpuCommandEncoderRelease(encoder);
	wgpuBindGroupRelease(bind_group);

	/* Present */
	wgpuSurfacePresent(rt->surface);
	wgpuTextureViewRelease(surface_view);

	ydebug("render_target_texture: presented to surface");
	return YETTY_OK_VOID();
}

/*=============================================================================
 * vtable and create
 *===========================================================================*/

static const struct yetty_render_target_ops render_target_texture_ops = {
	.destroy = render_target_texture_destroy,
	.render_layer = render_target_texture_render_layer,
	.blend = render_target_texture_blend,
	.present = render_target_texture_present,
	.get_view = render_target_texture_get_view,
	.get_width = render_target_texture_get_width,
	.get_height = render_target_texture_get_height,
	.resize = render_target_texture_resize,
};

struct yetty_render_target_ptr_result
yetty_render_target_texture_create(WGPUDevice device, WGPUQueue queue,
				   WGPUTextureFormat format,
				   struct yetty_render_gpu_allocator *allocator,
				   WGPUSurface surface,
				   uint32_t width, uint32_t height)
{
	struct render_target_texture *rt = calloc(1, sizeof(*rt));
	if (!rt)
		return YETTY_ERR(yetty_render_target_ptr, "failed to allocate render target");

	rt->base.ops = &render_target_texture_ops;
	rt->device = device;
	rt->queue = queue;
	rt->format = format;
	rt->allocator = allocator;
	rt->surface = surface;  /* NULL for layer/terminal targets */

	/* Create binder */
	struct yetty_render_gpu_resource_binder_result binder_res =
		yetty_render_gpu_resource_binder_create(device, queue, format, allocator);
	if (!YETTY_IS_OK(binder_res)) {
		free(rt);
		return YETTY_ERR(yetty_render_target_ptr, binder_res.error.msg);
	}
	rt->binder = binder_res.value;

	/* Create initial texture */
	struct yetty_core_void_result res = render_target_texture_resize(&rt->base, width, height);
	if (!YETTY_IS_OK(res)) {
		rt->binder->ops->destroy(rt->binder);
		free(rt);
		return YETTY_ERR(yetty_render_target_ptr, res.error.msg);
	}

	ydebug("yetty_render_target_texture_create: %ux%u format=%d surface=%p",
	       width, height, format, (void *)surface);
	return YETTY_OK(yetty_render_target_ptr, &rt->base);
}
