/*
 * vnc-server.c - VNC server using libuv TCP via event loop
 */

#include <yetty/yvnc/vnc-server.h>
#include <yetty/ycore/event-loop.h>
#include <yetty/webgpu/error.h>
#include <yetty/yplatform/ycoroutine.h>
#include <yetty/yplatform/ywebgpu.h>
#include <yetty/yrender-utils/tile-diff.h>
#include <yetty/ytrace.h>
#include "protocol.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>

#define MAX_CLIENTS 16
#define FULL_REFRESH_INTERVAL 300
#define RECV_BUFFER_SIZE 65536

/* Per-client context */
struct vnc_client_ctx {
	struct yetty_vnc_server *server;
	struct yetty_tcp_conn *conn;
	int slot;

	/* Input buffer */
	uint8_t *recv_buffer;
	size_t recv_buffer_capacity;
	size_t recv_offset;
	size_t recv_needed;
	int reading_header;
	struct vnc_input_header header;
};

struct yetty_vnc_server {
	WGPUInstance instance;
	WGPUDevice device;
	WGPUQueue queue;
	struct yplatform_wgpu *wgpu;

	/* Event loop for async I/O */
	struct yetty_ycore_event_loop *event_loop;
	yetty_ycore_tcp_server_id tcp_server_id;

	/* Server state */
	uint16_t port;
	int running;

	/* Connected clients */
	struct vnc_client_ctx *clients[MAX_CLIENTS];
	size_t client_count;

	/* Frame dimensions */
	uint32_t last_width;
	uint32_t last_height;

	/* Delta + readback pipeline. Owns the prev-frame texture, the diff
	 * compute pipeline, and the readback buffers. See
	 * include/yetty/yrender-utils/tile-diff.h. */
	struct yetty_yrender_utils_tile_diff_engine *diff_engine;

	/* CPU framebuffer path (send_frame_cpu). */
	const uint8_t *cpu_pixels;
	uint32_t cpu_pixels_size;

	/* Packed (width*4 stride) copy of the most recent GPU readback, used
	 * by encode_tile. The engine hands us a row-aligned mapped range; we
	 * pack it here so encode_tile can keep using last_width*4 as the row
	 * pitch. */
	uint8_t *gpu_readback_pixels;
	size_t gpu_readback_pixels_size;

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

/* Forward declarations */
static void dispatch_input(struct yetty_vnc_server *server,
			   const struct vnc_input_header *hdr,
			   const uint8_t *data);
static struct yetty_ycore_void_result ensure_cpu_state(struct yetty_vnc_server *server,
						       uint32_t width, uint32_t height);
static struct yetty_ycore_void_result encode_tile(struct yetty_vnc_server *server,
						 uint16_t tx, uint16_t ty,
						 uint8_t **out_data, size_t *out_size,
						 uint8_t *out_encoding);

/*===========================================================================
 * TCP Server Callbacks
 *===========================================================================*/

static void *vnc_server_on_connect(void *ctx, struct yetty_tcp_conn *conn)
{
	struct yetty_vnc_server *server = ctx;

	/* Find empty slot */
	int slot = -1;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (server->clients[i] == NULL) {
			slot = i;
			break;
		}
	}

	if (slot < 0) {
		ywarn("VNC max clients reached, rejecting");
		server->event_loop->ops->tcp_close(conn);
		return NULL;
	}

	struct vnc_client_ctx *client_ctx = calloc(1, sizeof(struct vnc_client_ctx));
	if (!client_ctx) {
		yerror("VNC failed to allocate client context");
		server->event_loop->ops->tcp_close(conn);
		return NULL;
	}

	client_ctx->server = server;
	client_ctx->conn = conn;
	client_ctx->slot = slot;
	client_ctx->recv_buffer_capacity = RECV_BUFFER_SIZE;
	client_ctx->recv_buffer = malloc(client_ctx->recv_buffer_capacity);
	if (!client_ctx->recv_buffer) {
		free(client_ctx);
		server->event_loop->ops->tcp_close(conn);
		return NULL;
	}
	client_ctx->recv_needed = sizeof(struct vnc_input_header);
	client_ctx->reading_header = 1;

