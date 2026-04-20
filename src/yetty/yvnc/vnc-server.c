#include <yetty/yvnc/vnc-server.h>
#include <yetty/platform/socket.h>
#include <yetty/ytrace.h>
#include "protocol.h"

#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <turbojpeg.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#define MAX_CLIENTS 16
#define FULL_REFRESH_INTERVAL 300

/* Capture state machine */
enum capture_state {
	CAPTURE_IDLE,
	CAPTURE_WAITING_COMPUTE,
	CAPTURE_WAITING_MAP,
	CAPTURE_READY_TO_SEND,
	CAPTURE_WAITING_TILE_READBACK
};

/* Per-client input buffer */
struct client_input_buffer {
	uint8_t *buffer;
	size_t buffer_size;
	size_t buffer_capacity;
	size_t needed;
	int reading_header;
	struct vnc_input_header header;
};

/* Per-client send queue */
struct client_send_buffer {
	uint8_t *queue;
	size_t queue_size;
	size_t queue_capacity;
	size_t offset;
};

struct yetty_vnc_server {
	WGPUDevice device;
	WGPUQueue queue;

	/* Event loop for async I/O */
	struct yetty_core_event_loop *event_loop;
	struct yetty_core_event_listener listener;
	yetty_core_poll_id server_poll_id;
	yetty_core_poll_id client_poll_ids[MAX_CLIENTS];

	/* Server socket */
	yetty_socket_fd server_fd;
	uint16_t port;
	int running;

	/* Connected clients */
	yetty_socket_fd clients[MAX_CLIENTS];
	struct client_input_buffer input_buffers[MAX_CLIENTS];
	struct client_send_buffer send_buffers[MAX_CLIENTS];
	size_t client_count;

	/* Frame dimensions */
	uint32_t last_width;
	uint32_t last_height;

	/* GPU tile diff resources */
	WGPUTexture prev_texture;
	WGPUBuffer dirty_flags_buffer;
	WGPUBuffer dirty_flags_readback;
	WGPUBuffer tile_readback_buffer;
	uint32_t tile_readback_buffer_size;
	WGPUComputePipeline diff_pipeline;
	WGPUBindGroup diff_bind_group;
	WGPUBindGroupLayout diff_bind_group_layout;

	/* CPU framebuffer */
	const uint8_t *cpu_pixels;
	uint32_t cpu_pixels_size;
	uint8_t *gpu_readback_pixels;
	size_t gpu_readback_pixels_size;

	/* Async state machine */
	enum capture_state capture_state;
	volatile int gpu_work_done;
	WGPUMapAsyncStatus map_status;
	WGPUTexture pending_texture;

	/* Dirty tile tracking */
	int *dirty_tiles;
	uint16_t tiles_x;
	uint16_t tiles_y;

	/* Settings */
	int merge_rectangles;
	int force_raw;
	uint8_t jpeg_quality;
	int always_full_frame;
	int use_h264;
	volatile int force_full_frame;
	volatile int awaiting_ack;

	/* JPEG compression */
	tjhandle jpeg_compressor;

	/* Stats */
	struct yetty_vnc_server_stats stats;
	struct {
		uint32_t tiles_sent;
		uint32_t tiles_jpeg;
		uint32_t tiles_raw;
		uint64_t bytes_sent;
		uint64_t bytes_jpeg;
		uint64_t bytes_raw;
		uint32_t full_updates;
		uint32_t frames;
		double last_report_time;
	} current_stats;

	uint32_t frames_since_full_refresh;

	/* Input callbacks */
	yetty_vnc_on_mouse_move_fn on_mouse_move_fn;
	void *on_mouse_move_userdata;
	yetty_vnc_on_mouse_button_fn on_mouse_button_fn;
	void *on_mouse_button_userdata;
	yetty_vnc_on_mouse_scroll_fn on_mouse_scroll_fn;
	void *on_mouse_scroll_userdata;
	yetty_vnc_on_key_down_fn on_key_down_fn;
	void *on_key_down_userdata;
	yetty_vnc_on_key_up_fn on_key_up_fn;
	void *on_key_up_userdata;
	yetty_vnc_on_text_input_fn on_text_input_fn;
	void *on_text_input_userdata;
	yetty_vnc_on_resize_fn on_resize_fn;
	void *on_resize_userdata;
	yetty_vnc_on_cell_size_fn on_cell_size_fn;
	void *on_cell_size_userdata;
	yetty_vnc_on_char_with_mods_fn on_char_with_mods_fn;
	void *on_char_with_mods_userdata;
	yetty_vnc_on_input_received_fn on_input_received_fn;
	void *on_input_received_userdata;
};

/* Compute shader for tile diff detection */
static const char *DIFF_SHADER =
"@group(0) @binding(0) var currTex: texture_2d<f32>;\n"
"@group(0) @binding(1) var prevTex: texture_2d<f32>;\n"
"@group(0) @binding(2) var<storage, read_write> dirtyFlags: array<u32>;\n"
"\n"
"const TILE_SIZE: u32 = 64;\n"
"const PIXELS_PER_THREAD: u32 = 8;\n"
"\n"
"@compute @workgroup_size(8, 8)\n"
"fn main(@builtin(local_invocation_id) lid: vec3<u32>,\n"
"        @builtin(workgroup_id) wgid: vec3<u32>) {\n"
"    let dims = textureDimensions(currTex);\n"
"    let tilesX = (dims.x + TILE_SIZE - 1u) / TILE_SIZE;\n"
"    let tileIdx = wgid.y * tilesX + wgid.x;\n"
"    let tileStartX = wgid.x * TILE_SIZE;\n"
"    let tileStartY = wgid.y * TILE_SIZE;\n"
"\n"
"    let regionStartX = tileStartX + lid.x * PIXELS_PER_THREAD;\n"
"    let regionStartY = tileStartY + lid.y * PIXELS_PER_THREAD;\n"
"\n"
"    for (var dy: u32 = 0u; dy < PIXELS_PER_THREAD; dy++) {\n"
"        for (var dx: u32 = 0u; dx < PIXELS_PER_THREAD; dx++) {\n"
"            let px = regionStartX + dx;\n"
"            let py = regionStartY + dy;\n"
"\n"
"            if (px >= dims.x || py >= dims.y) {\n"
"                continue;\n"
"            }\n"
"\n"
"            let curr = textureLoad(currTex, vec2<u32>(px, py), 0);\n"
"            let prev = textureLoad(prevTex, vec2<u32>(px, py), 0);\n"
"\n"
"            if (any(curr != prev)) {\n"
"                dirtyFlags[tileIdx] = 1u;\n"
"                return;\n"
"            }\n"
"        }\n"
"    }\n"
"}\n";

/* Forward declarations */
static void poll_client_input(struct yetty_vnc_server *server, int client_idx);
static void dispatch_input(struct yetty_vnc_server *server,
			   const struct vnc_input_header *hdr,
			   const uint8_t *data);
