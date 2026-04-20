#include <yetty/yvnc/vnc-client.h>
#include <yetty/platform/socket.h>
#include <yetty/ytrace.h>
#include "protocol.h"

#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <turbojpeg.h>
#include <time.h>

/* Recv state machine */
enum recv_state {
	RECV_FRAME_HEADER,
	RECV_TILE_HEADER,
	RECV_TILE_DATA,
	RECV_RECT_HEADER,
	RECV_RECT_DATA
};

/* Fullscreen quad shader */
static const char *QUAD_SHADER = "\
struct VertexOutput {\n\
    @builtin(position) position: vec4<f32>,\n\
    @location(0) uv: vec2<f32>,\n\
};\n\
\n\
@vertex\n\
fn vs_main(@builtin(vertex_index) idx: u32) -> VertexOutput {\n\
    var positions = array<vec2<f32>, 6>(\n\
        vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0),\n\
        vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(1.0, 1.0)\n\
    );\n\
    var uvs = array<vec2<f32>, 6>(\n\
        vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(0.0, 0.0),\n\
        vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(1.0, 0.0)\n\
    );\n\
    var out: VertexOutput;\n\
    out.position = vec4(positions[idx], 0.0, 1.0);\n\
    out.uv = uvs[idx];\n\
    return out;\n\
}\n\
\n\
@group(0) @binding(0) var frame_texture: texture_2d<f32>;\n\
@group(0) @binding(1) var frame_sampler: sampler;\n\
\n\
@fragment\n\
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {\n\
    let tex = textureSample(frame_texture, frame_sampler, in.uv);\n\
    if (tex.a < 0.01) {\n\
        return vec4<f32>(1.0, 0.0, 1.0, 1.0);\n\
    }\n\
    return tex;\n\
}\n";

struct yetty_vnc_client {
	WGPUDevice device;
	WGPUQueue queue;
	WGPUTextureFormat surface_format;

	/* Event loop for async I/O */
	struct yetty_core_event_loop *event_loop;
	struct yetty_core_event_listener listener;
	yetty_core_poll_id poll_id;

	/* Network */
	yetty_socket_fd socket;
	int connected;
	int connecting;
	int wants_reconnect;

	/* Reconnection */
	char *reconnect_host;
	uint16_t reconnect_port;

	/* Frame state */
	uint16_t width;
	uint16_t height;

	/* Recv state machine */
	enum recv_state recv_state;
	uint8_t *recv_buffer;
	size_t recv_buffer_capacity;
	size_t recv_offset;
	size_t recv_needed;

	/* Current frame/tile/rect being received */
	struct vnc_frame_header current_frame;
	struct vnc_tile_header current_tile;
	struct vnc_rect_header current_rect;
	uint16_t tiles_received;

	/* Send queue for async writes */
	uint8_t *send_queue;
	size_t send_queue_size;
	size_t send_queue_capacity;
	size_t send_offset;

	/* GPU resources */
	WGPUTexture texture;
	uint16_t texture_width;
	uint16_t texture_height;
	WGPUTextureView texture_view;
	WGPUSampler sampler;
	WGPUBindGroup bind_group;
	WGPUBindGroupLayout bind_group_layout;
	WGPURenderPipeline pipeline;

	/* JPEG decompressor */
	tjhandle jpeg_decompressor;

	/* Tile pixel buffer */
	uint8_t *tile_pixels;

	/* Callbacks */
	yetty_vnc_on_frame_fn on_frame_fn;
	void *on_frame_userdata;
	yetty_vnc_on_connected_fn on_connected_fn;
	void *on_connected_userdata;
	yetty_vnc_on_disconnected_fn on_disconnected_fn;
	void *on_disconnected_userdata;

	/* Stats */
	struct yetty_vnc_client_stats stats;
	uint64_t stats_bytes_window;
	uint32_t stats_frames_window;
	uint32_t stats_tiles_window;
	double stats_window_start;
};

static double get_time_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static inline int clamp_int(int val, int min_val, int max_val)
{
	if (val < min_val)
		return min_val;
	if (val > max_val)
		return max_val;
	return val;
}

static struct yetty_core_void_result ensure_resources(
	struct yetty_vnc_client *client, uint16_t width, uint16_t height);
static struct yetty_core_void_result create_pipeline(
	struct yetty_vnc_client *client);
static void send_input(struct yetty_vnc_client *client, const void *data,
		       size_t size);
static void drain_send_queue(struct yetty_vnc_client *client);

/* Forward declaration */
static struct yetty_core_int_result vnc_client_on_event(
	struct yetty_core_event_listener *listener,
	const struct yetty_core_event *event);

struct yetty_vnc_client_ptr_result
yetty_vnc_client_create(WGPUDevice device, WGPUQueue queue,
			WGPUTextureFormat surface_format,
			struct yetty_core_event_loop *event_loop,
			uint16_t width, uint16_t height)
{
	if (!event_loop)
		return YETTY_ERR(yetty_vnc_client_ptr, "event_loop is NULL");

	struct yetty_vnc_client *client =
		calloc(1, sizeof(struct yetty_vnc_client));
	if (!client)
		return YETTY_ERR(yetty_vnc_client_ptr, "failed to allocate client");

	client->device = device;
	client->queue = queue;
	client->surface_format = surface_format;
	client->event_loop = event_loop;
	client->listener.handler = vnc_client_on_event;
	client->poll_id = -1;
	client->width = width;
	client->height = height;
	client->socket = YETTY_SOCKET_INVALID;

	/* Initialize recv state */
	client->recv_state = RECV_FRAME_HEADER;
	client->recv_needed = sizeof(struct vnc_frame_header);
	client->recv_buffer_capacity = 64 * 1024;
	client->recv_buffer = malloc(client->recv_buffer_capacity);
	if (!client->recv_buffer) {
		free(client);
		return YETTY_ERR(yetty_vnc_client_ptr, "failed to allocate recv buffer");
	}

	/* Initialize JPEG decompressor */
	client->jpeg_decompressor = tjInitDecompress();
	if (!client->jpeg_decompressor) {
		free(client->recv_buffer);
		free(client);
		return YETTY_ERR(yetty_vnc_client_ptr, "failed to init jpeg decompressor");
	}

	/* Tile pixel buffer */
	client->tile_pixels = malloc(VNC_TILE_SIZE * VNC_TILE_SIZE * 4);
	if (!client->tile_pixels) {
		tjDestroy(client->jpeg_decompressor);
		free(client->recv_buffer);
		free(client);
		return YETTY_ERR(yetty_vnc_client_ptr, "failed to allocate tile pixels");
	}

	client->stats_window_start = get_time_sec();

	/* Create initial GPU resources */
	if (width > 0 && height > 0) {
		struct yetty_core_void_result res = ensure_resources(client, width, height);
		if (!YETTY_IS_OK(res)) {
			tjDestroy(client->jpeg_decompressor);
			free(client->tile_pixels);
			free(client->recv_buffer);
			free(client);
			return YETTY_ERR(yetty_vnc_client_ptr, "failed to create GPU resources");
		}
	}

	return YETTY_OK(yetty_vnc_client_ptr, client);
}