	server->clients[slot] = client_ctx;
	server->client_count++;
	server->force_full_frame = 1;

	yinfo("VNC client connected (slot %d, total %zu)", slot, server->client_count);

	/* Trigger render to send first frame to new client */
	if (server->event_loop && server->event_loop->ops->request_render)
		server->event_loop->ops->request_render(server->event_loop);

	return client_ctx;
}

static void vnc_server_on_alloc(void *conn_ctx, size_t suggested, char **buf, size_t *len)
{
	struct vnc_client_ctx *client_ctx = conn_ctx;
	(void)suggested;

	if (!client_ctx) {
		*buf = NULL;
		*len = 0;
		return;
	}

	/* Return pointer into recv buffer at current offset */
	size_t space = client_ctx->recv_buffer_capacity - client_ctx->recv_offset;
	*buf = (char *)client_ctx->recv_buffer + client_ctx->recv_offset;
	*len = space;
}

static void vnc_server_on_data(void *conn_ctx, struct yetty_tcp_conn *conn,
			       const char *data, long nread)
{
	struct vnc_client_ctx *client_ctx = conn_ctx;
	(void)conn;
	(void)data; /* Data is already in recv_buffer from on_alloc */

	if (!client_ctx || nread <= 0)
		return;

	struct yetty_vnc_server *server = client_ctx->server;
	client_ctx->recv_offset += (size_t)nread;

	/* Process complete messages */
	while (client_ctx->recv_offset >= client_ctx->recv_needed) {
		if (client_ctx->reading_header) {
			/* Parse header */
			memcpy(&client_ctx->header, client_ctx->recv_buffer,
			       sizeof(struct vnc_input_header));

			if (client_ctx->header.data_size > 0) {
				client_ctx->reading_header = 0;
				client_ctx->recv_needed = sizeof(struct vnc_input_header) +
							  client_ctx->header.data_size;

				/* Resize buffer if needed */
				if (client_ctx->recv_needed > client_ctx->recv_buffer_capacity) {
					size_t new_cap = client_ctx->recv_needed * 2;
					uint8_t *new_buf = realloc(client_ctx->recv_buffer, new_cap);
					if (!new_buf) {
						yerror("VNC realloc failed");
						return;
					}
					client_ctx->recv_buffer = new_buf;
					client_ctx->recv_buffer_capacity = new_cap;
				}
				continue;
			}
			/* No payload - dispatch immediately */
		}

		/* Dispatch input */
		const uint8_t *payload = client_ctx->recv_buffer + sizeof(struct vnc_input_header);
		dispatch_input(server, &client_ctx->header, payload);

		/* Notify input received */
		if (server->on_input_received_fn)
			server->on_input_received_fn(server->on_input_received_userdata);

		/* Shift remaining data */
		size_t consumed = sizeof(struct vnc_input_header) + client_ctx->header.data_size;
		size_t remaining = client_ctx->recv_offset - consumed;
		if (remaining > 0)
			memmove(client_ctx->recv_buffer,
				client_ctx->recv_buffer + consumed,
				remaining);
		client_ctx->recv_offset = remaining;
		client_ctx->recv_needed = sizeof(struct vnc_input_header);
		client_ctx->reading_header = 1;
	}
}

static void vnc_server_on_disconnect(void *conn_ctx)
{
	struct vnc_client_ctx *client_ctx = conn_ctx;
	if (!client_ctx)
		return;

	struct yetty_vnc_server *server = client_ctx->server;
	int slot = client_ctx->slot;

	yinfo("VNC client disconnected (slot %d)", slot);

	server->clients[slot] = NULL;
	server->client_count--;

	free(client_ctx->recv_buffer);
	free(client_ctx);
}

/*===========================================================================
 * Public API
 *===========================================================================*/

struct yetty_vnc_server_ptr_result
yetty_vnc_server_create(WGPUInstance instance, WGPUDevice device,
			WGPUQueue queue,
			struct yetty_ycore_event_loop *event_loop,
			struct yplatform_wgpu *wgpu)
{
	if (!event_loop)
		return YETTY_ERR(yetty_vnc_server_ptr, "event_loop is NULL");
	if (!wgpu)
		return YETTY_ERR(yetty_vnc_server_ptr, "wgpu is NULL");