static struct yetty_core_void_result send_to_client(struct yetty_vnc_server *server,
						    int client_idx,
						    const void *data, size_t size);
static void handle_accept(struct yetty_vnc_server *server);
static struct yetty_core_void_result ensure_resources(struct yetty_vnc_server *server,
						      uint32_t width, uint32_t height);
static struct yetty_core_void_result create_diff_pipeline(struct yetty_vnc_server *server);
static struct yetty_core_void_result encode_tile(struct yetty_vnc_server *server,
						 uint16_t tx, uint16_t ty,
						 uint8_t **out_data, size_t *out_size,
						 uint8_t *out_encoding);

/* Forward declaration */
static struct yetty_core_int_result vnc_server_on_event(
	struct yetty_core_event_listener *listener,
	const struct yetty_core_event *event);

struct yetty_vnc_server_ptr_result
yetty_vnc_server_create(WGPUDevice device, WGPUQueue queue,
			struct yetty_core_event_loop *event_loop)
{
	if (!event_loop)
		return YETTY_ERR(yetty_vnc_server_ptr, "event_loop is NULL");

	struct yetty_vnc_server *server =
		calloc(1, sizeof(struct yetty_vnc_server));
	if (!server)
		return YETTY_ERR(yetty_vnc_server_ptr, "failed to allocate server");

	server->device = device;
	server->queue = queue;
	server->event_loop = event_loop;
	server->listener.handler = vnc_server_on_event;
	server->server_fd = YETTY_SOCKET_INVALID;
	server->server_poll_id = -1;
	server->jpeg_quality = 80;
	server->force_full_frame = 1;
	server->capture_state = CAPTURE_IDLE;
	server->gpu_work_done = 1;

	/* Initialize client arrays */
	for (int i = 0; i < MAX_CLIENTS; i++) {
		server->clients[i] = YETTY_SOCKET_INVALID;
		server->client_poll_ids[i] = -1;
	}

	/* Initialize JPEG compressor */
	server->jpeg_compressor = tjInitCompress();
	if (!server->jpeg_compressor) {
		free(server);
		return YETTY_ERR(yetty_vnc_server_ptr, "failed to init JPEG compressor");
	}

	return YETTY_OK(yetty_vnc_server_ptr, server);
}

void yetty_vnc_server_destroy(struct yetty_vnc_server *server)
{
	if (!server)
		return;

	yetty_vnc_server_stop(server);

	/* Release GPU resources */
	if (server->prev_texture)
		wgpuTextureRelease(server->prev_texture);
	if (server->dirty_flags_buffer)
		wgpuBufferRelease(server->dirty_flags_buffer);
	if (server->dirty_flags_readback)
		wgpuBufferRelease(server->dirty_flags_readback);
	if (server->tile_readback_buffer)
		wgpuBufferRelease(server->tile_readback_buffer);
	if (server->diff_pipeline)
		wgpuComputePipelineRelease(server->diff_pipeline);
	if (server->diff_bind_group)
		wgpuBindGroupRelease(server->diff_bind_group);
	if (server->diff_bind_group_layout)
		wgpuBindGroupLayoutRelease(server->diff_bind_group_layout);

	if (server->jpeg_compressor)
		tjDestroy(server->jpeg_compressor);

	free(server->dirty_tiles);
	free(server->gpu_readback_pixels);

	/* Free client buffers */
	for (int i = 0; i < MAX_CLIENTS; i++) {
		free(server->input_buffers[i].buffer);
		free(server->send_buffers[i].queue);
	}

	free(server);
}

struct yetty_core_void_result
yetty_vnc_server_start(struct yetty_vnc_server *server, uint16_t port)
{
	if (!server)
		return YETTY_ERR(yetty_core_void, "null server");

	if (server->running)
		return YETTY_ERR(yetty_core_void, "already running");

	struct yetty_socket_fd_result sock_res =
		yetty_platform_socket_create_tcp();
	if (!YETTY_IS_OK(sock_res))
		return YETTY_ERR(yetty_core_void, "socket create failed");

	server->server_fd = sock_res.value;

	struct yetty_core_void_result res;

	res = yetty_platform_socket_set_reuseaddr(server->server_fd, 1);
	if (!YETTY_IS_OK(res))
		goto fail;

	res = yetty_platform_socket_set_nonblocking(server->server_fd);
	if (!YETTY_IS_OK(res))
		goto fail;

	res = yetty_platform_socket_bind(server->server_fd, port);
	if (!YETTY_IS_OK(res))
		goto fail;

	res = yetty_platform_socket_listen(server->server_fd, 4);
	if (!YETTY_IS_OK(res))
		goto fail;

	server->port = port;
	server->running = 1;

	/* Register server socket with event loop */
	struct yetty_core_poll_id_result poll_res =
		server->event_loop->ops->create_poll(server->event_loop);
	if (!YETTY_IS_OK(poll_res)) {
		res = YETTY_ERR(yetty_core_void, "failed to create server poll");
		goto fail;
	}
	server->server_poll_id = poll_res.value;

	res = server->event_loop->ops->config_poll(server->event_loop,
						   server->server_poll_id,
						   server->server_fd);
	if (!YETTY_IS_OK(res))
		goto fail_poll;

	res = server->event_loop->ops->register_poll_listener(
		server->event_loop, server->server_poll_id, &server->listener);
	if (!YETTY_IS_OK(res))
		goto fail_poll;

	res = server->event_loop->ops->start_poll(server->event_loop,
						  server->server_poll_id,
						  YETTY_CORE_POLL_READABLE);
	if (!YETTY_IS_OK(res))
		goto fail_poll;

	yinfo("VNC server listening on port %u", port);
	return YETTY_OK_VOID();

	/* Centralized cleanup (kernel style) */
fail_poll:
	server->event_loop->ops->destroy_poll(server->event_loop,
					      server->server_poll_id);
	server->server_poll_id = -1;
fail:
	yetty_platform_socket_close(server->server_fd);
	server->server_fd = YETTY_SOCKET_INVALID;
	return res;
}

struct yetty_core_void_result
yetty_vnc_server_stop(struct yetty_vnc_server *server)
{
	if (!server)
		return YETTY_OK_VOID();

	server->running = 0;

	/* Unregister and close server socket */
	if (server->server_poll_id >= 0) {
		server->event_loop->ops->stop_poll(server->event_loop,
						   server->server_poll_id);
		server->event_loop->ops->destroy_poll(server->event_loop,
						      server->server_poll_id);
		server->server_poll_id = -1;
	}
	if (server->server_fd != YETTY_SOCKET_INVALID) {
		shutdown(server->server_fd, SHUT_RDWR);
		yetty_platform_socket_close(server->server_fd);
		server->server_fd = YETTY_SOCKET_INVALID;
	}

	/* Close all client connections */
	for (size_t i = 0; i < server->client_count; i++) {
		if (server->client_poll_ids[i] >= 0) {
			server->event_loop->ops->stop_poll(
				server->event_loop, server->client_poll_ids[i]);
			server->event_loop->ops->destroy_poll(
				server->event_loop, server->client_poll_ids[i]);
			server->client_poll_ids[i] = -1;
		}
		if (server->clients[i] != YETTY_SOCKET_INVALID) {
			shutdown(server->clients[i], SHUT_RDWR);
			yetty_platform_socket_close(server->clients[i]);
			server->clients[i] = YETTY_SOCKET_INVALID;
		}
	}
	server->client_count = 0;

	return YETTY_OK_VOID();
}

