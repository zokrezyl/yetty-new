/* WebAssembly event loop using emscripten main loop */

#include <yetty/ycore/event-loop.h>
#include <yetty/ycore/types.h>
#include <yetty/platform/platform-input-pipe.h>
#include <yetty/platform/pty-poll-source.h>
#include <yetty/ytrace.h>
#include "webasm-pty-poll-source.h"
#include <emscripten/emscripten.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LISTENERS_PER_POLL 16
#define MAX_LISTENERS_PER_TIMER 16
#define MAX_LISTENERS_PER_TYPE 64
#define MAX_POLLS 256
#define MAX_TIMERS 64

struct webasm_event_loop;

struct poll_handle {
	int id;
	int active;
	struct yetty_core_event_listener *listeners[MAX_LISTENERS_PER_POLL];
	int listener_count;
	struct webasm_event_loop *impl;
};

struct timer_handle {
	int id;
	int timeout_ms;
	int active;
	double last_fire;
	struct yetty_core_event_listener *listeners[MAX_LISTENERS_PER_TIMER];
	int listener_count;
	struct webasm_event_loop *impl;
};

struct prioritized_listener {
	struct yetty_core_event_listener *listener;
	int priority;
};

struct webasm_event_loop {
	struct yetty_core_event_loop base;

	struct prioritized_listener listeners[YETTY_EVENT_COUNT][MAX_LISTENERS_PER_TYPE];
	int listener_counts[YETTY_EVENT_COUNT];

	struct poll_handle polls[MAX_POLLS];
	int next_poll_id;

	struct timer_handle timers[MAX_TIMERS];
	int next_timer_id;

	struct yetty_platform_input_pipe *platform_input_pipe;
	int running;
};

/* Forward declarations */
static void webasm_destroy(struct yetty_core_event_loop *self);
static struct yetty_core_void_result webasm_start(struct yetty_core_event_loop *self);
static struct yetty_core_void_result webasm_stop(struct yetty_core_event_loop *self);
static struct yetty_core_void_result webasm_register_listener(
	struct yetty_core_event_loop *self, enum yetty_core_event_type type,
	struct yetty_core_event_listener *listener, int priority);
static struct yetty_core_void_result webasm_deregister_listener(
	struct yetty_core_event_loop *self, enum yetty_core_event_type type,
	struct yetty_core_event_listener *listener);
static struct yetty_core_int_result webasm_dispatch(
	struct yetty_core_event_loop *self, const struct yetty_core_event *event);
static struct yetty_core_void_result webasm_broadcast(
	struct yetty_core_event_loop *self, const struct yetty_core_event *event);
static struct yetty_core_poll_id_result webasm_create_poll(struct yetty_core_event_loop *self);
static struct yetty_core_poll_id_result webasm_create_pty_poll(
	struct yetty_core_event_loop *self, struct yetty_platform_pty_poll_source *source);
static struct yetty_core_void_result webasm_config_poll(
	struct yetty_core_event_loop *self, yetty_core_poll_id id, int fd);
static struct yetty_core_void_result webasm_start_poll(
	struct yetty_core_event_loop *self, yetty_core_poll_id id, int events);
static struct yetty_core_void_result webasm_stop_poll(
	struct yetty_core_event_loop *self, yetty_core_poll_id id);
static struct yetty_core_void_result webasm_destroy_poll(
	struct yetty_core_event_loop *self, yetty_core_poll_id id);
static struct yetty_core_void_result webasm_register_poll_listener(
	struct yetty_core_event_loop *self, yetty_core_poll_id id,
	struct yetty_core_event_listener *listener);
static struct yetty_core_timer_id_result webasm_create_timer(struct yetty_core_event_loop *self);
static struct yetty_core_void_result webasm_config_timer(
	struct yetty_core_event_loop *self, yetty_core_timer_id id, int timeout_ms);
static struct yetty_core_void_result webasm_start_timer(
	struct yetty_core_event_loop *self, yetty_core_timer_id id);
static struct yetty_core_void_result webasm_stop_timer(
	struct yetty_core_event_loop *self, yetty_core_timer_id id);
static struct yetty_core_void_result webasm_destroy_timer(
	struct yetty_core_event_loop *self, yetty_core_timer_id id);
static struct yetty_core_void_result webasm_register_timer_listener(
	struct yetty_core_event_loop *self, yetty_core_timer_id id,
	struct yetty_core_event_listener *listener);
static void webasm_request_render(struct yetty_core_event_loop *self);