	struct yetty_vnc_server *server =
		calloc(1, sizeof(struct yetty_vnc_server));
	if (!server)
		return YETTY_ERR(yetty_vnc_server_ptr, "failed to allocate server");

	server->instance = instance;
	server->device = device;
	server->queue = queue;
	server->wgpu = wgpu;
	server->event_loop = event_loop;
	server->tcp_server_id = -1;
	server->jpeg_quality = 80;
	server->force_full_frame = 1;

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

	if (server->diff_engine)
		yetty_yrender_utils_tile_diff_engine_destroy(server->diff_engine);

	if (server->jpeg_compressor)
		tjDestroy(server->jpeg_compressor);

	free(server->dirty_tiles);
	free(server->gpu_readback_pixels);
	free(server);
}

struct yetty_ycore_void_result
yetty_vnc_server_start(struct yetty_vnc_server *server, uint16_t port)
{
	if (!server)
		return YETTY_ERR(yetty_ycore_void, "null server");

	if (server->running)
		return YETTY_ERR(yetty_ycore_void, "already running");

	/* Setup TCP server callbacks */
	struct yetty_tcp_server_callbacks callbacks = {
		.ctx = server,
		.on_connect = vnc_server_on_connect,
		.on_alloc = vnc_server_on_alloc,
		.on_data = vnc_server_on_data,
		.on_disconnect = vnc_server_on_disconnect,
	};

	/* Create TCP server */
	struct yetty_ycore_tcp_server_id_result id_res =
		server->event_loop->ops->create_tcp_server(
			server->event_loop, "0.0.0.0", port, &callbacks);
	if (!YETTY_IS_OK(id_res))
		return YETTY_ERR(yetty_ycore_void, "failed to create TCP server");

	server->tcp_server_id = id_res.value;

	/* Start listening */
	struct yetty_ycore_void_result res =
		server->event_loop->ops->start_tcp_server(
			server->event_loop, server->tcp_server_id);
	if (!YETTY_IS_OK(res)) {
		server->tcp_server_id = -1;
		return res;
	}

	server->port = port;
	server->running = 1;

	yinfo("VNC server listening on port %u", port);
	return YETTY_OK_VOID();
}