void yetty_vnc_client_destroy(struct yetty_vnc_client *client)
{
	if (!client)
		return;

	yetty_vnc_client_disconnect(client);

	if (client->jpeg_decompressor)
		tjDestroy(client->jpeg_decompressor);

	if (client->pipeline)
		wgpuRenderPipelineRelease(client->pipeline);
	if (client->bind_group)
		wgpuBindGroupRelease(client->bind_group);
	if (client->bind_group_layout)
		wgpuBindGroupLayoutRelease(client->bind_group_layout);
	if (client->sampler)
		wgpuSamplerRelease(client->sampler);
	if (client->texture_view)
		wgpuTextureViewRelease(client->texture_view);
	if (client->texture)
		wgpuTextureRelease(client->texture);

	free(client->reconnect_host);
	free(client->tile_pixels);
	free(client->recv_buffer);
	free(client->send_queue);
	free(client);
}

struct yetty_core_void_result
yetty_vnc_client_connect(struct yetty_vnc_client *client, const char *host,
			 uint16_t port)
{
	if (!client)
		return YETTY_ERR(yetty_core_void, "null client");
	if (client->connected)
		return YETTY_ERR(yetty_core_void, "already connected");

	yetty_platform_socket_init();

	/* Create socket */
	struct yetty_socket_fd_result sock_res = yetty_platform_socket_create_tcp();
	if (!YETTY_IS_OK(sock_res))
		return YETTY_ERR(yetty_core_void, "failed to create socket");

	client->socket = sock_res.value;

	/* Set non-blocking */
	struct yetty_core_void_result res = yetty_platform_socket_set_nonblocking(client->socket);
	if (!YETTY_IS_OK(res)) {
		yetty_platform_socket_close(client->socket);
		client->socket = YETTY_SOCKET_INVALID;
		return res;
	}

	/* Set TCP_NODELAY */
	yetty_platform_socket_set_nodelay(client->socket, 1);

	/* Reset recv state */
	client->recv_state = RECV_FRAME_HEADER;
	client->recv_needed = sizeof(struct vnc_frame_header);
	client->recv_offset = 0;

	/* Register socket with event loop */
	struct yetty_core_poll_id_result poll_res =
		client->event_loop->ops->create_poll(client->event_loop);
	if (!YETTY_IS_OK(poll_res)) {
		yetty_platform_socket_close(client->socket);
		client->socket = YETTY_SOCKET_INVALID;
		return YETTY_ERR(yetty_core_void, "failed to create poll");
	}
	client->poll_id = poll_res.value;

	res = client->event_loop->ops->config_poll(client->event_loop,
						   client->poll_id,
						   client->socket);
	if (!YETTY_IS_OK(res)) {
		client->event_loop->ops->destroy_poll(client->event_loop, client->poll_id);
		client->poll_id = -1;
		yetty_platform_socket_close(client->socket);
		client->socket = YETTY_SOCKET_INVALID;
		return res;
	}

	res = client->event_loop->ops->register_poll_listener(
		client->event_loop, client->poll_id, &client->listener);
	if (!YETTY_IS_OK(res)) {
		client->event_loop->ops->destroy_poll(client->event_loop, client->poll_id);
		client->poll_id = -1;
		yetty_platform_socket_close(client->socket);
		client->socket = YETTY_SOCKET_INVALID;
		return res;
	}

	/* Connect (async) */
	res = yetty_platform_socket_connect(client->socket, host, port);
	if (!YETTY_IS_OK(res)) {
		if (!yetty_platform_socket_connect_in_progress()) {
			client->event_loop->ops->destroy_poll(client->event_loop, client->poll_id);
			client->poll_id = -1;
			yetty_platform_socket_close(client->socket);
			client->socket = YETTY_SOCKET_INVALID;
			return res;
		}
		/* Connect in progress - watch for writable to know when connected */
		client->connecting = 1;
		client->event_loop->ops->start_poll(client->event_loop, client->poll_id,
						    YETTY_CORE_POLL_READABLE | YETTY_CORE_POLL_WRITABLE);
		ydebug("VNC client connecting to %s:%u (async)...", host, port);
	} else {
		/* Connected immediately (localhost) - watch for readable */
		client->connected = 1;
		client->event_loop->ops->start_poll(client->event_loop, client->poll_id,
						    YETTY_CORE_POLL_READABLE);
		ydebug("VNC client connected to %s:%u (immediate)", host, port);
		if (client->on_connected_fn)
			client->on_connected_fn(client->on_connected_userdata);
	}

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_vnc_client_disconnect(struct yetty_vnc_client *client)
{
	if (!client)
		return YETTY_OK_VOID();

	/* Unregister poll before closing socket */
	if (client->poll_id >= 0) {
		client->event_loop->ops->stop_poll(client->event_loop,
						   client->poll_id);
		client->event_loop->ops->destroy_poll(client->event_loop,
						      client->poll_id);
		client->poll_id = -1;
	}

	if (client->socket != YETTY_SOCKET_INVALID) {
		yetty_platform_socket_close(client->socket);
		client->socket = YETTY_SOCKET_INVALID;
	}
	client->connected = 0;
	client->connecting = 0;

	return YETTY_OK_VOID();
}

int yetty_vnc_client_is_connected(const struct yetty_vnc_client *client)
{
	return client ? client->connected : 0;
}

uint16_t yetty_vnc_client_width(const struct yetty_vnc_client *client)
{
	return client ? client->width : 0;
}

uint16_t yetty_vnc_client_height(const struct yetty_vnc_client *client)
{
	return client ? client->height : 0;
}

static void process_socket_readable(struct yetty_vnc_client *client)
{
	if (!client->connected || client->socket == YETTY_SOCKET_INVALID)
		return;

	int tiles_received = 0;
	int loop_count = 0;

	while (1) {
		loop_count++;
		if (loop_count > 10000) {
			yerror("VNC client: infinite loop in recv, breaking");
			break;
		}

		/* Ensure recv buffer capacity */
		if (client->recv_needed > client->recv_buffer_capacity) {
			size_t new_cap = client->recv_needed * 2;
			uint8_t *new_buf = realloc(client->recv_buffer, new_cap);
			if (!new_buf) {
				yerror("VNC client: failed to resize recv buffer");
				break;
			}
			client->recv_buffer = new_buf;
			client->recv_buffer_capacity = new_cap;
		}

		struct yetty_core_size_result recv_res = yetty_platform_socket_recv(
			client->socket,
			client->recv_buffer + client->recv_offset,
			client->recv_needed - client->recv_offset);

		if (!YETTY_IS_OK(recv_res)) {
			if (yetty_platform_socket_would_block()) {
				/* No more data */
				if (tiles_received && client->on_frame_fn)
					client->on_frame_fn(client->on_frame_userdata);
				return;
			}
			ywarn("VNC client: recv error");
			yetty_vnc_client_disconnect(client);
			if (client->on_disconnected_fn)
				client->on_disconnected_fn(client->on_disconnected_userdata);
			return;
		}

		if (recv_res.value == 0) {
			ydebug("VNC client: server closed connection");
			yetty_vnc_client_disconnect(client);
			if (client->on_disconnected_fn)
				client->on_disconnected_fn(client->on_disconnected_userdata);
			return;
		}

		client->recv_offset += recv_res.value;
		client->stats_bytes_window += recv_res.value;

		if (client->recv_offset < client->recv_needed)
			continue;

		/* Process based on current state */
		switch (client->recv_state) {
		case RECV_FRAME_HEADER: {
			memcpy(&client->current_frame, client->recv_buffer,
			       sizeof(struct vnc_frame_header));

			if (client->current_frame.magic != VNC_FRAME_MAGIC) {
				ywarn("VNC client: invalid frame magic 0x%08X",
				      client->current_frame.magic);
				/* Reset and try to resync */
				client->recv_state = RECV_FRAME_HEADER;
				client->recv_needed = sizeof(struct vnc_frame_header);
				client->recv_offset = 0;
				return;
			}

			/* Sanity check */
			if (client->current_frame.width == 0 ||
			    client->current_frame.height == 0 ||
			    client->current_frame.width > 8192 ||
			    client->current_frame.height > 8192) {
				ywarn("VNC client: invalid frame dimensions %ux%u",
				      client->current_frame.width,
				      client->current_frame.height);
				client->recv_state = RECV_FRAME_HEADER;
				client->recv_needed = sizeof(struct vnc_frame_header);
				client->recv_offset = 0;
				return;
			}

			ydebug("VNC client: frame %ux%u tiles=%u",
			       client->current_frame.width,
			       client->current_frame.height,
			       client->current_frame.num_tiles);

			/* Update dimensions */
			if (client->current_frame.width != client->width ||
			    client->current_frame.height != client->height) {
				client->width = client->current_frame.width;
				client->height = client->current_frame.height;
			}

			ensure_resources(client, client->width, client->height);
			client->tiles_received = 0;

			if (client->current_frame.num_tiles == 0) {
				/* No tiles */
				client->recv_state = RECV_FRAME_HEADER;
				client->recv_needed = sizeof(struct vnc_frame_header);
				client->recv_offset = 0;
			} else if (client->current_frame.tile_size == 0) {
				/* Rectangle mode */
				client->recv_state = RECV_RECT_HEADER;
				client->recv_needed = sizeof(struct vnc_rect_header);
				client->recv_offset = 0;
			} else {
				/* Tile mode */
				client->recv_state = RECV_TILE_HEADER;
				client->recv_needed = sizeof(struct vnc_tile_header);
				client->recv_offset = 0;
			}
			break;
		}

		case RECV_TILE_HEADER: {
			memcpy(&client->current_tile, client->recv_buffer,
			       sizeof(struct vnc_tile_header));

			if (client->current_tile.data_size > 16 * 1024 * 1024) {
				ywarn("VNC client: tile data too large %u",
				      client->current_tile.data_size);
				client->recv_state = RECV_FRAME_HEADER;
				client->recv_needed = sizeof(struct vnc_frame_header);
				client->recv_offset = 0;
				return;
			}

			client->recv_state = RECV_TILE_DATA;
			client->recv_needed = client->current_tile.data_size;
			client->recv_offset = 0;
			break;
		}

		case RECV_TILE_DATA: {
			ydebug("vnc_client: tile (%u,%u) encoding=%u size=%u",
			       client->current_tile.tile_x, client->current_tile.tile_y,
			       client->current_tile.encoding, client->current_tile.data_size);

			memset(client->tile_pixels, 0, VNC_TILE_SIZE * VNC_TILE_SIZE * 4);

			switch (client->current_tile.encoding) {
			case VNC_ENCODING_RAW:
				ydebug("vnc_client: decoding RAW tile");
				if (client->current_tile.data_size == VNC_TILE_SIZE * VNC_TILE_SIZE * 4) {
					memcpy(client->tile_pixels, client->recv_buffer,
					       client->current_tile.data_size);
				}
				break;

			case VNC_ENCODING_JPEG: {
				ydebug("vnc_client: decoding JPEG tile");
				int w, h, subsamp, cs;
				if (tjDecompressHeader3(client->jpeg_decompressor,
							client->recv_buffer,
							client->current_tile.data_size,
							&w, &h, &subsamp, &cs) == 0) {
					if (w <= VNC_TILE_SIZE && h <= VNC_TILE_SIZE) {
						tjDecompress2(client->jpeg_decompressor,
							      client->recv_buffer,
							      client->current_tile.data_size,
							      client->tile_pixels,
							      VNC_TILE_SIZE, 0, VNC_TILE_SIZE,
							      TJPF_BGRA, TJFLAG_FASTDCT);
					}
				}
				break;
			}

			case VNC_ENCODING_FULL_FRAME: {
				/* Full frame JPEG */
				int w, h, subsamp, cs;
				if (tjDecompressHeader3(client->jpeg_decompressor,
							client->recv_buffer,
							client->current_tile.data_size,
							&w, &h, &subsamp, &cs) == 0) {
					if ((uint16_t)w == client->width && (uint16_t)h == client->height) {
						uint8_t *full_frame = malloc(client->width * client->height * 4);
						if (full_frame) {
							tjDecompress2(client->jpeg_decompressor,
								      client->recv_buffer,
								      client->current_tile.data_size,
								      full_frame,
								      client->width, 0, client->height,
								      TJPF_BGRA, TJFLAG_FASTDCT);

							if (client->texture) {
								WGPUTexelCopyTextureInfo dst = {0};
								dst.texture = client->texture;
								dst.origin = (WGPUOrigin3D){0, 0, 0};

								WGPUTexelCopyBufferLayout layout = {0};
								layout.bytesPerRow = client->width * 4;
								layout.rowsPerImage = client->height;

								WGPUExtent3D size = {client->width, client->height, 1};
								wgpuQueueWriteTexture(client->queue, &dst, full_frame,
										      client->width * client->height * 4,
										      &layout, &size);
							}
							free(full_frame);
						}
					}
				}
				tiles_received = 1;
				client->tiles_received++;
				client->stats_tiles_window++;
				if (client->tiles_received >= client->current_frame.num_tiles) {
					client->stats_frames_window++;
					client->recv_state = RECV_FRAME_HEADER;
					client->recv_needed = sizeof(struct vnc_frame_header);
					client->recv_offset = 0;
					yetty_vnc_client_send_frame_ack(client);
				}
				break;
			}

			default:
				break;
			}

			/* Upload tile to GPU (skip for FULL_FRAME - already handled) */
			if (client->current_tile.encoding != VNC_ENCODING_FULL_FRAME &&
			    client->texture && client->width > 0 && client->height > 0) {
				uint32_t px = client->current_tile.tile_x * VNC_TILE_SIZE;
				uint32_t py = client->current_tile.tile_y * VNC_TILE_SIZE;
				if (px < client->width && py < client->height) {
					uint32_t tw = VNC_TILE_SIZE;
					uint32_t th = VNC_TILE_SIZE;
					if (px + tw > client->width)
						tw = client->width - px;
					if (py + th > client->height)
						th = client->height - py;

					ydebug("vnc_client: uploading tile to GPU at (%u,%u) size %ux%u",
					       px, py, tw, th);

					/* Debug: check first pixel */
					uint8_t *p = client->tile_pixels;
					ydebug("vnc_client: first pixel BGRA = %u,%u,%u,%u",
					       p[0], p[1], p[2], p[3]);

					WGPUTexelCopyTextureInfo dst = {0};
					dst.texture = client->texture;
					dst.origin = (WGPUOrigin3D){px, py, 0};

					WGPUTexelCopyBufferLayout layout = {0};
					layout.bytesPerRow = VNC_TILE_SIZE * 4;
					layout.rowsPerImage = VNC_TILE_SIZE;

					WGPUExtent3D size = {tw, th, 1};
					wgpuQueueWriteTexture(client->queue, &dst,
							      client->tile_pixels,
							      VNC_TILE_SIZE * VNC_TILE_SIZE * 4,
							      &layout, &size);
				}
			}

			if (client->current_tile.encoding != VNC_ENCODING_FULL_FRAME) {
				tiles_received = 1;
				client->tiles_received++;
				client->stats_tiles_window++;
			}

			if (client->tiles_received >= client->current_frame.num_tiles) {
				client->stats_frames_window++;
				client->recv_state = RECV_FRAME_HEADER;
				client->recv_needed = sizeof(struct vnc_frame_header);
				client->recv_offset = 0;
				yetty_vnc_client_send_frame_ack(client);
			} else {
				client->recv_state = RECV_TILE_HEADER;
				client->recv_needed = sizeof(struct vnc_tile_header);
				client->recv_offset = 0;
			}
			break;
		}

		case RECV_RECT_HEADER: {
			memcpy(&client->current_rect, client->recv_buffer,
			       sizeof(struct vnc_rect_header));

			if (client->current_rect.data_size > 16 * 1024 * 1024) {
				ywarn("VNC client: rect data too large %u",
				      client->current_rect.data_size);
				client->recv_state = RECV_FRAME_HEADER;
				client->recv_needed = sizeof(struct vnc_frame_header);
				client->recv_offset = 0;
				return;
			}

			client->recv_state = RECV_RECT_DATA;
			client->recv_needed = client->current_rect.data_size;
			client->recv_offset = 0;
			break;
		}

		case RECV_RECT_DATA: {
			uint16_t px = client->current_rect.px_x;
			uint16_t py = client->current_rect.px_y;
			uint16_t rw = client->current_rect.width;
			uint16_t rh = client->current_rect.height;

			uint8_t *pixels = malloc(rw * rh * 4);
			if (!pixels)
				break;

			memset(pixels, 0, rw * rh * 4);

			switch (client->current_rect.encoding) {
			case VNC_ENCODING_RECT_RAW:
				if (client->current_rect.data_size == (uint32_t)(rw * rh * 4)) {
					memcpy(pixels, client->recv_buffer,
					       client->current_rect.data_size);
				}
				break;

			case VNC_ENCODING_RECT_JPEG: {
				int w, h, subsamp, cs;
				if (tjDecompressHeader3(client->jpeg_decompressor,
							client->recv_buffer,
							client->current_rect.data_size,
							&w, &h, &subsamp, &cs) == 0) {
					if (w == rw && h == rh) {
						tjDecompress2(client->jpeg_decompressor,
							      client->recv_buffer,
							      client->current_rect.data_size,
							      pixels, rw, 0, rh,
							      TJPF_BGRA, TJFLAG_FASTDCT);
					}
				}
				break;
			}

			default:
				break;
			}

			/* Upload to GPU */
			if (client->texture && client->width > 0 && client->height > 0) {
				if (px < client->width && py < client->height) {
					uint32_t uw = rw;
					uint32_t uh = rh;
					if (px + uw > client->width)
						uw = client->width - px;
					if (py + uh > client->height)
						uh = client->height - py;

					WGPUTexelCopyTextureInfo dst = {0};
					dst.texture = client->texture;
					dst.origin = (WGPUOrigin3D){px, py, 0};

					WGPUTexelCopyBufferLayout layout = {0};
					layout.bytesPerRow = rw * 4;
					layout.rowsPerImage = rh;

					WGPUExtent3D size = {uw, uh, 1};
					wgpuQueueWriteTexture(client->queue, &dst, pixels,
							      rw * rh * 4, &layout, &size);
				}
			}

			free(pixels);

			tiles_received = 1;
			client->tiles_received++;
			client->stats_tiles_window++;

			if (client->tiles_received >= client->current_frame.num_tiles) {
				client->stats_frames_window++;
				client->recv_state = RECV_FRAME_HEADER;
				client->recv_needed = sizeof(struct vnc_frame_header);
				client->recv_offset = 0;
				yetty_vnc_client_send_frame_ack(client);
			} else {
				client->recv_state = RECV_RECT_HEADER;
				client->recv_needed = sizeof(struct vnc_rect_header);
				client->recv_offset = 0;
			}
			break;
		}
		}
	}
}

/* Event loop callback for socket events */
static struct yetty_core_int_result vnc_client_on_event(
	struct yetty_core_event_listener *listener,
	const struct yetty_core_event *event)
{
	struct yetty_vnc_client *client =
		(struct yetty_vnc_client *)((char *)listener -
			offsetof(struct yetty_vnc_client, listener));

	if (event->type == YETTY_EVENT_POLL_WRITABLE) {
		/* Check async connect completion */
		if (client->connecting) {
			struct yetty_core_void_result res =
				yetty_platform_socket_connect_check(client->socket);
			if (YETTY_IS_OK(res)) {
				client->connecting = 0;
				client->connected = 1;
				/* Switch to readable-only now that we're connected */
				client->event_loop->ops->start_poll(
					client->event_loop, client->poll_id,
					YETTY_CORE_POLL_READABLE);
				ydebug("VNC client connected (async complete)");
				if (client->on_connected_fn)
					client->on_connected_fn(client->on_connected_userdata);
			} else {
				yerror("VNC client async connect failed");
				yetty_vnc_client_disconnect(client);
				if (client->on_disconnected_fn)
					client->on_disconnected_fn(client->on_disconnected_userdata);
			}
			return YETTY_OK(yetty_core_int, 1);
		}

		/* Drain send queue */
		drain_send_queue(client);
		return YETTY_OK(yetty_core_int, 1);
	}

	if (event->type == YETTY_EVENT_POLL_READABLE) {
		ydebug("VNC client socket readable");
		process_socket_readable(client);
		return YETTY_OK(yetty_core_int, 1);
	}

	return YETTY_OK(yetty_core_int, 0);
}

struct yetty_core_void_result
yetty_vnc_client_update_texture(struct yetty_vnc_client *client)
{
	if (!client)
		return YETTY_ERR(yetty_core_void, "null client");

	/* Socket events are now handled by the event loop callback */

	/* Update stats every second */
	double now = get_time_sec();
	double elapsed = now - client->stats_window_start;
	if (elapsed >= 1.0) {
		client->stats.fps = client->stats_frames_window / elapsed;
		client->stats.tps = client->stats_tiles_window / elapsed;
		client->stats.mbps = (client->stats_bytes_window * 8.0) / (elapsed * 1000000.0);
		client->stats_frames_window = 0;
		client->stats_tiles_window = 0;
		client->stats_bytes_window = 0;
		client->stats_window_start = now;
	}

	return YETTY_OK_VOID();
}

static struct yetty_core_void_result ensure_resources(
	struct yetty_vnc_client *client, uint16_t width, uint16_t height)
{
	if (client->texture && client->texture_width == width &&
	    client->texture_height == height)
		return YETTY_OK_VOID();

	/* Cleanup old resources */
	if (client->bind_group) {
		wgpuBindGroupRelease(client->bind_group);
		client->bind_group = NULL;
	}
	if (client->texture_view) {
		wgpuTextureViewRelease(client->texture_view);
		client->texture_view = NULL;
	}
	if (client->texture) {
		wgpuTextureRelease(client->texture);
		client->texture = NULL;
	}

	/* Create texture */
	WGPUTextureDescriptor tex_desc = {0};
	tex_desc.size = (WGPUExtent3D){width, height, 1};
	tex_desc.format = WGPUTextureFormat_BGRA8Unorm;
	tex_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
	tex_desc.mipLevelCount = 1;
	tex_desc.sampleCount = 1;
	tex_desc.dimension = WGPUTextureDimension_2D;

	client->texture = wgpuDeviceCreateTexture(client->device, &tex_desc);
	if (!client->texture)
		return YETTY_ERR(yetty_core_void, "failed to create texture");

	client->texture_width = width;
	client->texture_height = height;

	client->texture_view = wgpuTextureCreateView(client->texture, NULL);
	if (!client->texture_view)
		return YETTY_ERR(yetty_core_void, "failed to create texture view");

	/* Create sampler (once) */
	if (!client->sampler) {
		WGPUSamplerDescriptor sampler_desc = {0};
		sampler_desc.magFilter = WGPUFilterMode_Linear;
		sampler_desc.minFilter = WGPUFilterMode_Linear;
		sampler_desc.addressModeU = WGPUAddressMode_ClampToEdge;
		sampler_desc.addressModeV = WGPUAddressMode_ClampToEdge;
		sampler_desc.maxAnisotropy = 1;
		client->sampler = wgpuDeviceCreateSampler(client->device, &sampler_desc);
	}

	/* Create pipeline (once) */
	if (!client->pipeline) {
		struct yetty_core_void_result res = create_pipeline(client);
		if (!YETTY_IS_OK(res))
			return res;
	}

	/* Create bind group */
	WGPUBindGroupEntry entries[2] = {0};
	entries[0].binding = 0;
	entries[0].textureView = client->texture_view;
	entries[1].binding = 1;
	entries[1].sampler = client->sampler;

	WGPUBindGroupDescriptor bg_desc = {0};
	bg_desc.layout = client->bind_group_layout;
	bg_desc.entryCount = 2;
	bg_desc.entries = entries;

	client->bind_group = wgpuDeviceCreateBindGroup(client->device, &bg_desc);
	if (!client->bind_group)
		return YETTY_ERR(yetty_core_void, "failed to create bind group");

	return YETTY_OK_VOID();
}

static struct yetty_core_void_result create_pipeline(struct yetty_vnc_client *client)
{
	/* Create shader module */
	WGPUShaderSourceWGSL wgsl_desc = {0};
	wgsl_desc.chain.sType = WGPUSType_ShaderSourceWGSL;
	wgsl_desc.code = (WGPUStringView){QUAD_SHADER, strlen(QUAD_SHADER)};

	WGPUShaderModuleDescriptor shader_desc = {0};
	shader_desc.nextInChain = &wgsl_desc.chain;

	WGPUShaderModule shader = wgpuDeviceCreateShaderModule(client->device, &shader_desc);
	if (!shader)
		return YETTY_ERR(yetty_core_void, "failed to create shader");

	/* Bind group layout */
	WGPUBindGroupLayoutEntry layout_entries[2] = {0};
	layout_entries[0].binding = 0;
	layout_entries[0].visibility = WGPUShaderStage_Fragment;
	layout_entries[0].texture.sampleType = WGPUTextureSampleType_Float;
	layout_entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;

	layout_entries[1].binding = 1;
	layout_entries[1].visibility = WGPUShaderStage_Fragment;
	layout_entries[1].sampler.type = WGPUSamplerBindingType_Filtering;

	WGPUBindGroupLayoutDescriptor bgl_desc = {0};
	bgl_desc.entryCount = 2;
	bgl_desc.entries = layout_entries;
	client->bind_group_layout = wgpuDeviceCreateBindGroupLayout(client->device, &bgl_desc);

	/* Pipeline layout */
	WGPUPipelineLayoutDescriptor pl_desc = {0};
	pl_desc.bindGroupLayoutCount = 1;
	pl_desc.bindGroupLayouts = &client->bind_group_layout;
	WGPUPipelineLayout pipeline_layout = wgpuDeviceCreatePipelineLayout(client->device, &pl_desc);

	/* Render pipeline */
	WGPURenderPipelineDescriptor pipeline_desc = {0};
	pipeline_desc.layout = pipeline_layout;

	pipeline_desc.vertex.module = shader;
	pipeline_desc.vertex.entryPoint = (WGPUStringView){"vs_main", 7};

	WGPUColorTargetState color_target = {0};
	color_target.format = client->surface_format;
	color_target.writeMask = WGPUColorWriteMask_All;

	WGPUFragmentState fragment = {0};
	fragment.module = shader;
	fragment.entryPoint = (WGPUStringView){"fs_main", 7};
	fragment.targetCount = 1;
	fragment.targets = &color_target;
	pipeline_desc.fragment = &fragment;

	pipeline_desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
	pipeline_desc.multisample.count = 1;
	pipeline_desc.multisample.mask = ~0u;

	client->pipeline = wgpuDeviceCreateRenderPipeline(client->device, &pipeline_desc);

	wgpuShaderModuleRelease(shader);
	wgpuPipelineLayoutRelease(pipeline_layout);

	if (!client->pipeline)
		return YETTY_ERR(yetty_core_void, "failed to create pipeline");

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_vnc_client_render(struct yetty_vnc_client *client,
			WGPURenderPassEncoder pass, uint32_t render_target_w,
			uint32_t render_target_h)
{
	if (!client)
		return YETTY_ERR(yetty_core_void, "null client");

	ydebug("vnc_client_render: pipeline=%p bind_group=%p texture=%p",
	       (void *)client->pipeline, (void *)client->bind_group,
	       (void *)client->texture);
	ydebug("vnc_client_render: client dims %ux%u, target %ux%u",
	       client->width, client->height, render_target_w, render_target_h);
	ydebug("vnc_client_render: texture dims %ux%u",
	       client->texture_width, client->texture_height);

	if (!client->pipeline || !client->bind_group) {
		ydebug("vnc_client_render: no pipeline or bind_group, skipping");
		return YETTY_OK_VOID();
	}

	/* NOTE: Caller (vnc_viewer) already sets viewport, don't override it!
	 * The fullscreen quad shader draws -1..1 NDC which maps to the viewport.
	 */

	wgpuRenderPassEncoderSetPipeline(pass, client->pipeline);
	wgpuRenderPassEncoderSetBindGroup(pass, 0, client->bind_group, 0, NULL);
	wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);

	ydebug("vnc_client_render: draw submitted");

	return YETTY_OK_VOID();
}

WGPUTextureView
yetty_vnc_client_get_texture_view(const struct yetty_vnc_client *client)
{
	return client ? client->texture_view : NULL;
}

/* Input sending */

static void send_input(struct yetty_vnc_client *client, const void *data,
		       size_t size)
{
	if (!client->connected || client->socket == YETTY_SOCKET_INVALID)
		return;

	const uint8_t *ptr = data;

	/* If send queue is empty, try to send directly */
	if (client->send_queue_size == client->send_offset) {
		struct yetty_core_size_result res =
			yetty_platform_socket_send(client->socket, ptr, size);
		if (YETTY_IS_OK(res)) {
			if ((size_t)res.value == size)
				return; /* All sent */
			ptr += res.value;
			size -= res.value;
		} else if (!yetty_platform_socket_would_block()) {
			return;
		}
	}

	/* Queue remaining data */
	size_t new_size = client->send_queue_size + size;
	if (new_size > client->send_queue_capacity) {
		size_t new_cap = new_size * 2;
		uint8_t *new_buf = realloc(client->send_queue, new_cap);
		if (!new_buf)
			return;
		client->send_queue = new_buf;
		client->send_queue_capacity = new_cap;
	}
	memcpy(client->send_queue + client->send_queue_size, ptr, size);
	client->send_queue_size += size;
}

static void drain_send_queue(struct yetty_vnc_client *client)
{
	if (client->send_offset >= client->send_queue_size ||
	    client->socket == YETTY_SOCKET_INVALID)
		return;

	while (client->send_offset < client->send_queue_size) {
		size_t remaining = client->send_queue_size - client->send_offset;
		struct yetty_core_size_result res = yetty_platform_socket_send(
			client->socket,
			client->send_queue + client->send_offset,
			remaining);

		if (!YETTY_IS_OK(res) || res.value <= 0) {
			if (yetty_platform_socket_would_block())
				return;
			client->send_queue_size = 0;
			client->send_offset = 0;
			return;
		}

		client->send_offset += res.value;
	}

	/* All sent */
	client->send_queue_size = 0;
	client->send_offset = 0;
}

struct yetty_core_void_result
yetty_vnc_client_send_mouse_move(struct yetty_vnc_client *client, int16_t x,
				 int16_t y, uint8_t mods)
{
	if (!client)
		return YETTY_ERR(yetty_core_void, "null client");

	struct vnc_input_header hdr = {0};
	hdr.type = VNC_INPUT_MOUSE_MOVE;
	hdr.data_size = sizeof(struct vnc_mouse_move_event);

	struct vnc_mouse_move_event evt = {0};
	evt.x = x;
	evt.y = y;
	evt.mods = mods;

	uint8_t buf[sizeof(hdr) + sizeof(evt)];
	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), &evt, sizeof(evt));
	send_input(client, buf, sizeof(buf));

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_vnc_client_send_mouse_button(struct yetty_vnc_client *client, int16_t x,
				   int16_t y, uint8_t button, int pressed,
				   uint8_t mods)
{
	if (!client)
		return YETTY_ERR(yetty_core_void, "null client");

	struct vnc_input_header hdr = {0};
	hdr.type = VNC_INPUT_MOUSE_BUTTON;
	hdr.data_size = sizeof(struct vnc_mouse_button_event);

	struct vnc_mouse_button_event evt = {0};
	evt.x = x;
	evt.y = y;
	evt.button = button;
	evt.pressed = pressed ? 1 : 0;
	evt.mods = mods;

	uint8_t buf[sizeof(hdr) + sizeof(evt)];
	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), &evt, sizeof(evt));
	send_input(client, buf, sizeof(buf));

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_vnc_client_send_mouse_scroll(struct yetty_vnc_client *client, int16_t x,
				   int16_t y, int16_t delta_x, int16_t delta_y,
				   uint8_t mods)
{
	if (!client)
		return YETTY_ERR(yetty_core_void, "null client");

	struct vnc_input_header hdr = {0};
	hdr.type = VNC_INPUT_MOUSE_SCROLL;
	hdr.data_size = sizeof(struct vnc_mouse_scroll_event);

	struct vnc_mouse_scroll_event evt = {0};
	evt.x = x;
	evt.y = y;
	evt.delta_x = delta_x;
	evt.delta_y = delta_y;
	evt.mods = mods;

	uint8_t buf[sizeof(hdr) + sizeof(evt)];
	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), &evt, sizeof(evt));
	send_input(client, buf, sizeof(buf));

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_vnc_client_send_key_down(struct yetty_vnc_client *client,
			       uint32_t keycode, uint32_t scancode,
			       uint8_t mods)
{
	if (!client)
		return YETTY_ERR(yetty_core_void, "null client");

	struct vnc_input_header hdr = {0};
	hdr.type = VNC_INPUT_KEY_DOWN;
	hdr.data_size = sizeof(struct vnc_key_event);

	struct vnc_key_event evt = {0};
	evt.keycode = keycode;
	evt.scancode = scancode;
	evt.mods = mods;

	uint8_t buf[sizeof(hdr) + sizeof(evt)];
	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), &evt, sizeof(evt));
	send_input(client, buf, sizeof(buf));

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_vnc_client_send_key_up(struct yetty_vnc_client *client, uint32_t keycode,
			     uint32_t scancode, uint8_t mods)
{
	if (!client)
		return YETTY_ERR(yetty_core_void, "null client");

	struct vnc_input_header hdr = {0};
	hdr.type = VNC_INPUT_KEY_UP;
	hdr.data_size = sizeof(struct vnc_key_event);

	struct vnc_key_event evt = {0};
	evt.keycode = keycode;
	evt.scancode = scancode;
	evt.mods = mods;

	uint8_t buf[sizeof(hdr) + sizeof(evt)];
	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), &evt, sizeof(evt));
	send_input(client, buf, sizeof(buf));

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_vnc_client_send_char_with_mods(struct yetty_vnc_client *client,
				     uint32_t codepoint, uint8_t mods)
{
	if (!client)
		return YETTY_ERR(yetty_core_void, "null client");

	struct vnc_input_header hdr = {0};
	hdr.type = VNC_INPUT_CHAR_WITH_MODS;
	hdr.data_size = sizeof(struct vnc_char_with_mods_event);

	struct vnc_char_with_mods_event evt = {0};
	evt.codepoint = codepoint;
	evt.mods = mods;

	uint8_t buf[sizeof(hdr) + sizeof(evt)];
	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), &evt, sizeof(evt));
	send_input(client, buf, sizeof(buf));

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_vnc_client_send_text_input(struct yetty_vnc_client *client,
				 const char *text, size_t len)
{
	if (!client)
		return YETTY_ERR(yetty_core_void, "null client");
	if (len == 0 || len > 1024)
		return YETTY_OK_VOID();

	struct vnc_input_header hdr = {0};
	hdr.type = VNC_INPUT_TEXT;
	hdr.data_size = (uint16_t)len;

	uint8_t *buf = malloc(sizeof(hdr) + len);
	if (!buf)
		return YETTY_ERR(yetty_core_void, "out of memory");

	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), text, len);
	send_input(client, buf, sizeof(hdr) + len);
	free(buf);

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_vnc_client_send_resize(struct yetty_vnc_client *client, uint16_t width,
			     uint16_t height)
{
	if (!client)
		return YETTY_ERR(yetty_core_void, "null client");

	ydebug("VNC client send resize: %ux%u", width, height);

	struct vnc_input_header hdr = {0};
	hdr.type = VNC_INPUT_RESIZE;
	hdr.data_size = sizeof(struct vnc_resize_event);

	struct vnc_resize_event evt = {0};
	evt.width = width;
	evt.height = height;

	uint8_t buf[sizeof(hdr) + sizeof(evt)];
	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), &evt, sizeof(evt));
	send_input(client, buf, sizeof(buf));

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_vnc_client_send_cell_size(struct yetty_vnc_client *client,
				uint8_t cell_height)
{
	if (!client)
		return YETTY_ERR(yetty_core_void, "null client");

	struct vnc_input_header hdr = {0};
	hdr.type = VNC_INPUT_CELL_SIZE;
	hdr.data_size = sizeof(struct vnc_cell_size_event);

	struct vnc_cell_size_event evt = {0};
	evt.cell_height = cell_height;

	uint8_t buf[sizeof(hdr) + sizeof(evt)];
	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), &evt, sizeof(evt));
	send_input(client, buf, sizeof(buf));

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_vnc_client_send_frame_ack(struct yetty_vnc_client *client)
{
	if (!client)
		return YETTY_ERR(yetty_core_void, "null client");

	struct vnc_input_header hdr = {0};
	hdr.type = VNC_INPUT_FRAME_ACK;
	hdr.data_size = 0;

	send_input(client, &hdr, sizeof(hdr));

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_vnc_client_send_compression_config(struct yetty_vnc_client *client,
					 int force_raw, uint8_t quality,
					 int always_full, uint8_t codec)
{
	if (!client)
		return YETTY_ERR(yetty_core_void, "null client");

	client->stats.quality = quality;

	struct vnc_input_header hdr = {0};
	hdr.type = VNC_INPUT_COMPRESSION_CONFIG;
	hdr.data_size = sizeof(struct vnc_compression_config_event);

	struct vnc_compression_config_event evt = {0};
	evt.force_raw = force_raw ? 1 : 0;
	evt.quality = quality;
	evt.always_full = always_full ? 1 : 0;
	evt.codec = codec;

	uint8_t buf[sizeof(hdr) + sizeof(evt)];
	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), &evt, sizeof(evt));
	send_input(client, buf, sizeof(buf));

	return YETTY_OK_VOID();
}