static const struct yetty_core_event_loop_ops webasm_ops = {
	.destroy = webasm_destroy,
	.start = webasm_start,
	.stop = webasm_stop,
	.register_listener = webasm_register_listener,
	.deregister_listener = webasm_deregister_listener,
	.dispatch = webasm_dispatch,
	.broadcast = webasm_broadcast,
	.create_poll = webasm_create_poll,
	.create_pty_poll = webasm_create_pty_poll,
	.config_poll = webasm_config_poll,
	.start_poll = webasm_start_poll,
	.stop_poll = webasm_stop_poll,
	.destroy_poll = webasm_destroy_poll,
	.register_poll_listener = webasm_register_poll_listener,
	.create_timer = webasm_create_timer,
	.config_timer = webasm_config_timer,
	.start_timer = webasm_start_timer,
	.stop_timer = webasm_stop_timer,
	.destroy_timer = webasm_destroy_timer,
	.register_timer_listener = webasm_register_timer_listener,
	.request_render = webasm_request_render,
};

/* Main loop tick - fires timers */
static void main_loop_tick(void *arg)
{
	struct webasm_event_loop *impl = arg;
	double now;
	int i, j;

	if (!impl->running)
		return;

	now = emscripten_get_now();

	/* Fire timers that are due */
	for (i = 0; i < impl->next_timer_id; i++) {
		struct timer_handle *th = &impl->timers[i];
		double elapsed;

		if (!th->active)
			continue;

		elapsed = now - th->last_fire;
		if (elapsed >= th->timeout_ms) {
			struct yetty_core_event event = {0};

			th->last_fire = now;
			event.type = YETTY_EVENT_TIMER;
			event.timer.timer_id = th->id;

			for (j = 0; j < th->listener_count; j++)
				th->listeners[j]->handler(th->listeners[j], &event);
		}
	}
}

/* Implementation */

static void webasm_destroy(struct yetty_core_event_loop *self)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);
	free(impl);
}

static struct yetty_core_void_result webasm_start(struct yetty_core_event_loop *self)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);

	ydebug("webasm_event_loop: start - setting up emscripten main loop");
	impl->running = 1;

	/* Set up pipe notification if pipe provided */
	if (impl->platform_input_pipe) {
		impl->platform_input_pipe->ops->set_event_loop(impl->platform_input_pipe, self);
		ydebug("webasm_event_loop: pipe notification set up");
	}

	/* Set up browser main loop - runs at ~60fps via requestAnimationFrame */
	emscripten_set_main_loop_arg(main_loop_tick, impl, 0, 1);
	/* Note: emscripten_set_main_loop with simulate_infinite_loop=1 doesn't return */

	return YETTY_OK_VOID();
}

static struct yetty_core_void_result webasm_stop(struct yetty_core_event_loop *self)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);

	ydebug("webasm_event_loop: stop");
	impl->running = 0;
	emscripten_cancel_main_loop();

	return YETTY_OK_VOID();
}

static struct yetty_core_void_result webasm_register_listener(
	struct yetty_core_event_loop *self, enum yetty_core_event_type type,
	struct yetty_core_event_listener *listener, int priority)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);
	int count, i, insert_pos;

	if (!listener || type >= YETTY_EVENT_COUNT)
		return YETTY_ERR(yetty_core_void, "invalid listener or type");

	count = impl->listener_counts[type];
	if (count >= MAX_LISTENERS_PER_TYPE)
		return YETTY_ERR(yetty_core_void, "too many listeners");

	/* Find insert position (sorted by priority descending) */
	insert_pos = count;
	for (i = 0; i < count; i++) {
		if (impl->listeners[type][i].priority < priority) {
			insert_pos = i;
			break;
		}
	}

	/* Shift elements */
	for (i = count; i > insert_pos; i--)
		impl->listeners[type][i] = impl->listeners[type][i - 1];

	impl->listeners[type][insert_pos].listener = listener;
	impl->listeners[type][insert_pos].priority = priority;
	impl->listener_counts[type]++;

	return YETTY_OK_VOID();
}

static struct yetty_core_void_result webasm_deregister_listener(
	struct yetty_core_event_loop *self, enum yetty_core_event_type type,
	struct yetty_core_event_listener *listener)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);
	int count, i, j;

	if (type >= YETTY_EVENT_COUNT)
		return YETTY_OK_VOID();

	count = impl->listener_counts[type];
	for (i = 0; i < count; i++) {
		if (impl->listeners[type][i].listener == listener) {
			for (j = i; j < count - 1; j++)
				impl->listeners[type][j] = impl->listeners[type][j + 1];
			impl->listener_counts[type]--;
			break;
		}
	}

	return YETTY_OK_VOID();
}

