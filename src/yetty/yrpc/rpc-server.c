/* RPC server implementation */

#include <yetty/yrpc/rpc-server.h>
#include <yetty/platform/ipc-socket.h>
#include <yetty/ytrace.h>

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#endif

/* Maximum number of handlers */
#define MAX_HANDLERS 64

/* Maximum number of clients */
#define MAX_CLIENTS 16

/* Receive buffer size */
#define RECV_BUFFER_SIZE 4096

/* Response buffer size */
#define RESPONSE_BUFFER_SIZE 4096

/* Handler entry */
struct yetty_rpc_handler_entry {
	uint32_t channel;
	char method[64];
	yetty_rpc_handler_fn handler;
	void *userdata;
};

/* Client connection */
struct yetty_rpc_client {
	yetty_ipc_socket_t socket;
	uint8_t recv_buf[RECV_BUFFER_SIZE];
	size_t recv_len;
};

/* RPC server */
struct yetty_rpc_server {
	yetty_ipc_socket_t listen_socket;
	char socket_path[YETTY_IPC_SOCKET_PATH_MAX];
	int running;

	/* Handlers */
	struct yetty_rpc_handler_entry handlers[MAX_HANDLERS];
	size_t handler_count;

	/* Connected clients */
	struct yetty_rpc_client clients[MAX_CLIENTS];
	size_t client_count;

	/* Response buffer (reused) */
	uint8_t response_buf[RESPONSE_BUFFER_SIZE];
};

static struct yetty_rpc_handler_entry *
find_handler(struct yetty_rpc_server *server, uint32_t channel,
	     const char *method, size_t method_len)
{
	for (size_t i = 0; i < server->handler_count; i++) {
		struct yetty_rpc_handler_entry *e = &server->handlers[i];
		if (e->channel == channel &&
		    strncmp(e->method, method, method_len) == 0 &&
		    e->method[method_len] == '\0') {
			return e;
		}
	}
	return NULL;
}

static void handle_message(struct yetty_rpc_server *server,
			   struct yetty_rpc_client *client,
			   const uint8_t *data, size_t len)
{
	struct yetty_rpc_message_result parse_res;
	struct yetty_rpc_message msg;
	struct yetty_rpc_handler_entry *handler;
	struct yetty_rpc_handler_result result;
	struct yetty_rpc_write_buffer wbuf;
	struct yetty_core_size_result send_res;

	parse_res = yetty_rpc_message_parse(data, len);
	if (YETTY_IS_ERR(parse_res)) {
		ytrace("yrpc: failed to parse message: %s", parse_res.error.msg);
		return;
	}

	msg = parse_res.value;

	ytrace("yrpc: received %s: channel=%u method=%.*s msgid=%u",
	       msg.type == YETTY_RPC_MSG_REQUEST      ? "request" :
	       msg.type == YETTY_RPC_MSG_NOTIFICATION ? "notification" :
						        "response",
	       msg.channel, (int)msg.method_len, msg.method, msg.msgid);

	/* Find handler */
	handler = find_handler(server, msg.channel, msg.method, msg.method_len);
	if (!handler) {
		ytrace("yrpc: no handler for channel=%u method=%.*s",
		       msg.channel, (int)msg.method_len, msg.method);

		if (msg.type == YETTY_RPC_MSG_REQUEST) {
			/* Send error response */
			yetty_rpc_write_buffer_init(&wbuf, server->response_buf,
						    RESPONSE_BUFFER_SIZE);
			yetty_rpc_write_response_error(&wbuf, msg.msgid,
						       "method not found");
			yetty_ipc_socket_send(client->socket, wbuf.data,
					      wbuf.len);
		}
		return;
	}

	/* Call handler */
	result = handler->handler(&msg, handler->userdata);

	/* Send response for requests */
	if (msg.type == YETTY_RPC_MSG_REQUEST) {
		yetty_rpc_write_buffer_init(&wbuf, server->response_buf,
					    RESPONSE_BUFFER_SIZE);

		if (result.ok) {
			if (result.value.data) {
				yetty_rpc_write_response_ok(
					&wbuf, msg.msgid, result.value.data,
					result.value.len);
			} else {
				yetty_rpc_write_response_bool(
					&wbuf, msg.msgid,
					result.value.bool_value);
			}
		} else {
			yetty_rpc_write_response_error(&wbuf, msg.msgid,
						       result.error);
		}

		send_res = yetty_ipc_socket_send(client->socket, wbuf.data,
						 wbuf.len);
		if (YETTY_IS_ERR(send_res)) {
			ytrace("yrpc: failed to send response: %s",
			       send_res.error.msg);
		}
	}
}

