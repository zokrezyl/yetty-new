#include <yetty/yvnc/vnc-server.h>
#include <yetty/platform/socket.h>
#include "protocol.h"

#include <stdlib.h>
#include <string.h>

struct yetty_vnc_server {
	WGPUDevice device;
	WGPUQueue queue;

	/* Server socket */
	yetty_socket_fd server_fd;
	uint16_t port;
	int running;

	/* Connected clients */
	yetty_socket_fd *clients;
	size_t client_count;
	size_t client_capacity;

	/* Frame dimensions */
	uint32_t last_width;
	uint32_t last_height;

	/* Settings */
	int merge_rectangles;
	int force_raw;
	uint8_t jpeg_quality;
	int always_full_frame;
	int use_h264;
	int force_full_frame;
	int awaiting_ack;

	/* Stats */
	struct yetty_vnc_server_stats stats;

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

struct yetty_vnc_server_ptr_result
yetty_vnc_server_create(WGPUDevice device, WGPUQueue queue)
{
	struct yetty_vnc_server *server =
		calloc(1, sizeof(struct yetty_vnc_server));
	if (!server)
		return YETTY_ERR(yetty_vnc_server_ptr, "failed to allocate server");

	server->device = device;
	server->queue = queue;
	server->server_fd = YETTY_SOCKET_INVALID;
	server->jpeg_quality = 80;
	server->force_full_frame = 1;

	return YETTY_OK(yetty_vnc_server_ptr, server);
}

void yetty_vnc_server_destroy(struct yetty_vnc_server *server)
{
	if (!server)
		return;

	if (server->server_fd != YETTY_SOCKET_INVALID)
		yetty_platform_socket_close(server->server_fd);

	for (size_t i = 0; i < server->client_count; i++) {
		if (server->clients[i] != YETTY_SOCKET_INVALID)
			yetty_platform_socket_close(server->clients[i]);
	}
	free(server->clients);
	free(server);
}

struct yetty_core_void_result
yetty_vnc_server_start(struct yetty_vnc_server *server, uint16_t port)
{
	/* TODO: implement */
	(void)server;
	(void)port;
	return YETTY_ERR(yetty_core_void, "not implemented");
}

struct yetty_core_void_result
yetty_vnc_server_stop(struct yetty_vnc_server *server)
{
	if (!server)
		return YETTY_OK_VOID();

	if (server->server_fd != YETTY_SOCKET_INVALID) {
		yetty_platform_socket_close(server->server_fd);
		server->server_fd = YETTY_SOCKET_INVALID;
	}
	server->running = 0;

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
	/* TODO: implement when H.264 support is added */
	(void)server;
}

struct yetty_core_void_result
yetty_vnc_server_send_frame(struct yetty_vnc_server *server, WGPUTexture texture,
			    const uint8_t *cpu_pixels, uint32_t width,
			    uint32_t height)
{
	/* TODO: implement */
	(void)server;
	(void)texture;
	(void)cpu_pixels;
	(void)width;
	(void)height;
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
	/* TODO: implement */
	(void)server;
	return 0;
}

struct yetty_core_void_result
yetty_vnc_server_process_input(struct yetty_vnc_server *server)
{
	/* TODO: implement */
	(void)server;
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

void yetty_vnc_server_set_on_mouse_move(struct yetty_vnc_server *server,
					yetty_vnc_on_mouse_move_fn callback,
					void *userdata)
{
	if (!server)
		return;
	server->on_mouse_move_fn = callback;
	server->on_mouse_move_userdata = userdata;
}

void yetty_vnc_server_set_on_mouse_button(struct yetty_vnc_server *server,
					  yetty_vnc_on_mouse_button_fn callback,
					  void *userdata)
{
	if (!server)
		return;
	server->on_mouse_button_fn = callback;
	server->on_mouse_button_userdata = userdata;
}

void yetty_vnc_server_set_on_mouse_scroll(struct yetty_vnc_server *server,
					  yetty_vnc_on_mouse_scroll_fn callback,
					  void *userdata)
{
	if (!server)
		return;
	server->on_mouse_scroll_fn = callback;
	server->on_mouse_scroll_userdata = userdata;
}

void yetty_vnc_server_set_on_key_down(struct yetty_vnc_server *server,
				      yetty_vnc_on_key_down_fn callback,
				      void *userdata)
{
	if (!server)
		return;
	server->on_key_down_fn = callback;
	server->on_key_down_userdata = userdata;
}

void yetty_vnc_server_set_on_key_up(struct yetty_vnc_server *server,
				    yetty_vnc_on_key_up_fn callback,
				    void *userdata)
{
	if (!server)
		return;
	server->on_key_up_fn = callback;
	server->on_key_up_userdata = userdata;
}

void yetty_vnc_server_set_on_text_input(struct yetty_vnc_server *server,
					yetty_vnc_on_text_input_fn callback,
					void *userdata)
{
	if (!server)
		return;
	server->on_text_input_fn = callback;
	server->on_text_input_userdata = userdata;
}

void yetty_vnc_server_set_on_resize(struct yetty_vnc_server *server,
				    yetty_vnc_on_resize_fn callback,
				    void *userdata)
{
	if (!server)
		return;
	server->on_resize_fn = callback;
	server->on_resize_userdata = userdata;
}

void yetty_vnc_server_set_on_cell_size(struct yetty_vnc_server *server,
				       yetty_vnc_on_cell_size_fn callback,
				       void *userdata)
{
	if (!server)
		return;
	server->on_cell_size_fn = callback;
	server->on_cell_size_userdata = userdata;
}

void yetty_vnc_server_set_on_char_with_mods(
	struct yetty_vnc_server *server,
	yetty_vnc_on_char_with_mods_fn callback, void *userdata)
{
	if (!server)
		return;
	server->on_char_with_mods_fn = callback;
	server->on_char_with_mods_userdata = userdata;
}

void yetty_vnc_server_set_on_input_received(
	struct yetty_vnc_server *server,
	yetty_vnc_on_input_received_fn callback, void *userdata)
{
	if (!server)
		return;
	server->on_input_received_fn = callback;
	server->on_input_received_userdata = userdata;
}