static struct yetty_core_int_result webasm_dispatch(
	struct yetty_core_event_loop *self, const struct yetty_core_event *event)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);
	int count, i;

	if (event->type >= YETTY_EVENT_COUNT)
		return YETTY_OK(yetty_core_int, 0);

	count = impl->listener_counts[event->type];
	for (i = 0; i < count; i++) {
		struct yetty_core_event_listener *listener = impl->listeners[event->type][i].listener;
		struct yetty_core_int_result res = listener->handler(listener, event);

		if (!YETTY_IS_OK(res))
			return YETTY_ERR(yetty_core_int, res.error.msg);
		if (res.value)
			return YETTY_OK(yetty_core_int, 1);
	}

	return YETTY_OK(yetty_core_int, 0);
}

static struct yetty_core_void_result webasm_broadcast(
	struct yetty_core_event_loop *self, const struct yetty_core_event *event)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);
	int t, count, i;

	for (t = 0; t < YETTY_EVENT_COUNT; t++) {
		count = impl->listener_counts[t];
		for (i = 0; i < count; i++) {
			struct yetty_core_event_listener *listener = impl->listeners[t][i].listener;
			struct yetty_core_int_result res = listener->handler(listener, event);

			if (!YETTY_IS_OK(res))
				return YETTY_ERR(yetty_core_void, res.error.msg);
		}
	}

	return YETTY_OK_VOID();
}

/* Poll - callback-based on webasm (no fd polling) */

static struct yetty_core_poll_id_result webasm_create_poll(struct yetty_core_event_loop *self)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);
	int id = impl->next_poll_id++;

	if (id >= MAX_POLLS)
		return YETTY_ERR(yetty_core_poll_id, "too many polls");

	memset(&impl->polls[id], 0, sizeof(impl->polls[id]));
	impl->polls[id].id = id;
	impl->polls[id].impl = impl;

	return YETTY_OK(yetty_core_poll_id, id);
}

/* Callback for pty poll source notification */
static void on_pty_data_available(void *user_data)
{
	struct poll_handle *ph = user_data;
	struct yetty_core_event event = {0};
	int i;

	event.type = YETTY_EVENT_POLL_READABLE;
	event.poll.fd = ph->id;

	for (i = 0; i < ph->listener_count; i++)
		ph->listeners[i]->handler(ph->listeners[i], &event);
}

static struct yetty_core_poll_id_result webasm_create_pty_poll(
	struct yetty_core_event_loop *self, struct yetty_platform_pty_poll_source *source)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);
	struct webasm_pty_poll_source *webasm_source;
	struct poll_handle *ph;
	int id;

	id = impl->next_poll_id++;
	if (id >= MAX_POLLS)
		return YETTY_ERR(yetty_core_poll_id, "too many polls");

	ph = &impl->polls[id];
	memset(ph, 0, sizeof(*ph));
	ph->id = id;
	ph->impl = impl;

	/* Cast to WebASM poll source and set callback */
	webasm_source = (struct webasm_pty_poll_source *)source;
	webasm_pty_poll_source_set_callback(webasm_source, on_pty_data_available, ph);

	return YETTY_OK(yetty_core_poll_id, id);
}

static struct yetty_core_void_result webasm_config_poll(
	struct yetty_core_event_loop *self, yetty_core_poll_id id, int fd)
{
	(void)self;
	(void)id;
	(void)fd;
	return YETTY_OK_VOID();  /* No-op on webasm */
}

static struct yetty_core_void_result webasm_start_poll(
	struct yetty_core_event_loop *self, yetty_core_poll_id id, int events)
{
	(void)self;
	(void)id;
	(void)events;
	return YETTY_OK_VOID();  /* No-op on webasm - callbacks handle notification */
}

static struct yetty_core_void_result webasm_stop_poll(
	struct yetty_core_event_loop *self, yetty_core_poll_id id)
{
	(void)self;
	(void)id;
	return YETTY_OK_VOID();  /* No-op on webasm */
}

static struct yetty_core_void_result webasm_destroy_poll(
	struct yetty_core_event_loop *self, yetty_core_poll_id id)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);

	if (id < 0 || id >= MAX_POLLS)
		return YETTY_ERR(yetty_core_void, "invalid poll id");

	impl->polls[id].active = 0;
	impl->polls[id].listener_count = 0;

	return YETTY_OK_VOID();
}