int yetty_vnc_server_is_running(const struct yetty_vnc_server *server)
{
	return server ? server->running : 0;
}

int yetty_vnc_server_has_clients(const struct yetty_vnc_server *server)
{
	return server ? (server->client_count > 0) : 0;
}

int yetty_vnc_server_is_awaiting_ack(const struct yetty_vnc_server *server)
{
	return server ? server->awaiting_ack : 0;
}

int yetty_vnc_server_is_ready_for_frame(const struct yetty_vnc_server *server)
{
	if (!server)
		return 0;
	return server->capture_state == CAPTURE_IDLE && !server->awaiting_ack;
}

void yetty_vnc_server_force_full_frame(struct yetty_vnc_server *server)
{
	if (server)
		server->force_full_frame = 1;
}

void yetty_vnc_server_set_merge_rectangles(struct yetty_vnc_server *server,
					   int enable)
{
	if (server)
		server->merge_rectangles = enable;
}

int yetty_vnc_server_get_merge_rectangles(const struct yetty_vnc_server *server)
{
	return server ? server->merge_rectangles : 0;
}

void yetty_vnc_server_set_force_raw(struct yetty_vnc_server *server, int enable)
{
	if (server)
		server->force_raw = enable;
}

int yetty_vnc_server_get_force_raw(const struct yetty_vnc_server *server)
{
	return server ? server->force_raw : 0;
}

void yetty_vnc_server_set_jpeg_quality(struct yetty_vnc_server *server,
				       uint8_t quality)
{
	if (server)
		server->jpeg_quality = quality;
}

uint8_t yetty_vnc_server_get_jpeg_quality(const struct yetty_vnc_server *server)
{
	return server ? server->jpeg_quality : 80;
}

void yetty_vnc_server_set_always_full_frame(struct yetty_vnc_server *server,
					    int enable)
{
	if (server)
		server->always_full_frame = enable;
}

int yetty_vnc_server_get_always_full_frame(
	const struct yetty_vnc_server *server)
{
	return server ? server->always_full_frame : 0;
}

void yetty_vnc_server_set_use_h264(struct yetty_vnc_server *server, int enable)
{
	if (server)
		server->use_h264 = enable;
	/* H.264 not implemented yet */
}

int yetty_vnc_server_get_use_h264(const struct yetty_vnc_server *server)
{
	return server ? server->use_h264 : 0;
}

void yetty_vnc_server_force_h264_idr(struct yetty_vnc_server *server)
{
	(void)server;
	/* H.264 not implemented yet */
}

/* Accept new client connections */
static void handle_accept(struct yetty_vnc_server *server)
{
	while (1) {
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		int client_fd = accept(server->server_fd,
				       (struct sockaddr *)&client_addr,
				       &client_len);
		if (client_fd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			ywarn("VNC accept failed: %s", strerror(errno));
			break;
		}

		if (server->client_count >= MAX_CLIENTS) {
			ywarn("VNC max clients reached, rejecting");
			close(client_fd);
			continue;
		}

		/* Set socket options */
		int flag = 1;
		setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

		int flags = fcntl(client_fd, F_GETFL, 0);
		fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

		char client_ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
		yinfo("VNC client connected from %s", client_ip);

		/* Find empty slot */
		int slot = -1;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (server->clients[i] == YETTY_SOCKET_INVALID) {
				slot = i;
				break;
			}
		}

		if (slot < 0) {
			close(client_fd);
			continue;
		}

		server->clients[slot] = client_fd;
		server->client_count++;

		/* Register client socket with event loop */
		struct yetty_core_poll_id_result poll_res =
			server->event_loop->ops->create_poll(server->event_loop);
		if (!YETTY_IS_OK(poll_res)) {
			ywarn("VNC failed to create client poll");
			close(client_fd);
			server->clients[slot] = YETTY_SOCKET_INVALID;
			server->client_count--;
			continue;
		}
		server->client_poll_ids[slot] = poll_res.value;

		server->event_loop->ops->config_poll(server->event_loop,
						     server->client_poll_ids[slot],
						     client_fd);
		server->event_loop->ops->register_poll_listener(
			server->event_loop, server->client_poll_ids[slot],
			&server->listener);
		server->event_loop->ops->start_poll(server->event_loop,
						    server->client_poll_ids[slot],
						    YETTY_CORE_POLL_READABLE);

		/* Initialize input buffer */
		struct client_input_buffer *buf = &server->input_buffers[slot];
		buf->buffer_size = 0;
		buf->needed = sizeof(struct vnc_input_header);
		buf->reading_header = 1;

		server->force_full_frame = 1;
	}
}

/* Event loop callback for server and client sockets */
static struct yetty_core_int_result vnc_server_on_event(
	struct yetty_core_event_listener *listener,
	const struct yetty_core_event *event)
{
	struct yetty_vnc_server *server =
		(struct yetty_vnc_server *)((char *)listener -
			offsetof(struct yetty_vnc_server, listener));

	if (event->type != YETTY_EVENT_POLL_READABLE &&
	    event->type != YETTY_EVENT_POLL_WRITABLE)
		return YETTY_OK(yetty_core_int, 0);

	int fd = event->poll.fd;

	/* Server socket - accept new connections */
	if (fd == server->server_fd) {
		ydebug("VNC server socket readable, accepting");
		handle_accept(server);
		return YETTY_OK(yetty_core_int, 1);
	}

	/* Client socket - find which client and read input */
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (server->clients[i] == fd) {
			ydebug("VNC client %d socket readable", i);
			poll_client_input(server, i);
			return YETTY_OK(yetty_core_int, 1);
		}
	}

	return YETTY_OK(yetty_core_int, 0);
}

/* Send data to a client */
static struct yetty_core_void_result send_to_client(struct yetty_vnc_server *server,
						    int client_idx,
						    const void *data, size_t size)
{
	int fd = server->clients[client_idx];
	const uint8_t *ptr = data;
	size_t remaining = size;

	while (remaining > 0) {
		ssize_t sent = send(fd, ptr, remaining, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (sent > 0) {
			ptr += sent;
			remaining -= sent;
			continue;
		}
		if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			/* Would need async handling - for now just return error */
			return YETTY_ERR(yetty_core_void, "would block");
		}
		return YETTY_ERR(yetty_core_void, "send failed");
	}

	return YETTY_OK_VOID();
}