static void process_client_data(struct yetty_rpc_server *server,
				struct yetty_rpc_client *client)
{
	/* For now, assume each recv gets a complete message */
	/* TODO: proper message framing with length prefix */
	if (client->recv_len > 0) {
		handle_message(server, client, client->recv_buf,
			       client->recv_len);
		client->recv_len = 0;
	}
}

static void close_client(struct yetty_rpc_server *server, size_t index)
{
	struct yetty_rpc_client *client = &server->clients[index];

	ytrace("yrpc: closing client %zu", index);

	yetty_ipc_socket_close(client->socket);
	client->socket = YETTY_IPC_SOCKET_INVALID;
	client->recv_len = 0;

	/* Move last client to this slot */
	if (index < server->client_count - 1) {
		server->clients[index] =
			server->clients[server->client_count - 1];
	}
	server->client_count--;
}

struct yetty_rpc_server_ptr_result yetty_rpc_server_create(void)
{
	struct yetty_rpc_server *server;

	server = calloc(1, sizeof(struct yetty_rpc_server));
	if (!server)
		return YETTY_ERR(yetty_rpc_server_ptr, "out of memory");

	return YETTY_OK(yetty_rpc_server_ptr, server);
}

void yetty_rpc_server_destroy(struct yetty_rpc_server *server)
{
	if (!server)
		return;

	yetty_rpc_server_stop(server);
	free(server);
}

struct yetty_core_void_result
yetty_rpc_server_start(struct yetty_rpc_server *server, const char *path)
{
	struct yetty_ipc_socket_result res;

	if (!server)
		return YETTY_ERR(yetty_core_void, "server is NULL");

	if (server->running)
		return YETTY_ERR(yetty_core_void, "server already running");

	res = yetty_ipc_socket_listen(path, server->socket_path);
	if (YETTY_IS_ERR(res))
		return YETTY_ERR(yetty_core_void, res.error.msg);

	server->listen_socket = res.value;
	server->running = 1;

	ytrace("yrpc: server listening on %s", server->socket_path);

	return YETTY_OK_VOID();
}

const char *
yetty_rpc_server_get_socket_path(const struct yetty_rpc_server *server)
{
	if (!server || !server->running)
		return NULL;
	return server->socket_path;
}

struct yetty_core_void_result
yetty_rpc_server_stop(struct yetty_rpc_server *server)
{
	if (!server)
		return YETTY_OK_VOID();

	/* Close all clients */
	while (server->client_count > 0) {
		close_client(server, server->client_count - 1);
	}

	/* Close listening socket */
	if (server->listen_socket) {
		yetty_ipc_socket_close(server->listen_socket);
		yetty_ipc_socket_unlink(server->socket_path);
		server->listen_socket = NULL;
	}

	server->running = 0;

	ytrace("yrpc: server stopped");

	return YETTY_OK_VOID();
}

int yetty_rpc_server_is_running(const struct yetty_rpc_server *server)
{
	return server && server->running;
}

