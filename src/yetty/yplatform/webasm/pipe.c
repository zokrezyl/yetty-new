/* WebAssembly platform input pipe implementation */

#include <yetty/platform/platform-input-pipe.h>
#include <yetty/ycore/event-loop.h>
#include <yetty/ycore/event.h>
#include <yetty/ycore/types.h>
#include <yetty/ytrace.h>
#include <emscripten/emscripten.h>
#include <stdlib.h>
#include <string.h>

#define PIPE_BUFFER_CAPACITY 16384

/* WebASM input pipe - uses an internal buffer instead of fd pipe */
struct webasm_platform_input_pipe {
	struct yetty_platform_input_pipe base;
	struct yetty_core_event_loop *event_loop;
	char *buffer;
	size_t buffer_size;
	size_t buffer_capacity;
	int callback_pending;
};

/* Forward declarations */
static void webasm_pipe_destroy(struct yetty_platform_input_pipe *self);
static struct yetty_core_size_result webasm_pipe_write(struct yetty_platform_input_pipe *self,
						       const void *data, size_t size);
static struct yetty_core_size_result webasm_pipe_read(struct yetty_platform_input_pipe *self,
						      void *data, size_t max_size);
static struct yetty_core_int_result webasm_pipe_read_fd(const struct yetty_platform_input_pipe *self);
static struct yetty_core_void_result webasm_pipe_set_event_loop(struct yetty_platform_input_pipe *self,
								struct yetty_core_event_loop *loop);

/* Ops table */
static const struct yetty_platform_input_pipe_ops webasm_pipe_ops = {
	.destroy = webasm_pipe_destroy,
	.write = webasm_pipe_write,
	.read = webasm_pipe_read,
	.read_fd = webasm_pipe_read_fd,
	.set_event_loop = webasm_pipe_set_event_loop,
};

/* Async callback to notify EventLoop */
static void on_data_available(void *arg)
{
	struct webasm_platform_input_pipe *pipe = arg;

	pipe->callback_pending = 0;

	if (!pipe->event_loop) {
		ywarn("webasm_pipe: on_data_available: no EventLoop");
		return;
	}

	/* Notify EventLoop - it will read and dispatch */
	struct yetty_core_event event;
	while (pipe->buffer_size >= sizeof(event)) {
		memcpy(&event, pipe->buffer, sizeof(event));
		memmove(pipe->buffer, pipe->buffer + sizeof(event),
			pipe->buffer_size - sizeof(event));
		pipe->buffer_size -= sizeof(event);

		ydebug("webasm_pipe: dispatching event type=%d", (int)event.type);
		pipe->event_loop->ops->dispatch(pipe->event_loop, &event);
	}
}

/* Implementation */

static void webasm_pipe_destroy(struct yetty_platform_input_pipe *self)
{
	struct webasm_platform_input_pipe *pipe;

	pipe = container_of(self, struct webasm_platform_input_pipe, base);
	free(pipe->buffer);
	free(pipe);
}

static struct yetty_core_size_result webasm_pipe_write(struct yetty_platform_input_pipe *self,
						       const void *data, size_t size)
{
	struct webasm_platform_input_pipe *pipe;

	pipe = container_of(self, struct webasm_platform_input_pipe, base);

	if (size == 0)
		return YETTY_OK(yetty_core_size, 0);

	/* Grow buffer if needed */
	if (pipe->buffer_size + size > pipe->buffer_capacity) {
		size_t new_cap = pipe->buffer_capacity * 2;
		char *new_buf;

		while (new_cap < pipe->buffer_size + size)
			new_cap *= 2;

		new_buf = realloc(pipe->buffer, new_cap);
		if (!new_buf)
			return YETTY_ERR(yetty_core_size, "failed to grow pipe buffer");

		pipe->buffer = new_buf;
		pipe->buffer_capacity = new_cap;
	}

	memcpy(pipe->buffer + pipe->buffer_size, data, size);
	pipe->buffer_size += size;

	/* Schedule async callback to notify EventLoop */
	if (!pipe->callback_pending && pipe->event_loop) {
		pipe->callback_pending = 1;
		emscripten_async_call(on_data_available, pipe, 0);
	}

	return YETTY_OK(yetty_core_size, size);
}

static struct yetty_core_size_result webasm_pipe_read(struct yetty_platform_input_pipe *self,
						      void *data, size_t max_size)
{
	struct webasm_platform_input_pipe *pipe;
	size_t to_read;

	pipe = container_of(self, struct webasm_platform_input_pipe, base);

	if (pipe->buffer_size == 0 || max_size == 0)
		return YETTY_OK(yetty_core_size, 0);

	to_read = (max_size < pipe->buffer_size) ? max_size : pipe->buffer_size;
	memcpy(data, pipe->buffer, to_read);
	memmove(pipe->buffer, pipe->buffer + to_read, pipe->buffer_size - to_read);
	pipe->buffer_size -= to_read;

	return YETTY_OK(yetty_core_size, to_read);
}

static struct yetty_core_int_result webasm_pipe_read_fd(const struct yetty_platform_input_pipe *self)
{
	(void)self;
	return YETTY_OK(yetty_core_int, -1);  /* No fd on webasm */
}

static struct yetty_core_void_result webasm_pipe_set_event_loop(struct yetty_platform_input_pipe *self,
								struct yetty_core_event_loop *loop)
{
	struct webasm_platform_input_pipe *pipe;

	pipe = container_of(self, struct webasm_platform_input_pipe, base);
	pipe->event_loop = loop;

	/* If there's pending data from before setEventLoop was called, schedule callback now */
	if (pipe->event_loop && pipe->buffer_size > 0 && !pipe->callback_pending) {
		pipe->callback_pending = 1;
		emscripten_async_call(on_data_available, pipe, 0);
	}

	return YETTY_OK_VOID();
}

/* Create function */

struct yetty_platform_input_pipe_result yetty_platform_input_pipe_create(void)
{
	struct webasm_platform_input_pipe *pipe;

	pipe = calloc(1, sizeof(struct webasm_platform_input_pipe));
	if (!pipe)
		return YETTY_ERR(yetty_platform_input_pipe, "failed to allocate webasm input pipe");

	pipe->base.ops = &webasm_pipe_ops;
	pipe->buffer_capacity = PIPE_BUFFER_CAPACITY;
	pipe->buffer = malloc(pipe->buffer_capacity);
	if (!pipe->buffer) {
		free(pipe);
		return YETTY_ERR(yetty_platform_input_pipe, "failed to allocate pipe buffer");
	}

	return YETTY_OK(yetty_platform_input_pipe, &pipe->base);
}