/* Create GPU diff pipeline */
static struct yetty_core_void_result create_diff_pipeline(struct yetty_vnc_server *server)
{
	/* Create shader module */
	WGPUShaderSourceWGSL wgsl_desc = {0};
	wgsl_desc.chain.sType = WGPUSType_ShaderSourceWGSL;
	wgsl_desc.code = (WGPUStringView){.data = DIFF_SHADER, .length = strlen(DIFF_SHADER)};

	WGPUShaderModuleDescriptor shader_desc = {0};
	shader_desc.nextInChain = (WGPUChainedStruct *)&wgsl_desc;

	WGPUShaderModule shader = wgpuDeviceCreateShaderModule(server->device, &shader_desc);
	if (!shader)
		return YETTY_ERR(yetty_core_void, "failed to create diff shader");

	/* Bind group layout */
	WGPUBindGroupLayoutEntry entries[3] = {0};
	entries[0].binding = 0;
	entries[0].visibility = WGPUShaderStage_Compute;
	entries[0].texture.sampleType = WGPUTextureSampleType_Float;
	entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;

	entries[1].binding = 1;
	entries[1].visibility = WGPUShaderStage_Compute;
	entries[1].texture.sampleType = WGPUTextureSampleType_Float;
	entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;

	entries[2].binding = 2;
	entries[2].visibility = WGPUShaderStage_Compute;
	entries[2].buffer.type = WGPUBufferBindingType_Storage;

	WGPUBindGroupLayoutDescriptor bgl_desc = {0};
	bgl_desc.entryCount = 3;
	bgl_desc.entries = entries;
	server->diff_bind_group_layout = wgpuDeviceCreateBindGroupLayout(server->device, &bgl_desc);

	/* Pipeline layout */
	WGPUPipelineLayoutDescriptor pl_desc = {0};
	pl_desc.bindGroupLayoutCount = 1;
	pl_desc.bindGroupLayouts = &server->diff_bind_group_layout;
	WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(server->device, &pl_desc);

	/* Compute pipeline */
	WGPUComputePipelineDescriptor cp_desc = {0};
	cp_desc.layout = layout;
	cp_desc.compute.module = shader;
	cp_desc.compute.entryPoint = (WGPUStringView){.data = "main", .length = 4};

	server->diff_pipeline = wgpuDeviceCreateComputePipeline(server->device, &cp_desc);

	wgpuShaderModuleRelease(shader);
	wgpuPipelineLayoutRelease(layout);

	if (!server->diff_pipeline)
		return YETTY_ERR(yetty_core_void, "failed to create diff pipeline");

	return YETTY_OK_VOID();
}

/* Ensure GPU resources exist for given dimensions */
static struct yetty_core_void_result ensure_resources(struct yetty_vnc_server *server,
						      uint32_t width, uint32_t height)
{
	if (server->last_width == width && server->last_height == height && server->prev_texture)
		return YETTY_OK_VOID();

	/* Release old resources */
	if (server->prev_texture) {
		wgpuTextureRelease(server->prev_texture);
		server->prev_texture = NULL;
	}
	if (server->dirty_flags_buffer) {
		wgpuBufferRelease(server->dirty_flags_buffer);
		server->dirty_flags_buffer = NULL;
	}
	if (server->dirty_flags_readback) {
		wgpuBufferUnmap(server->dirty_flags_readback);
		wgpuBufferRelease(server->dirty_flags_readback);
		server->dirty_flags_readback = NULL;
	}
	if (server->diff_bind_group) {
		wgpuBindGroupRelease(server->diff_bind_group);
		server->diff_bind_group = NULL;
	}
	if (server->tile_readback_buffer) {
		wgpuBufferUnmap(server->tile_readback_buffer);
		wgpuBufferRelease(server->tile_readback_buffer);
		server->tile_readback_buffer = NULL;
		server->tile_readback_buffer_size = 0;
	}

	server->last_width = width;
	server->last_height = height;
	server->tiles_x = vnc_tiles_x(width);
	server->tiles_y = vnc_tiles_y(height);

	uint32_t num_tiles = server->tiles_x * server->tiles_y;

	/* Allocate dirty tiles array */
	free(server->dirty_tiles);
	server->dirty_tiles = calloc(num_tiles, sizeof(int));
	if (!server->dirty_tiles)
		return YETTY_ERR(yetty_core_void, "failed to allocate dirty tiles");

	/* Create previous frame texture */
	WGPUTextureDescriptor tex_desc = {0};
	tex_desc.size = (WGPUExtent3D){width, height, 1};
	tex_desc.format = WGPUTextureFormat_BGRA8Unorm;
	tex_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
	tex_desc.mipLevelCount = 1;
	tex_desc.sampleCount = 1;
	tex_desc.dimension = WGPUTextureDimension_2D;
	server->prev_texture = wgpuDeviceCreateTexture(server->device, &tex_desc);
	if (!server->prev_texture)
		return YETTY_ERR(yetty_core_void, "failed to create prev texture");

	/* Create dirty flags buffer */
	WGPUBufferDescriptor buf_desc = {0};
	buf_desc.size = num_tiles * sizeof(uint32_t);
	buf_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
	server->dirty_flags_buffer = wgpuDeviceCreateBuffer(server->device, &buf_desc);
	if (!server->dirty_flags_buffer)
		return YETTY_ERR(yetty_core_void, "failed to create dirty flags buffer");

	/* Create readback buffer */
	buf_desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
	server->dirty_flags_readback = wgpuDeviceCreateBuffer(server->device, &buf_desc);
	if (!server->dirty_flags_readback)
		return YETTY_ERR(yetty_core_void, "failed to create readback buffer");

	/* Create diff pipeline if needed */
	if (!server->diff_pipeline) {
		struct yetty_core_void_result res = create_diff_pipeline(server);
		if (!YETTY_IS_OK(res))
			return res;
	}

	ydebug("VNC resources: %ux%u, %u tiles", width, height, num_tiles);
	return YETTY_OK_VOID();
}

