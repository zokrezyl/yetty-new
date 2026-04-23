#include <yetty/yvnc/vnc-client.h>
#include <yetty/platform/socket.h>
#include "protocol.h"

#include <stdlib.h>
#include <string.h>

struct yetty_vnc_client {
	WGPUDevice device;
	WGPUQueue queue;
	WGPUTextureFormat surface_format;

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
	uint8_t *pixels;

	/* GPU resources */
	WGPUTexture texture;
	uint16_t texture_width;
	uint16_t texture_height;
	WGPUTextureView texture_view;
	WGPUSampler sampler;
	WGPUBindGroup bind_group;
	WGPUBindGroupLayout bind_group_layout;
	WGPURenderPipeline pipeline;
	WGPUBuffer vertex_buffer;

	/* Callbacks */
	yetty_vnc_on_frame_fn on_frame_fn;
	void *on_frame_userdata;
	yetty_vnc_on_connected_fn on_connected_fn;
	void *on_connected_userdata;
	yetty_vnc_on_disconnected_fn on_disconnected_fn;
	void *on_disconnected_userdata;

	/* Stats */
	struct yetty_vnc_client_stats stats;
};

struct yetty_vnc_client_ptr_result
yetty_vnc_client_create(WGPUDevice device, WGPUQueue queue,
			WGPUTextureFormat surface_format, uint16_t width,
			uint16_t height)
{
	struct yetty_vnc_client *client =
		calloc(1, sizeof(struct yetty_vnc_client));
	if (!client)
		return YETTY_ERR(yetty_vnc_client_ptr, "failed to allocate client");

	client->device = device;
	client->queue = queue;
	client->surface_format = surface_format;
	client->width = width;
	client->height = height;
	client->socket = YETTY_SOCKET_INVALID;

	return YETTY_OK(yetty_vnc_client_ptr, client);
}

void yetty_vnc_client_destroy(struct yetty_vnc_client *client)
{
	if (!client)
		return;

	if (client->socket != YETTY_SOCKET_INVALID)
		yetty_yplatform_socket_close(client->socket);

	free(client->reconnect_host);
	free(client->pixels);
	free(client);
}

struct yetty_ycore_void_result
yetty_vnc_client_connect(struct yetty_vnc_client *client, const char *host,
			 uint16_t port)
{
	/* TODO: implement */
	(void)client;
	(void)host;
	(void)port;
	return YETTY_ERR(yetty_ycore_void, "not implemented");
}

struct yetty_ycore_void_result
yetty_vnc_client_disconnect(struct yetty_vnc_client *client)
{
	if (!client)
		return YETTY_OK_VOID();

	if (client->socket != YETTY_SOCKET_INVALID) {
		yetty_yplatform_socket_close(client->socket);
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

struct yetty_ycore_void_result
yetty_vnc_client_update_texture(struct yetty_vnc_client *client)
{
	/* TODO: implement */
	(void)client;
	return YETTY_OK_VOID();
}

struct yetty_ycore_void_result
yetty_vnc_client_render(struct yetty_vnc_client *client,
			WGPURenderPassEncoder pass, uint32_t render_target_w,
			uint32_t render_target_h)
{
	/* TODO: implement */
	(void)client;
	(void)pass;
	(void)render_target_w;
	(void)render_target_h;
	return YETTY_OK_VOID();
}

WGPUTextureView
yetty_vnc_client_get_texture_view(const struct yetty_vnc_client *client)
{
	return client ? client->texture_view : NULL;
}

struct yetty_ycore_void_result
yetty_vnc_client_send_mouse_move(struct yetty_vnc_client *client, int16_t x,
				 int16_t y, uint8_t mods)
{
	/* TODO: implement */
	(void)client;
	(void)x;
	(void)y;
	(void)mods;
	return YETTY_OK_VOID();
}

struct yetty_ycore_void_result
yetty_vnc_client_send_mouse_button(struct yetty_vnc_client *client, int16_t x,
				   int16_t y, uint8_t button, int pressed,
				   uint8_t mods)
{
	/* TODO: implement */
	(void)client;
	(void)x;
	(void)y;
	(void)button;
	(void)pressed;
	(void)mods;
	return YETTY_OK_VOID();
}

struct yetty_ycore_void_result
yetty_vnc_client_send_mouse_scroll(struct yetty_vnc_client *client, int16_t x,
				   int16_t y, int16_t delta_x, int16_t delta_y,
				   uint8_t mods)
{
	/* TODO: implement */
	(void)client;
	(void)x;
	(void)y;
	(void)delta_x;
	(void)delta_y;
	(void)mods;
	return YETTY_OK_VOID();
}

struct yetty_ycore_void_result
yetty_vnc_client_send_key_down(struct yetty_vnc_client *client,
			       uint32_t keycode, uint32_t scancode,
			       uint8_t mods)
{
	/* TODO: implement */
	(void)client;
	(void)keycode;
	(void)scancode;
	(void)mods;
	return YETTY_OK_VOID();
}

struct yetty_ycore_void_result
yetty_vnc_client_send_key_up(struct yetty_vnc_client *client, uint32_t keycode,
			     uint32_t scancode, uint8_t mods)
{
	/* TODO: implement */
	(void)client;
	(void)keycode;
	(void)scancode;
	(void)mods;
	return YETTY_OK_VOID();
}

struct yetty_ycore_void_result
yetty_vnc_client_send_char_with_mods(struct yetty_vnc_client *client,
				     uint32_t codepoint, uint8_t mods)
{
	/* TODO: implement */
	(void)client;
	(void)codepoint;
	(void)mods;
	return YETTY_OK_VOID();
}

struct yetty_ycore_void_result
yetty_vnc_client_send_text_input(struct yetty_vnc_client *client,
				 const char *text, size_t len)
{
	/* TODO: implement */
	(void)client;
	(void)text;
	(void)len;
	return YETTY_OK_VOID();
}

struct yetty_ycore_void_result
yetty_vnc_client_send_resize(struct yetty_vnc_client *client, uint16_t width,
			     uint16_t height)
{
	/* TODO: implement */
	(void)client;
	(void)width;
	(void)height;
	return YETTY_OK_VOID();
}

struct yetty_ycore_void_result
yetty_vnc_client_send_cell_size(struct yetty_vnc_client *client,
				uint8_t cell_height)
{
	/* TODO: implement */
	(void)client;
	(void)cell_height;
	return YETTY_OK_VOID();
}

struct yetty_ycore_void_result
yetty_vnc_client_send_frame_ack(struct yetty_vnc_client *client)
{
	/* TODO: implement */
	(void)client;
	return YETTY_OK_VOID();
}

struct yetty_ycore_void_result
yetty_vnc_client_send_compression_config(struct yetty_vnc_client *client,
					 int force_raw, uint8_t quality,
					 int always_full, uint8_t codec)
{
	/* TODO: implement */
	(void)client;
	(void)force_raw;
	(void)quality;
	(void)always_full;
	(void)codec;
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

struct yetty_ycore_void_result
yetty_vnc_client_reconnect(struct yetty_vnc_client *client)
{
	if (!client)
		return YETTY_ERR(yetty_ycore_void, "null client");
	if (!client->reconnect_host)
		return YETTY_ERR(yetty_ycore_void, "no reconnect params set");
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