struct yetty_core_void_result
yetty_rpc_server_register_handler(struct yetty_rpc_server *server,
				  uint32_t channel, const char *method,
				  yetty_rpc_handler_fn handler, void *userdata)
{
	struct yetty_rpc_handler_entry *entry;

	if (!server)
		return YETTY_ERR(yetty_core_void, "server is NULL");

	if (!method || !handler)
		return YETTY_ERR(yetty_core_void, "invalid arguments");

	if (server->handler_count >= MAX_HANDLERS)
		return YETTY_ERR(yetty_core_void, "too many handlers");

	/* Check for duplicate */
	if (find_handler(server, channel, method, strlen(method)))
		return YETTY_ERR(yetty_core_void, "handler already registered");

	entry = &server->handlers[server->handler_count++];
	entry->channel = channel;
	strncpy(entry->method, method, sizeof(entry->method) - 1);
	entry->handler = handler;
	entry->userdata = userdata;

	ytrace("yrpc: registered handler: channel=%u method=%s", channel,
	       method);

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_rpc_server_unregister_handler(struct yetty_rpc_server *server,
				    uint32_t channel, const char *method)
{
	struct yetty_rpc_handler_entry *entry;

	if (!server)
		return YETTY_ERR(yetty_core_void, "server is NULL");

	entry = find_handler(server, channel, method, strlen(method));
	if (!entry)
		return YETTY_ERR(yetty_core_void, "handler not found");

	/* Move last handler to this slot */
	size_t index = entry - server->handlers;
	if (index < server->handler_count - 1) {
		server->handlers[index] =
			server->handlers[server->handler_count - 1];
	}
	server->handler_count--;

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_rpc_server_poll(struct yetty_rpc_server *server, int timeout_ms)
{
	struct yetty_ipc_socket_result accept_res;
	struct yetty_core_size_result recv_res;

	if (!server || !server->running)
		return YETTY_ERR(yetty_core_void, "server not running");

#ifndef _WIN32
	/* Unix: use poll() */
	struct pollfd fds[MAX_CLIENTS + 1];
	int nfds = 0;

	/* Add listening socket */
	fds[nfds].fd = yetty_ipc_socket_get_fd(server->listen_socket);
	fds[nfds].events = POLLIN;
	nfds++;

	/* Add client sockets */
	for (size_t i = 0; i < server->client_count; i++) {
		fds[nfds].fd = yetty_ipc_socket_get_fd(server->clients[i].socket);
		fds[nfds].events = POLLIN;
		nfds++;
	}

	int ret = poll(fds, nfds, timeout_ms);
	if (ret < 0)
		return YETTY_ERR(yetty_core_void, "poll failed");

	if (ret == 0)
		return YETTY_OK_VOID(); /* timeout */

	/* Check for new connections */
	if (fds[0].revents & POLLIN) {
		accept_res = yetty_ipc_socket_accept(server->listen_socket);
		if (YETTY_IS_OK(accept_res) && accept_res.value) {
			if (server->client_count < MAX_CLIENTS) {
				struct yetty_rpc_client *client =
					&server->clients[server->client_count++];
				client->socket = accept_res.value;
				client->recv_len = 0;
				ytrace("yrpc: new client connected (total: %zu)",
				       server->client_count);
			} else {
				yetty_ipc_socket_close(accept_res.value);
				ytrace("yrpc: rejected client: too many connections");
			}
		}
	}

	/* Check clients for data */
	for (size_t i = 0; i < server->client_count; i++) {
		if (fds[i + 1].revents & (POLLIN | POLLHUP | POLLERR)) {
			struct yetty_rpc_client *client = &server->clients[i];

			recv_res = yetty_ipc_socket_recv(
				client->socket, client->recv_buf,
				RECV_BUFFER_SIZE);

			if (YETTY_IS_ERR(recv_res) ||
			    (YETTY_IS_OK(recv_res) && recv_res.value == 0 &&
			     (fds[i + 1].revents & POLLHUP))) {
				/* Connection closed or error */
				close_client(server, i);
				i--; /* Recheck this index */
			} else if (YETTY_IS_OK(recv_res) && recv_res.value > 0) {
				client->recv_len = recv_res.value;
				process_client_data(server, client);
			}
		}
	}

#else
	/* Windows: use has_data polling */
	(void)timeout_ms; /* TODO: implement timeout */

	/* Check for new connections */
	accept_res = yetty_ipc_socket_accept(server->listen_socket);
	if (YETTY_IS_OK(accept_res) && accept_res.value) {
		if (server->client_count < MAX_CLIENTS) {
			struct yetty_rpc_client *client =
				&server->clients[server->client_count++];
			client->socket = accept_res.value;
			client->recv_len = 0;
			ytrace("yrpc: new client connected (total: %zu)",
			       server->client_count);
		} else {
			yetty_ipc_socket_close(accept_res.value);
		}
	}

	/* Check clients for data */
	for (size_t i = 0; i < server->client_count; i++) {
		struct yetty_rpc_client *client = &server->clients[i];

		if (yetty_ipc_socket_has_data(client->socket)) {
			recv_res = yetty_ipc_socket_recv(
				client->socket, client->recv_buf,
				RECV_BUFFER_SIZE);

			if (YETTY_IS_ERR(recv_res)) {
				close_client(server, i);
				i--;
			} else if (recv_res.value > 0) {
				client->recv_len = recv_res.value;
				process_client_data(server, client);
			}
		}
	}
#endif

	return YETTY_OK_VOID();
}

size_t yetty_rpc_server_client_count(const struct yetty_rpc_server *server)
{
	return server ? server->client_count : 0;
}