/* Encode a single tile */
static struct yetty_core_void_result encode_tile(struct yetty_vnc_server *server,
						 uint16_t tx, uint16_t ty,
						 uint8_t **out_data, size_t *out_size,
						 uint8_t *out_encoding)
{
	const uint8_t *pixels = server->cpu_pixels ? server->cpu_pixels : server->gpu_readback_pixels;
	if (!pixels)
		return YETTY_ERR(yetty_core_void, "no pixels");

	uint32_t start_x = tx * VNC_TILE_SIZE;
	uint32_t start_y = ty * VNC_TILE_SIZE;
	uint32_t tile_w = VNC_TILE_SIZE;
	uint32_t tile_h = VNC_TILE_SIZE;

	if (start_x + tile_w > server->last_width)
		tile_w = server->last_width - start_x;
	if (start_y + tile_h > server->last_height)
		tile_h = server->last_height - start_y;

	/* Extract tile pixels */
	uint8_t *tile_pixels = calloc(VNC_TILE_SIZE * VNC_TILE_SIZE * 4, 1);
	if (!tile_pixels)
		return YETTY_ERR(yetty_core_void, "alloc failed");

	uint32_t src_stride = server->last_width * 4;
	for (uint32_t y = 0; y < tile_h; y++) {
		const uint8_t *src = pixels + (start_y + y) * src_stride + start_x * 4;
		uint8_t *dst = tile_pixels + y * VNC_TILE_SIZE * 4;
		memcpy(dst, src, tile_w * 4);
	}

	uint32_t raw_size = VNC_TILE_SIZE * VNC_TILE_SIZE * 4;

	if (server->force_raw) {
		*out_data = tile_pixels;
		*out_size = raw_size;
		*out_encoding = VNC_ENCODING_RAW;
		return YETTY_OK_VOID();
	}

	/* Try JPEG compression */
	unsigned char *jpeg_buf = NULL;
	unsigned long jpeg_size = 0;
	int result = tjCompress2(
		server->jpeg_compressor,
		tile_pixels,
		VNC_TILE_SIZE, 0, VNC_TILE_SIZE,
		TJPF_BGRA,
		&jpeg_buf, &jpeg_size,
		TJSAMP_420, server->jpeg_quality,
		TJFLAG_FASTDCT);

	if (result == 0 && jpeg_size < raw_size * 0.8) {
		free(tile_pixels);
		*out_data = malloc(jpeg_size);
		if (!*out_data) {
			tjFree(jpeg_buf);
			return YETTY_ERR(yetty_core_void, "alloc failed");
		}
		memcpy(*out_data, jpeg_buf, jpeg_size);
		tjFree(jpeg_buf);
		*out_size = jpeg_size;
		*out_encoding = VNC_ENCODING_JPEG;
	} else {
		if (jpeg_buf)
			tjFree(jpeg_buf);
		*out_data = tile_pixels;
		*out_size = raw_size;
		*out_encoding = VNC_ENCODING_RAW;
	}

	return YETTY_OK_VOID();
}

/* GPU work done callback */
static void on_gpu_work_done(WGPUQueueWorkDoneStatus status, WGPUStringView message, void *userdata1, void *userdata2)
{
	(void)status;
	(void)message;
	(void)userdata2;
	struct yetty_vnc_server *server = userdata1;
	server->gpu_work_done = 1;
}

/* Buffer map callback */
static void on_buffer_mapped(WGPUMapAsyncStatus status, WGPUStringView message, void *userdata1, void *userdata2)
{
	(void)message;
	(void)userdata2;
	struct yetty_vnc_server *server = userdata1;
	server->map_status = status;
	server->gpu_work_done = 1;
}