void yetty_vnc_client_set_on_frame(struct yetty_vnc_client *client,
				   yetty_vnc_on_frame_fn callback,
				   void *userdata)
{
	if (!client)
		return;
	client->on_frame_fn = callback;
	client->on_frame_userdata = userdata;
}

void yetty_vnc_client_set_on_connected(struct yetty_vnc_client *client,
				       yetty_vnc_on_connected_fn callback,
				       void *userdata)
{
	if (!client)
		return;
	client->on_connected_fn = callback;
	client->on_connected_userdata = userdata;
}

void yetty_vnc_client_set_on_disconnected(struct yetty_vnc_client *client,
					  yetty_vnc_on_disconnected_fn callback,
					  void *userdata)
{
	if (!client)
		return;
	client->on_disconnected_fn = callback;
	client->on_disconnected_userdata = userdata;
}

struct yetty_vnc_client_stats
yetty_vnc_client_get_stats(const struct yetty_vnc_client *client)
{
	if (!client) {
		struct yetty_vnc_client_stats empty = {0};
		return empty;
	}
	return client->stats;
}

void yetty_vnc_client_set_reconnect_params(struct yetty_vnc_client *client,
					   const char *host, uint16_t port)
{
	if (!client)
		return;
	free(client->reconnect_host);
	client->reconnect_host = host ? strdup(host) : NULL;
	client->reconnect_port = port;
}

struct yetty_core_void_result
yetty_vnc_client_reconnect(struct yetty_vnc_client *client)
{
	if (!client)
		return YETTY_ERR(yetty_core_void, "null client");
	if (!client->reconnect_host)
		return YETTY_ERR(yetty_core_void, "no reconnect params set");

	ydebug("VNC client reconnecting to %s:%u", client->reconnect_host,
	       client->reconnect_port);
	client->wants_reconnect = 0;

	/* Reset recv state */
	client->recv_state = RECV_FRAME_HEADER;
	client->recv_needed = sizeof(struct vnc_frame_header);
	client->recv_offset = 0;

	/* Clear send queue */
	client->send_queue_size = 0;
	client->send_offset = 0;

	return yetty_vnc_client_connect(client, client->reconnect_host,
					client->reconnect_port);
}

int yetty_vnc_client_wants_reconnect(const struct yetty_vnc_client *client)
{
	return client ? client->wants_reconnect : 0;
}

void yetty_vnc_client_clear_reconnect(struct yetty_vnc_client *client)
{
	if (client)
		client->wants_reconnect = 0;
}