static struct yetty_core_void_result webasm_register_poll_listener(
	struct yetty_core_event_loop *self, yetty_core_poll_id id,
	struct yetty_core_event_listener *listener)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);
	struct poll_handle *ph;

	if (id < 0 || id >= MAX_POLLS || !listener)
		return YETTY_ERR(yetty_core_void, "invalid poll id or listener");

	ph = &impl->polls[id];
	if (ph->listener_count >= MAX_LISTENERS_PER_POLL)
		return YETTY_ERR(yetty_core_void, "too many poll listeners");

	ph->listeners[ph->listener_count++] = listener;

	return YETTY_OK_VOID();
}

/* Timer */

static struct yetty_core_timer_id_result webasm_create_timer(struct yetty_core_event_loop *self)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);
	int id = impl->next_timer_id++;
	struct timer_handle *th;

	if (id >= MAX_TIMERS)
		return YETTY_ERR(yetty_core_timer_id, "too many timers");

	th = &impl->timers[id];
	memset(th, 0, sizeof(*th));
	th->id = id;
	th->impl = impl;

	ydebug("webasm_event_loop: create_timer id=%d", id);

	return YETTY_OK(yetty_core_timer_id, id);
}

static struct yetty_core_void_result webasm_config_timer(
	struct yetty_core_event_loop *self, yetty_core_timer_id id, int timeout_ms)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);

	if (id < 0 || id >= MAX_TIMERS)
		return YETTY_ERR(yetty_core_void, "invalid timer id");

	impl->timers[id].timeout_ms = timeout_ms;
	ydebug("webasm_event_loop: config_timer id=%d timeout=%d", id, timeout_ms);

	return YETTY_OK_VOID();
}

static struct yetty_core_void_result webasm_start_timer(
	struct yetty_core_event_loop *self, yetty_core_timer_id id)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);

	if (id < 0 || id >= MAX_TIMERS)
		return YETTY_ERR(yetty_core_void, "invalid timer id");

	impl->timers[id].active = 1;
	impl->timers[id].last_fire = emscripten_get_now();
	ydebug("webasm_event_loop: start_timer id=%d", id);

	return YETTY_OK_VOID();
}

static struct yetty_core_void_result webasm_stop_timer(
	struct yetty_core_event_loop *self, yetty_core_timer_id id)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);

	if (id < 0 || id >= MAX_TIMERS)
		return YETTY_ERR(yetty_core_void, "invalid timer id");

	impl->timers[id].active = 0;
	ydebug("webasm_event_loop: stop_timer id=%d", id);

	return YETTY_OK_VOID();
}

static struct yetty_core_void_result webasm_destroy_timer(
	struct yetty_core_event_loop *self, yetty_core_timer_id id)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);

	if (id < 0 || id >= MAX_TIMERS)
		return YETTY_ERR(yetty_core_void, "invalid timer id");

	impl->timers[id].active = 0;
	ydebug("webasm_event_loop: destroy_timer id=%d", id);

	return YETTY_OK_VOID();
}

static struct yetty_core_void_result webasm_register_timer_listener(
	struct yetty_core_event_loop *self, yetty_core_timer_id id,
	struct yetty_core_event_listener *listener)
{
	struct webasm_event_loop *impl = container_of(self, struct webasm_event_loop, base);
	struct timer_handle *th;

	if (id < 0 || id >= MAX_TIMERS || !listener)
		return YETTY_ERR(yetty_core_void, "invalid timer id or listener");

	th = &impl->timers[id];
	if (th->listener_count >= MAX_LISTENERS_PER_TIMER)
		return YETTY_ERR(yetty_core_void, "too many timer listeners");

	th->listeners[th->listener_count++] = listener;
	ydebug("webasm_event_loop: register_timer_listener id=%d", id);

	return YETTY_OK_VOID();
}

static void webasm_request_render(struct yetty_core_event_loop *self)
{
	struct yetty_core_event event = {0};

	event.type = YETTY_EVENT_RENDER;
	webasm_dispatch(self, &event);
}

/* Create */

struct yetty_core_event_loop_result yetty_core_event_loop_create(
	struct yetty_platform_input_pipe *pipe)
{
	struct webasm_event_loop *impl;

	impl = calloc(1, sizeof(struct webasm_event_loop));
	if (!impl)
		return YETTY_ERR(yetty_core_event_loop, "failed to allocate event loop");

	impl->base.ops = &webasm_ops;
	impl->platform_input_pipe = pipe;

	return YETTY_OK(yetty_core_event_loop, &impl->base);
}