struct yetty_core_void_result
yetty_vnc_server_send_frame(struct yetty_vnc_server *server, WGPUTexture texture,
			    const uint8_t *cpu_pixels, uint32_t width,
			    uint32_t height)
{
	ydebug("vnc_server_send_frame: server=%p clients=%zu texture=%p %ux%u",
	       (void *)server, server ? server->client_count : 0,
	       (void *)texture, width, height);

	if (!server || server->client_count == 0)
		return YETTY_OK_VOID();

	/* Handle resize */
	if (width != server->last_width || height != server->last_height) {
		server->capture_state = CAPTURE_IDLE;
		server->gpu_work_done = 1;
	}

	/* Flow control */
	if (server->awaiting_ack)
		return YETTY_OK_VOID();

	server->cpu_pixels = cpu_pixels;
	server->cpu_pixels_size = width * height * 4;

	if (!cpu_pixels) {
		if (server->gpu_readback_pixels_size < width * height * 4) {
			free(server->gpu_readback_pixels);
			server->gpu_readback_pixels = malloc(width * height * 4);
			server->gpu_readback_pixels_size = width * height * 4;
		}
	}

	/* State machine */
	switch (server->capture_state) {
	case CAPTURE_IDLE: {
		struct yetty_core_void_result res = ensure_resources(server, width, height);
		if (!YETTY_IS_OK(res))
			return res;

		server->frames_since_full_refresh++;
		if (server->frames_since_full_refresh >= FULL_REFRESH_INTERVAL) {
			server->force_full_frame = 1;
			server->frames_since_full_refresh = 0;
		}

		if (server->always_full_frame)
			server->force_full_frame = 1;

		WGPUExtent3D extent = {width, height, 1};

		if (server->force_full_frame) {
			/* Mark all dirty */
			for (uint32_t i = 0; i < server->tiles_x * server->tiles_y; i++)
				server->dirty_tiles[i] = 1;
			server->force_full_frame = 0;
			server->pending_texture = texture;

			/* Copy current to prev */
			WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(server->device, NULL);
			WGPUTexelCopyTextureInfo src = {.texture = texture};
			WGPUTexelCopyTextureInfo dst = {.texture = server->prev_texture};
			wgpuCommandEncoderCopyTextureToTexture(enc, &src, &dst, &extent);
			WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
			wgpuQueueSubmit(server->queue, 1, &cmd);
			wgpuCommandBufferRelease(cmd);
			wgpuCommandEncoderRelease(enc);

			server->capture_state = CAPTURE_READY_TO_SEND;
			return YETTY_OK_VOID();
		}

		server->pending_texture = texture;

		/* Create bind group */
		if (server->diff_bind_group)
			wgpuBindGroupRelease(server->diff_bind_group);

		WGPUTextureView curr_view = wgpuTextureCreateView(texture, NULL);
		WGPUTextureView prev_view = wgpuTextureCreateView(server->prev_texture, NULL);

		WGPUBindGroupEntry entries[3] = {0};
		entries[0].binding = 0;
		entries[0].textureView = curr_view;
		entries[1].binding = 1;
		entries[1].textureView = prev_view;
		entries[2].binding = 2;
		entries[2].buffer = server->dirty_flags_buffer;
		entries[2].size = server->tiles_x * server->tiles_y * sizeof(uint32_t);

		WGPUBindGroupDescriptor bg_desc = {0};
		bg_desc.layout = server->diff_bind_group_layout;
		bg_desc.entryCount = 3;
		bg_desc.entries = entries;
		server->diff_bind_group = wgpuDeviceCreateBindGroup(server->device, &bg_desc);

		wgpuTextureViewRelease(curr_view);
		wgpuTextureViewRelease(prev_view);

		/* Run compute + copy */
		WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(server->device, NULL);

		wgpuCommandEncoderClearBuffer(enc, server->dirty_flags_buffer, 0,
					      server->tiles_x * server->tiles_y * sizeof(uint32_t));

		WGPUComputePassEncoder cpass = wgpuCommandEncoderBeginComputePass(enc, NULL);
		wgpuComputePassEncoderSetPipeline(cpass, server->diff_pipeline);
		wgpuComputePassEncoderSetBindGroup(cpass, 0, server->diff_bind_group, 0, NULL);
		wgpuComputePassEncoderDispatchWorkgroups(cpass, server->tiles_x, server->tiles_y, 1);
		wgpuComputePassEncoderEnd(cpass);
		wgpuComputePassEncoderRelease(cpass);

		wgpuCommandEncoderCopyBufferToBuffer(enc, server->dirty_flags_buffer, 0,
						     server->dirty_flags_readback, 0,
						     server->tiles_x * server->tiles_y * sizeof(uint32_t));

		WGPUTexelCopyTextureInfo src = {.texture = texture};
		WGPUTexelCopyTextureInfo dst = {.texture = server->prev_texture};
		wgpuCommandEncoderCopyTextureToTexture(enc, &src, &dst, &extent);

		WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
		wgpuQueueSubmit(server->queue, 1, &cmd);
		wgpuCommandBufferRelease(cmd);
		wgpuCommandEncoderRelease(enc);

		server->gpu_work_done = 0;
		WGPUQueueWorkDoneCallbackInfo cb = {0};
		cb.mode = WGPUCallbackMode_AllowSpontaneous;
		cb.callback = on_gpu_work_done;
		cb.userdata1 = server;
		wgpuQueueOnSubmittedWorkDone(server->queue, cb);

		server->capture_state = CAPTURE_WAITING_COMPUTE;
		return YETTY_OK_VOID();
	}

	case CAPTURE_WAITING_COMPUTE:
		if (!server->gpu_work_done)
			return YETTY_OK_VOID();

		server->gpu_work_done = 0;
		{
			WGPUBufferMapCallbackInfo cb = {0};
			cb.mode = WGPUCallbackMode_AllowSpontaneous;
			cb.callback = on_buffer_mapped;
			cb.userdata1 = server;
			wgpuBufferMapAsync(server->dirty_flags_readback, WGPUMapMode_Read, 0,
					   server->tiles_x * server->tiles_y * sizeof(uint32_t), cb);
		}
		server->capture_state = CAPTURE_WAITING_MAP;
		return YETTY_OK_VOID();

	case CAPTURE_WAITING_MAP:
		if (!server->gpu_work_done)
			return YETTY_OK_VOID();

		if (server->map_status != WGPUMapAsyncStatus_Success) {
			server->capture_state = CAPTURE_IDLE;
			return YETTY_OK_VOID();
		}

		{
			const uint32_t *flags = wgpuBufferGetConstMappedRange(
				server->dirty_flags_readback, 0,
				server->tiles_x * server->tiles_y * sizeof(uint32_t));
			for (uint32_t i = 0; i < server->tiles_x * server->tiles_y; i++)
				server->dirty_tiles[i] = (flags[i] != 0);
			wgpuBufferUnmap(server->dirty_flags_readback);
		}
		server->capture_state = CAPTURE_READY_TO_SEND;
		/* Fall through */

	case CAPTURE_READY_TO_SEND:
		if (!server->cpu_pixels && server->gpu_readback_pixels) {
			/* Need GPU readback */
			uint32_t bpp = 4;
			uint32_t aligned_row = (width * bpp + 255) & ~255;
			uint32_t buf_size = aligned_row * height;

			if (!server->tile_readback_buffer || server->tile_readback_buffer_size != buf_size) {
				if (server->tile_readback_buffer)
					wgpuBufferRelease(server->tile_readback_buffer);
				WGPUBufferDescriptor buf_desc = {0};
				buf_desc.size = buf_size;
				buf_desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
				server->tile_readback_buffer = wgpuDeviceCreateBuffer(server->device, &buf_desc);
				server->tile_readback_buffer_size = buf_size;
			}

			WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(server->device, NULL);
			WGPUTexelCopyTextureInfo src = {.texture = server->pending_texture ? server->pending_texture : texture};
			WGPUTexelCopyBufferInfo dst = {
				.buffer = server->tile_readback_buffer,
				.layout = {.bytesPerRow = aligned_row, .rowsPerImage = height}
			};
			WGPUExtent3D copy_size = {width, height, 1};
			wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &copy_size);
			WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
			wgpuQueueSubmit(server->queue, 1, &cmd);
			wgpuCommandBufferRelease(cmd);
			wgpuCommandEncoderRelease(enc);

			server->gpu_work_done = 0;
			WGPUBufferMapCallbackInfo cb = {0};
			cb.mode = WGPUCallbackMode_AllowSpontaneous;
			cb.callback = on_buffer_mapped;
			cb.userdata1 = server;
			wgpuBufferMapAsync(server->tile_readback_buffer, WGPUMapMode_Read, 0, buf_size, cb);

			server->capture_state = CAPTURE_WAITING_TILE_READBACK;
			return YETTY_OK_VOID();
		}
		server->capture_state = CAPTURE_IDLE;
		break;

	case CAPTURE_WAITING_TILE_READBACK:
		if (!server->gpu_work_done)
			return YETTY_OK_VOID();

		if (server->map_status != WGPUMapAsyncStatus_Success) {
			server->capture_state = CAPTURE_IDLE;
			return YETTY_OK_VOID();
		}

		{
			uint32_t bpp = 4;
			uint32_t aligned_row = (width * bpp + 255) & ~255;
			uint32_t buf_size = aligned_row * height;

			const uint8_t *mapped = wgpuBufferGetConstMappedRange(
				server->tile_readback_buffer, 0, buf_size);
			uint32_t unaligned_row = width * bpp;
			for (uint32_t y = 0; y < height; y++) {
				memcpy(server->gpu_readback_pixels + y * unaligned_row,
				       mapped + y * aligned_row, unaligned_row);
			}
			wgpuBufferUnmap(server->tile_readback_buffer);
		}
		server->capture_state = CAPTURE_IDLE;
		break;
	}

	/* Count dirty tiles */
	uint16_t num_dirty = 0;
	for (uint32_t i = 0; i < server->tiles_x * server->tiles_y; i++) {
		if (server->dirty_tiles[i])
			num_dirty++;
	}

	if (num_dirty == 0)
		return YETTY_OK_VOID();

	server->awaiting_ack = 1;

	/* Build frame */
	size_t frame_capacity = 64 * 1024;
	uint8_t *frame_data = malloc(frame_capacity);
	size_t frame_size = 0;

	uint16_t total_tiles = server->tiles_x * server->tiles_y;
	int use_full_frame = !server->force_raw && (num_dirty > total_tiles / 2);

	if (use_full_frame) {
		/* Full frame JPEG */
		const uint8_t *pixels = server->cpu_pixels ? server->cpu_pixels : server->gpu_readback_pixels;
		if (!pixels) {
			free(frame_data);
			server->awaiting_ack = 0;
			return YETTY_ERR(yetty_core_void, "no pixels");
		}

		unsigned char *jpeg_buf = NULL;
		unsigned long jpeg_size = 0;
		int result = tjCompress2(server->jpeg_compressor, pixels, width, 0, height,
					 TJPF_BGRA, &jpeg_buf, &jpeg_size,
					 TJSAMP_420, server->jpeg_quality, TJFLAG_FASTDCT);
		if (result != 0) {
			if (jpeg_buf) tjFree(jpeg_buf);
			free(frame_data);
			server->awaiting_ack = 0;
			return YETTY_ERR(yetty_core_void, "JPEG failed");
		}

		struct vnc_frame_header fh = {
			.magic = VNC_FRAME_MAGIC,
			.width = width,
			.height = height,
			.tile_size = VNC_TILE_SIZE,
			.num_tiles = 1
		};
		memcpy(frame_data + frame_size, &fh, sizeof(fh));
		frame_size += sizeof(fh);

		struct vnc_tile_header th = {
			.tile_x = 0,
			.tile_y = 0,
			.encoding = VNC_ENCODING_FULL_FRAME,
			.data_size = jpeg_size
		};
		memcpy(frame_data + frame_size, &th, sizeof(th));
		frame_size += sizeof(th);

		if (frame_size + jpeg_size > frame_capacity) {
			frame_capacity = frame_size + jpeg_size;
			frame_data = realloc(frame_data, frame_capacity);
		}
		memcpy(frame_data + frame_size, jpeg_buf, jpeg_size);
		frame_size += jpeg_size;

		tjFree(jpeg_buf);
		server->current_stats.full_updates++;
	} else {
		/* Tile mode */
		struct vnc_frame_header fh = {
			.magic = VNC_FRAME_MAGIC,
			.width = width,
			.height = height,
			.tile_size = VNC_TILE_SIZE,
			.num_tiles = num_dirty
		};
		memcpy(frame_data + frame_size, &fh, sizeof(fh));
		frame_size += sizeof(fh);

		for (uint16_t ty = 0; ty < server->tiles_y; ty++) {
			for (uint16_t tx = 0; tx < server->tiles_x; tx++) {
				if (!server->dirty_tiles[ty * server->tiles_x + tx])
					continue;

				uint8_t *tile_data;
				size_t tile_size;
				uint8_t encoding;

				struct yetty_core_void_result res = encode_tile(server, tx, ty,
									       &tile_data, &tile_size, &encoding);
				if (!YETTY_IS_OK(res))
					continue;

				struct vnc_tile_header th = {
					.tile_x = tx,
					.tile_y = ty,
					.encoding = encoding,
					.data_size = tile_size
				};

				size_t needed = frame_size + sizeof(th) + tile_size;
				if (needed > frame_capacity) {
					frame_capacity = needed * 2;
					frame_data = realloc(frame_data, frame_capacity);
				}

				memcpy(frame_data + frame_size, &th, sizeof(th));
				frame_size += sizeof(th);
				memcpy(frame_data + frame_size, tile_data, tile_size);
				frame_size += tile_size;

				free(tile_data);

				server->current_stats.tiles_sent++;
				if (encoding == VNC_ENCODING_JPEG)
					server->current_stats.tiles_jpeg++;
				else
					server->current_stats.tiles_raw++;
			}
		}
	}

	/* Send to all clients */
	ydebug("vnc_server: sending frame size=%zu to %zu clients, num_dirty=%u",
	       frame_size, server->client_count, num_dirty);
	for (size_t i = 0; i < MAX_CLIENTS; i++) {
		if (server->clients[i] == YETTY_SOCKET_INVALID)
			continue;
		ydebug("vnc_server: sending to client %zu", i);
		struct yetty_core_void_result res = send_to_client(server, i, frame_data, frame_size);
		if (!YETTY_IS_OK(res)) {
			/* Remove dead client */
			ywarn("vnc_server: client %zu send failed, removing", i);
			yetty_platform_socket_close(server->clients[i]);
			server->clients[i] = YETTY_SOCKET_INVALID;
			server->client_count--;
		}
	}

	free(frame_data);
	server->current_stats.bytes_sent += frame_size;
	server->current_stats.frames++;

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_vnc_server_send_frame_gpu(struct yetty_vnc_server *server,
				WGPUTexture texture, uint32_t width,
				uint32_t height)
{
	return yetty_vnc_server_send_frame(server, texture, NULL, width, height);
}