struct yetty_ycore_void_result
yetty_vnc_server_stop(struct yetty_vnc_server *server)
{
	if (!server)
		return YETTY_OK_VOID();

	server->running = 0;

	/* Stop TCP server (closes all connections) */
	if (server->tcp_server_id >= 0) {
		server->event_loop->ops->stop_tcp_server(
			server->event_loop, server->tcp_server_id);
		server->tcp_server_id = -1;
	}

	/* Free any remaining client contexts */
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (server->clients[i]) {
			free(server->clients[i]->recv_buffer);
			free(server->clients[i]);
			server->clients[i] = NULL;
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
	/* Capture is synchronous now; always ready when ack isn't pending. */
	return server ? !server->awaiting_ack : 0;
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
}

int yetty_vnc_server_get_use_h264(const struct yetty_vnc_server *server)
{
	return server ? server->use_h264 : 0;
}

void yetty_vnc_server_force_h264_idr(struct yetty_vnc_server *server)
{
	(void)server;
}

/*===========================================================================
 * Send to clients
 *===========================================================================*/

static struct yetty_ycore_void_result send_to_all_clients(
	struct yetty_vnc_server *server, const void *data, size_t size)
{
	for (int i = 0; i < MAX_CLIENTS; i++) {
		struct vnc_client_ctx *client = server->clients[i];
		if (client && client->conn) {
			server->event_loop->ops->tcp_send(client->conn, data, size);
		}
	}
	return YETTY_OK_VOID();
}

/*===========================================================================
 * CPU-side per-frame state + tile encoding
 *
 * The GPU diff + readback pipeline lives in yrender-utils/tile-diff.c. What
 * remains here is the CPU-side bookkeeping the wire encoder needs:
 *   - last_width / last_height for encode_tile's per-row pointer math
 *   - tiles_x / tiles_y for the dirty-tile iteration
 *   - dirty_tiles[] tracking which tiles still need to be sent
 * Both the CPU-input path (send_frame_cpu) and the GPU-readback sink reset
 * these via ensure_cpu_state().
 *===========================================================================*/

static struct yetty_ycore_void_result ensure_cpu_state(struct yetty_vnc_server *server,
						       uint32_t width, uint32_t height)
{
	if (server->last_width == width && server->last_height == height &&
	    server->dirty_tiles)
		return YETTY_OK_VOID();

	server->force_full_frame = 1;
	if (server->diff_engine)
		yetty_yrender_utils_tile_diff_engine_force_full(server->diff_engine);

	server->last_width = width;
	server->last_height = height;
	server->tiles_x = vnc_tiles_x(width);
	server->tiles_y = vnc_tiles_y(height);

	uint32_t num_tiles = server->tiles_x * server->tiles_y;

	free(server->dirty_tiles);
	server->dirty_tiles = calloc(num_tiles, sizeof(int));
	if (!server->dirty_tiles)
		return YETTY_ERR(yetty_ycore_void, "failed to allocate dirty tiles");

	ydebug("VNC CPU state: %ux%u, %u tiles", width, height, num_tiles);
	return YETTY_OK_VOID();
}

static struct yetty_ycore_void_result encode_tile(struct yetty_vnc_server *server,
						 uint16_t tx, uint16_t ty,
						 uint8_t **out_data, size_t *out_size,
						 uint8_t *out_encoding)
{
	const uint8_t *pixels = server->cpu_pixels ? server->cpu_pixels : server->gpu_readback_pixels;
	if (!pixels)
		return YETTY_ERR(yetty_ycore_void, "no pixels");

	uint32_t start_x = tx * VNC_TILE_SIZE;
	uint32_t start_y = ty * VNC_TILE_SIZE;
	uint32_t tile_w = VNC_TILE_SIZE;
	uint32_t tile_h = VNC_TILE_SIZE;

	if (start_x + tile_w > server->last_width)
		tile_w = server->last_width - start_x;
	if (start_y + tile_h > server->last_height)
		tile_h = server->last_height - start_y;

	uint32_t row_pitch = server->last_width * 4;
	size_t raw_size = tile_w * tile_h * 4;

	static uint8_t tile_buffer[VNC_TILE_SIZE * VNC_TILE_SIZE * 4];

	for (uint32_t y = 0; y < tile_h; y++) {
		const uint8_t *src = pixels + (start_y + y) * row_pitch + start_x * 4;
		uint8_t *dst = tile_buffer + y * tile_w * 4;
		memcpy(dst, src, tile_w * 4);
	}

	if (server->force_raw) {
		*out_data = tile_buffer;
		*out_size = raw_size;
		*out_encoding = VNC_ENCODING_RAW;
		return YETTY_OK_VOID();
	}

	static uint8_t jpeg_buffer[VNC_TILE_SIZE * VNC_TILE_SIZE * 4];
	unsigned long jpeg_size = sizeof(jpeg_buffer);
	unsigned char *jpeg_out = jpeg_buffer;

	int result = tjCompress2(server->jpeg_compressor,
				 tile_buffer, tile_w, 0, tile_h,
				 TJPF_BGRA, &jpeg_out, &jpeg_size,
				 TJSAMP_420, server->jpeg_quality,
				 TJFLAG_FASTDCT | TJFLAG_NOREALLOC);

	if (result == 0 && jpeg_size < raw_size) {
		*out_data = jpeg_buffer;
		*out_size = jpeg_size;
		*out_encoding = VNC_ENCODING_JPEG;
	} else {
		*out_data = tile_buffer;
		*out_size = raw_size;
		*out_encoding = VNC_ENCODING_RAW;
	}

	return YETTY_OK_VOID();
}

/*===========================================================================
 * Input dispatch
 *===========================================================================*/

static void dispatch_input(struct yetty_vnc_server *server,
			   const struct vnc_input_header *hdr,
			   const uint8_t *data)
{
	ydebug("VNC dispatch_input: type=%u size=%u", hdr->type, hdr->data_size);
	switch (hdr->type) {
	case VNC_INPUT_MOUSE_MOVE:
		if (hdr->data_size >= sizeof(struct vnc_mouse_move_event) &&
		    server->on_mouse_move_fn) {
			const struct vnc_mouse_move_event *msg = (const void *)data;
			server->on_mouse_move_fn(msg->x, msg->y, msg->mods,
						 server->on_mouse_move_userdata);
		}
		break;

	case VNC_INPUT_MOUSE_BUTTON:
		if (hdr->data_size >= sizeof(struct vnc_mouse_button_event) &&
		    server->on_mouse_button_fn) {
			const struct vnc_mouse_button_event *msg = (const void *)data;
			server->on_mouse_button_fn(msg->x, msg->y, msg->button,
						   msg->pressed, msg->mods,
						   server->on_mouse_button_userdata);
		}
		break;

	case VNC_INPUT_MOUSE_SCROLL:
		if (hdr->data_size >= sizeof(struct vnc_mouse_scroll_event) &&
		    server->on_mouse_scroll_fn) {
			const struct vnc_mouse_scroll_event *msg = (const void *)data;
			server->on_mouse_scroll_fn(msg->x, msg->y,
						   msg->delta_x, msg->delta_y,
						   msg->mods,
						   server->on_mouse_scroll_userdata);
		}
		break;

	case VNC_INPUT_KEY_DOWN:
		if (hdr->data_size >= sizeof(struct vnc_key_event) &&
		    server->on_key_down_fn) {
			const struct vnc_key_event *msg = (const void *)data;
			server->on_key_down_fn(msg->keycode, msg->scancode,
					       msg->mods,
					       server->on_key_down_userdata);
		}
		break;

	case VNC_INPUT_KEY_UP:
		if (hdr->data_size >= sizeof(struct vnc_key_event) &&
		    server->on_key_up_fn) {
			const struct vnc_key_event *msg = (const void *)data;
			server->on_key_up_fn(msg->keycode, msg->scancode,
					     msg->mods,
					     server->on_key_up_userdata);
		}
		break;

	case VNC_INPUT_TEXT:
		if (server->on_text_input_fn && hdr->data_size > 0) {
			server->on_text_input_fn((const char *)data,
						 hdr->data_size,
						 server->on_text_input_userdata);
		}
		break;

	case VNC_INPUT_RESIZE:
		if (hdr->data_size >= sizeof(struct vnc_resize_event) &&
		    server->on_resize_fn) {
			const struct vnc_resize_event *msg = (const void *)data;
			server->on_resize_fn(msg->width, msg->height,
					     server->on_resize_userdata);
		}
		break;

	case VNC_INPUT_CELL_SIZE:
		if (hdr->data_size >= sizeof(struct vnc_cell_size_event) &&
		    server->on_cell_size_fn) {
			const struct vnc_cell_size_event *msg = (const void *)data;
			server->on_cell_size_fn(msg->cell_height,
						server->on_cell_size_userdata);
		}
		break;

	case VNC_INPUT_CHAR_WITH_MODS:
		if (hdr->data_size >= sizeof(struct vnc_char_with_mods_event) &&
		    server->on_char_with_mods_fn) {
			const struct vnc_char_with_mods_event *msg = (const void *)data;
			server->on_char_with_mods_fn(msg->codepoint, msg->mods,
						     server->on_char_with_mods_userdata);
		}
		break;

	case VNC_INPUT_FRAME_ACK:
		server->awaiting_ack = 0;
		break;

	default:
		ydebug("VNC unknown input type %u", hdr->type);
		break;
	}
}

/*===========================================================================
 * Frame capture and send
 *===========================================================================*/

struct yetty_ycore_void_result
yetty_vnc_server_send_frame_cpu(struct yetty_vnc_server *server,
				const uint8_t *pixels,
				uint32_t width, uint32_t height)
{
	if (!server || !server->running || server->client_count == 0)
		return YETTY_OK_VOID();

	server->cpu_pixels = pixels;
	server->cpu_pixels_size = width * height * 4;

	struct yetty_ycore_void_result res = ensure_cpu_state(server, width, height);
	if (!YETTY_IS_OK(res))
		return res;

	int do_full = server->force_full_frame || server->always_full_frame;
	if (do_full)
		server->force_full_frame = 0;

	/* Mark all tiles dirty for full frame, or compute diff */
	uint32_t num_tiles = server->tiles_x * server->tiles_y;
	if (do_full) {
		for (uint32_t i = 0; i < num_tiles; i++)
			server->dirty_tiles[i] = 1;
	}

	/* Count dirty tiles */
	uint16_t dirty_count = 0;
	for (uint32_t i = 0; i < num_tiles; i++) {
		if (server->dirty_tiles[i])
			dirty_count++;
	}

	if (dirty_count == 0)
		return YETTY_OK_VOID();

	/* Build frame */
	struct vnc_frame_header frame_hdr = {
		.magic = VNC_FRAME_MAGIC,
		.width = (uint16_t)width,
		.height = (uint16_t)height,
		.tile_size = VNC_TILE_SIZE,
		.num_tiles = dirty_count
	};

	send_to_all_clients(server, &frame_hdr, sizeof(frame_hdr));

	/* Send tiles */
	for (uint16_t ty = 0; ty < server->tiles_y; ty++) {
		for (uint16_t tx = 0; tx < server->tiles_x; tx++) {
			uint32_t idx = ty * server->tiles_x + tx;
			if (!server->dirty_tiles[idx])
				continue;

			uint8_t *tile_data;
			size_t tile_size;
			uint8_t encoding;

			res = encode_tile(server, tx, ty, &tile_data, &tile_size, &encoding);
			if (!YETTY_IS_OK(res))
				continue;

			struct vnc_tile_header tile_hdr = {
				.tile_x = tx,
				.tile_y = ty,
				.encoding = encoding,
				.data_size = (uint32_t)tile_size
			};

			send_to_all_clients(server, &tile_hdr, sizeof(tile_hdr));
			send_to_all_clients(server, tile_data, tile_size);

			server->dirty_tiles[idx] = 0;
		}
	}

	server->awaiting_ack = 1;
	server->current_stats.frames++;

	return YETTY_OK_VOID();
}

/* (flags_map_callback / pixels_map_callback removed — replaced by
 * yplatform_wgpu_buffer_map_await which yields the coroutine and resumes
 * it on the loop thread when the map completes.) */

/* Encode and send dirty tiles */
static struct yetty_ycore_void_result encode_and_send_dirty_tiles(
	struct yetty_vnc_server *server, uint32_t width, uint32_t height)
{
	uint32_t num_tiles = server->tiles_x * server->tiles_y;

	/* Count dirty tiles */
	uint16_t dirty_count = 0;
	for (uint32_t i = 0; i < num_tiles; i++) {
		if (server->dirty_tiles[i])
			dirty_count++;
	}

	if (dirty_count == 0)
		return YETTY_OK_VOID();

	/* Build frame header */
	struct vnc_frame_header frame_hdr = {
		.magic = VNC_FRAME_MAGIC,
		.width = (uint16_t)width,
		.height = (uint16_t)height,
		.tile_size = VNC_TILE_SIZE,
		.num_tiles = dirty_count
	};

	send_to_all_clients(server, &frame_hdr, sizeof(frame_hdr));

	/* Send dirty tiles */
	for (uint16_t ty = 0; ty < server->tiles_y; ty++) {
		for (uint16_t tx = 0; tx < server->tiles_x; tx++) {
			uint32_t idx = ty * server->tiles_x + tx;
			if (!server->dirty_tiles[idx])
				continue;

			uint8_t *tile_data;
			size_t tile_size;
			uint8_t encoding;

			struct yetty_ycore_void_result res =
				encode_tile(server, tx, ty, &tile_data, &tile_size, &encoding);
			if (!YETTY_IS_OK(res))
				continue;

			struct vnc_tile_header tile_hdr = {
				.tile_x = tx,
				.tile_y = ty,
				.encoding = encoding,
				.data_size = (uint32_t)tile_size
			};

			send_to_all_clients(server, &tile_hdr, sizeof(tile_hdr));
			send_to_all_clients(server, tile_data, tile_size);

			server->dirty_tiles[idx] = 0;
		}
	}

	server->awaiting_ack = 1;
	server->current_stats.frames++;

	return YETTY_OK_VOID();
}

/*
 * Invoked by the tile-diff engine on the loop thread after a submit has
 * completed and at least one subsequent submit was dropped for back-
 * pressure. Requests another render so the engine can catch up with the
 * latest render-target texture content.
 */
static void vnc_on_engine_idle(void *ctx)
{
	struct yetty_vnc_server *server = ctx;
	if (server && server->event_loop && server->event_loop->ops->request_render)
		server->event_loop->ops->request_render(server->event_loop);
}

/*
 * Sink callback invoked by the tile-diff engine once the GPU diff + readback
 * have completed. `frame->pixels` is a row-aligned mapped range that's only
 * valid until this function returns, so we pack it into gpu_readback_pixels
 * (flat width*4 stride) and then defer to encode_and_send_dirty_tiles which
 * uses the existing encode_tile machinery.
 */
static void vnc_tile_diff_sink(void *ctx,
			       const struct yetty_yrender_utils_tile_diff_frame *frame)
{
	struct yetty_vnc_server *server = ctx;

	struct yetty_ycore_void_result res =
		ensure_cpu_state(server, frame->width, frame->height);
	if (!YETTY_IS_OK(res)) {
		ywarn("vnc sink: ensure_cpu_state failed: %s", res.error.msg);
		return;
	}

	/* Use GPU-readback pixels for encode_tile (cpu_pixels is the other
	 * input path; clear it so encode_tile picks the readback). */
	server->cpu_pixels = NULL;

	size_t pixel_size = (size_t)frame->width * frame->height * 4;
	if (server->gpu_readback_pixels_size < pixel_size) {
		free(server->gpu_readback_pixels);
		server->gpu_readback_pixels = malloc(pixel_size);
		if (!server->gpu_readback_pixels) {
			ywarn("vnc sink: failed to allocate readback buffer");
			return;
		}
		server->gpu_readback_pixels_size = pixel_size;
	}

	/* Pack the aligned mapped pixels into a width*4-stride buffer so
	 * encode_tile can use last_width*4 as the row pitch. */
	uint32_t packed_row = frame->width * 4;
	for (uint32_t y = 0; y < frame->height; y++) {
		memcpy(server->gpu_readback_pixels + y * packed_row,
		       frame->pixels + y * frame->aligned_bytes_per_row,
		       packed_row);
	}

	/* Translate the engine's dirty bitmap into the int-sized dirty_tiles
	 * array the wire encoder expects. */
	uint32_t num_tiles = server->tiles_x * server->tiles_y;
	if ((uint32_t)(frame->tiles_x * frame->tiles_y) != num_tiles) {
		ywarn("vnc sink: tile count mismatch engine=%ux%u server=%ux%u",
		      frame->tiles_x, frame->tiles_y,
		      server->tiles_x, server->tiles_y);
		return;
	}
	for (uint32_t i = 0; i < num_tiles; i++)
		server->dirty_tiles[i] = frame->dirty_bitmap[i] ? 1 : 0;

	res = encode_and_send_dirty_tiles(server, frame->width, frame->height);
	if (!YETTY_IS_OK(res))
		ywarn("vnc sink: encode_and_send failed: %s", res.error.msg);
}

struct yetty_ycore_void_result
yetty_vnc_server_send_frame_gpu(struct yetty_vnc_server *server,
				WGPUTexture texture,
				uint32_t width, uint32_t height)
{
	if (!server || !server->running || server->client_count == 0)
		return YETTY_OK_VOID();

	if (!server->diff_engine) {
		struct yetty_yrender_utils_tile_diff_engine_ptr_result eng_res =
			yetty_yrender_utils_tile_diff_engine_create(
				server->device, server->queue, server->wgpu,
				VNC_TILE_SIZE);
		if (!YETTY_IS_OK(eng_res))
			return YETTY_ERR(yetty_ycore_void, eng_res.error.msg);
		server->diff_engine = eng_res.value;

		/* Engine back-pressure: if a second submit arrives while the first
		 * is still reading back, it's dropped to avoid racing on the shared
		 * GPU buffers. The engine fires this callback on the loop thread
		 * once it's idle; we ask for another render so the catch-up frame
		 * ships. Without this, a burst of terminal output (nvim initial
		 * draw, `find /nix` storm) would stall visibly until the next
		 * unrelated event nudged the render loop. */
		yetty_yrender_utils_tile_diff_engine_set_on_idle(
			server->diff_engine, vnc_on_engine_idle, server);
	}

	/* Propagate VNC-level full-frame requests (e.g. new client) to the
	 * engine so the next submit marks all tiles dirty. */
	if (server->force_full_frame) {
		server->force_full_frame = 0;
		yetty_yrender_utils_tile_diff_engine_force_full(server->diff_engine);
	}
	yetty_yrender_utils_tile_diff_engine_set_always_full(
		server->diff_engine, server->always_full_frame != 0);

	return yetty_yrender_utils_tile_diff_engine_submit(
		server->diff_engine, texture, width, height,
		vnc_tile_diff_sink, server);
}

struct yetty_ycore_void_result
yetty_vnc_server_send_frame(struct yetty_vnc_server *server, WGPUTexture texture,
			    const uint8_t *cpu_pixels, uint32_t width,
			    uint32_t height)
{
	(void)texture;
	/* Use CPU path for now */
	return yetty_vnc_server_send_frame_cpu(server, cpu_pixels, width, height);
}

/*===========================================================================
 * Callback setters
 *===========================================================================*/

void yetty_vnc_server_set_on_mouse_move(struct yetty_vnc_server *server,
					yetty_vnc_on_mouse_move_fn fn, void *userdata)
{
	if (server) {
		server->on_mouse_move_fn = fn;
		server->on_mouse_move_userdata = userdata;
	}
}

void yetty_vnc_server_set_on_mouse_button(struct yetty_vnc_server *server,
					  yetty_vnc_on_mouse_button_fn fn, void *userdata)
{
	if (server) {
		server->on_mouse_button_fn = fn;
		server->on_mouse_button_userdata = userdata;
	}
}

void yetty_vnc_server_set_on_mouse_scroll(struct yetty_vnc_server *server,
					  yetty_vnc_on_mouse_scroll_fn fn, void *userdata)
{
	if (server) {
		server->on_mouse_scroll_fn = fn;
		server->on_mouse_scroll_userdata = userdata;
	}
}

void yetty_vnc_server_set_on_key_down(struct yetty_vnc_server *server,
				      yetty_vnc_on_key_down_fn fn, void *userdata)
{
	if (server) {
		server->on_key_down_fn = fn;
		server->on_key_down_userdata = userdata;
	}
}

void yetty_vnc_server_set_on_key_up(struct yetty_vnc_server *server,
				    yetty_vnc_on_key_up_fn fn, void *userdata)
{
	if (server) {
		server->on_key_up_fn = fn;
		server->on_key_up_userdata = userdata;
	}
}

void yetty_vnc_server_set_on_text_input(struct yetty_vnc_server *server,
					yetty_vnc_on_text_input_fn fn, void *userdata)
{
	if (server) {
		server->on_text_input_fn = fn;
		server->on_text_input_userdata = userdata;
	}
}

void yetty_vnc_server_set_on_resize(struct yetty_vnc_server *server,
				    yetty_vnc_on_resize_fn fn, void *userdata)
{
	if (server) {
		server->on_resize_fn = fn;
		server->on_resize_userdata = userdata;
	}
}

void yetty_vnc_server_set_on_cell_size(struct yetty_vnc_server *server,
				       yetty_vnc_on_cell_size_fn fn, void *userdata)
{
	if (server) {
		server->on_cell_size_fn = fn;
		server->on_cell_size_userdata = userdata;
	}
}

void yetty_vnc_server_set_on_char_with_mods(struct yetty_vnc_server *server,
					    yetty_vnc_on_char_with_mods_fn fn, void *userdata)
{
	if (server) {
		server->on_char_with_mods_fn = fn;
		server->on_char_with_mods_userdata = userdata;
	}
}

void yetty_vnc_server_set_on_input_received(struct yetty_vnc_server *server,
					    yetty_vnc_on_input_received_fn fn, void *userdata)
{
	if (server) {
		server->on_input_received_fn = fn;
		server->on_input_received_userdata = userdata;
	}
}

struct yetty_vnc_server_stats
yetty_vnc_server_get_stats(const struct yetty_vnc_server *server)
{
	if (server)
		return server->stats;
	struct yetty_vnc_server_stats empty = {0};
	return empty;
}
