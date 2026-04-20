#include <yetty/ycore/types.h>
#include <stdlib.h>
#include <string.h>

struct yetty_core_buffer_result yetty_core_buffer_create(size_t initial_capacity)
{
	struct yetty_core_buffer buf = {0};
	if (initial_capacity > 0) {
		buf.data = malloc(initial_capacity);
		if (!buf.data)
			return YETTY_ERR(yetty_core_buffer, "malloc failed");
		buf.capacity = initial_capacity;
	}
	return YETTY_OK(yetty_core_buffer, buf);
}

void yetty_core_buffer_destroy(struct yetty_core_buffer *buf)
{
	if (!buf)
		return;
	free(buf->data);
	buf->data = NULL;
	buf->size = 0;
	buf->capacity = 0;
}

void yetty_core_buffer_clear(struct yetty_core_buffer *buf)
{
	if (buf)
		buf->size = 0;
}

struct yetty_core_void_result yetty_core_buffer_append(
    struct yetty_core_buffer *buf, const struct yetty_core_buffer *src)
{
	if (!buf)
		return YETTY_ERR(yetty_core_void, "buf is NULL");
	if (!src || src->size == 0)
		return YETTY_OK_VOID();

	size_t needed = buf->size + src->size;
	if (needed > buf->capacity) {
		size_t new_cap = buf->capacity ? buf->capacity : 64;
		while (new_cap < needed)
			new_cap *= 2;
		uint8_t *new_data = realloc(buf->data, new_cap);
		if (!new_data)
			return YETTY_ERR(yetty_core_void, "realloc failed");
		buf->data = new_data;
		buf->capacity = new_cap;
	}

	memcpy(buf->data + buf->size, src->data, src->size);
	buf->size += src->size;
	return YETTY_OK_VOID();
}