int yetty_vnc_server_has_pending_input(const struct yetty_vnc_server *server)
{
	(void)server;
	return 0;
}

/* Poll and dispatch input from clients */
static void poll_client_input(struct yetty_vnc_server *server, int client_idx)
{
	int fd = server->clients[client_idx];
	struct client_input_buffer *buf = &server->input_buffers[client_idx];

	while (1) {
		size_t to_read = buf->needed - buf->buffer_size;
		if (to_read == 0)
			break;

		/* Ensure buffer capacity */
		if (buf->buffer_size + to_read > buf->buffer_capacity) {
			buf->buffer_capacity = buf->buffer_size + to_read + 256;
			buf->buffer = realloc(buf->buffer, buf->buffer_capacity);
		}

		ssize_t n = recv(fd, buf->buffer + buf->buffer_size, to_read, MSG_DONTWAIT);
		if (n <= 0) {
			if (n == 0) {
				/* Disconnected */
				yetty_platform_socket_close(fd);
				server->clients[client_idx] = YETTY_SOCKET_INVALID;
				server->client_count--;
				return;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			break;
		}

		buf->buffer_size += n;

		if (buf->buffer_size >= buf->needed) {
			if (buf->reading_header) {
				memcpy(&buf->header, buf->buffer, sizeof(struct vnc_input_header));
				buf->buffer_size = 0;
				buf->needed = buf->header.data_size;
				buf->reading_header = 0;

				if (buf->needed == 0) {
					dispatch_input(server, &buf->header, NULL);
					buf->needed = sizeof(struct vnc_input_header);
					buf->reading_header = 1;
				}
			} else {
				dispatch_input(server, &buf->header, buf->buffer);
				buf->buffer_size = 0;
				buf->needed = sizeof(struct vnc_input_header);
				buf->reading_header = 1;
			}
		}
	}
}

static void dispatch_input(struct yetty_vnc_server *server,
			   const struct vnc_input_header *hdr,
			   const uint8_t *data)
{
	switch (hdr->type) {
	case VNC_INPUT_MOUSE_MOVE:
		if (data && hdr->data_size >= sizeof(struct vnc_mouse_move_event) && server->on_mouse_move_fn) {
			const struct vnc_mouse_move_event *m = (const struct vnc_mouse_move_event *)data;
			server->on_mouse_move_fn(m->x, m->y, m->mods, server->on_mouse_move_userdata);
		}
		break;

	case VNC_INPUT_MOUSE_BUTTON:
		if (data && hdr->data_size >= sizeof(struct vnc_mouse_button_event) && server->on_mouse_button_fn) {
			const struct vnc_mouse_button_event *m = (const struct vnc_mouse_button_event *)data;
			server->on_mouse_button_fn(m->x, m->y, m->button, m->pressed, m->mods, server->on_mouse_button_userdata);
		}
		break;

	case VNC_INPUT_MOUSE_SCROLL:
		if (data && hdr->data_size >= sizeof(struct vnc_mouse_scroll_event) && server->on_mouse_scroll_fn) {
			const struct vnc_mouse_scroll_event *m = (const struct vnc_mouse_scroll_event *)data;
			server->on_mouse_scroll_fn(m->x, m->y, m->delta_x, m->delta_y, m->mods, server->on_mouse_scroll_userdata);
		}
		break;

	case VNC_INPUT_KEY_DOWN:
		if (data && hdr->data_size >= sizeof(struct vnc_key_event) && server->on_key_down_fn) {
			const struct vnc_key_event *k = (const struct vnc_key_event *)data;
			server->on_key_down_fn(k->keycode, k->scancode, k->mods, server->on_key_down_userdata);
		}
		break;

	case VNC_INPUT_KEY_UP:
		if (data && hdr->data_size >= sizeof(struct vnc_key_event) && server->on_key_up_fn) {
			const struct vnc_key_event *k = (const struct vnc_key_event *)data;
			server->on_key_up_fn(k->keycode, k->scancode, k->mods, server->on_key_up_userdata);
		}
		break;

	case VNC_INPUT_TEXT:
		if (data && hdr->data_size > 0 && server->on_text_input_fn) {
			server->on_text_input_fn((const char *)data, hdr->data_size, server->on_text_input_userdata);
		}
		break;

	case VNC_INPUT_RESIZE:
		if (data && hdr->data_size >= sizeof(struct vnc_resize_event) && server->on_resize_fn) {
			const struct vnc_resize_event *r = (const struct vnc_resize_event *)data;
			server->on_resize_fn(r->width, r->height, server->on_resize_userdata);
		}
		break;

	case VNC_INPUT_CELL_SIZE:
		if (data && hdr->data_size >= sizeof(struct vnc_cell_size_event) && server->on_cell_size_fn) {
			const struct vnc_cell_size_event *c = (const struct vnc_cell_size_event *)data;
			server->on_cell_size_fn(c->cell_height, server->on_cell_size_userdata);
		}
		break;

	case VNC_INPUT_CHAR_WITH_MODS:
		if (data && hdr->data_size >= sizeof(struct vnc_char_with_mods_event) && server->on_char_with_mods_fn) {
			const struct vnc_char_with_mods_event *c = (const struct vnc_char_with_mods_event *)data;
			server->on_char_with_mods_fn(c->codepoint, c->mods, server->on_char_with_mods_userdata);
		}
		break;

	case VNC_INPUT_FRAME_ACK:
		server->awaiting_ack = 0;
		break;

	case VNC_INPUT_COMPRESSION_CONFIG:
		if (data && hdr->data_size >= sizeof(struct vnc_compression_config_event)) {
			const struct vnc_compression_config_event *c = (const struct vnc_compression_config_event *)data;
			server->force_raw = (c->force_raw != 0);
			if (c->quality > 0 && c->quality <= 100)
				server->jpeg_quality = c->quality;
			server->always_full_frame = (c->always_full != 0);
		}
		break;
	}

	if (server->on_input_received_fn)
		server->on_input_received_fn(server->on_input_received_userdata);
}

struct yetty_core_void_result
yetty_vnc_server_process_input(struct yetty_vnc_server *server)
{
	if (!server)
		return YETTY_OK_VOID();

	/* Accept new connections */
	if (server->server_fd != YETTY_SOCKET_INVALID)
		handle_accept(server);

	/* Poll all clients */
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (server->clients[i] != YETTY_SOCKET_INVALID)
			poll_client_input(server, i);
	}

	return YETTY_OK_VOID();
}

struct yetty_vnc_server_stats
yetty_vnc_server_get_stats(const struct yetty_vnc_server *server)
{
	if (!server) {
		struct yetty_vnc_server_stats empty = {0};
		return empty;
	}
	return server->stats;
}

/* Callback setters */
void yetty_vnc_server_set_on_mouse_move(struct yetty_vnc_server *server,
					yetty_vnc_on_mouse_move_fn callback,
					void *userdata)
{
	if (!server) return;
	server->on_mouse_move_fn = callback;
	server->on_mouse_move_userdata = userdata;
}

void yetty_vnc_server_set_on_mouse_button(struct yetty_vnc_server *server,
					  yetty_vnc_on_mouse_button_fn callback,
					  void *userdata)
{
	if (!server) return;
	server->on_mouse_button_fn = callback;
	server->on_mouse_button_userdata = userdata;
}

void yetty_vnc_server_set_on_mouse_scroll(struct yetty_vnc_server *server,
					  yetty_vnc_on_mouse_scroll_fn callback,
					  void *userdata)
{
	if (!server) return;
	server->on_mouse_scroll_fn = callback;
	server->on_mouse_scroll_userdata = userdata;
}

void yetty_vnc_server_set_on_key_down(struct yetty_vnc_server *server,
				      yetty_vnc_on_key_down_fn callback,
				      void *userdata)
{
	if (!server) return;
	server->on_key_down_fn = callback;
	server->on_key_down_userdata = userdata;
}

void yetty_vnc_server_set_on_key_up(struct yetty_vnc_server *server,
				    yetty_vnc_on_key_up_fn callback,
				    void *userdata)
{
	if (!server) return;
	server->on_key_up_fn = callback;
	server->on_key_up_userdata = userdata;
}

void yetty_vnc_server_set_on_text_input(struct yetty_vnc_server *server,
					yetty_vnc_on_text_input_fn callback,
					void *userdata)
{
	if (!server) return;
	server->on_text_input_fn = callback;
	server->on_text_input_userdata = userdata;
}

void yetty_vnc_server_set_on_resize(struct yetty_vnc_server *server,
				    yetty_vnc_on_resize_fn callback,
				    void *userdata)
{
	if (!server) return;
	server->on_resize_fn = callback;
	server->on_resize_userdata = userdata;
}

void yetty_vnc_server_set_on_cell_size(struct yetty_vnc_server *server,
				       yetty_vnc_on_cell_size_fn callback,
				       void *userdata)
{
	if (!server) return;
	server->on_cell_size_fn = callback;
	server->on_cell_size_userdata = userdata;
}

void yetty_vnc_server_set_on_char_with_mods(struct yetty_vnc_server *server,
					    yetty_vnc_on_char_with_mods_fn callback,
					    void *userdata)
{
	if (!server) return;
	server->on_char_with_mods_fn = callback;
	server->on_char_with_mods_userdata = userdata;
}

void yetty_vnc_server_set_on_input_received(struct yetty_vnc_server *server,
					    yetty_vnc_on_input_received_fn callback,
					    void *userdata)
{
	if (!server) return;
	server->on_input_received_fn = callback;
	server->on_input_received_userdata = userdata;
}
