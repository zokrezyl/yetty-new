#ifndef YETTY_TERM_TERMINAL_H
#define YETTY_TERM_TERMINAL_H

#include <stddef.h>
#include <stdint.h>
#include <yetty/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_render_gpu_resource_set;

/* Forward declarations */
struct yetty_term_terminal;
struct yetty_term_terminal_layer;
struct yetty_term_terminal_layer_ops;

/* Result types */
YETTY_RESULT_DECLARE(yetty_term_terminal, struct yetty_term_terminal *);
YETTY_RESULT_DECLARE(yetty_term_terminal_layer, struct yetty_term_terminal_layer *);

/* Layer ops */
struct yetty_term_terminal_layer_ops {
    void (*destroy)(struct yetty_term_terminal_layer *self);
    void (*write)(struct yetty_term_terminal_layer *self, const char *data, size_t len);
    void (*resize)(struct yetty_term_terminal_layer *self, uint32_t cols, uint32_t rows);
    struct yetty_render_gpu_resource_set (*get_gpu_resource_set)(const struct yetty_term_terminal_layer *self);
};

/* Layer base - embed as first member in subclasses */
struct yetty_term_terminal_layer {
    const struct yetty_term_terminal_layer_ops *ops;
    uint32_t cols;
    uint32_t rows;
    float cell_width;
    float cell_height;
    int dirty;
};

/* Forward declaration */
struct yetty_platform_input_pipe;

/* Terminal creation/destruction */
struct yetty_term_terminal_result yetty_term_terminal_create(
    uint32_t cols, uint32_t rows,
    struct yetty_platform_input_pipe *platform_input_pipe);
void yetty_term_terminal_destroy(struct yetty_term_terminal *terminal);

/* Terminal run */
struct yetty_core_void_result yetty_term_terminal_run(struct yetty_term_terminal *terminal);

/* Terminal input */
void yetty_term_terminal_write(struct yetty_term_terminal *terminal, const char *data, size_t len);
void yetty_term_terminal_resize(struct yetty_term_terminal *terminal, uint32_t cols, uint32_t rows);

/* Terminal state */
uint32_t yetty_term_terminal_get_cols(const struct yetty_term_terminal *terminal);
uint32_t yetty_term_terminal_get_rows(const struct yetty_term_terminal *terminal);

/* Layer management */
void yetty_term_terminal_layer_add(struct yetty_term_terminal *terminal, struct yetty_term_terminal_layer *layer);
void yetty_term_terminal_layer_remove(struct yetty_term_terminal *terminal, struct yetty_term_terminal_layer *layer);
size_t yetty_term_terminal_layer_count(const struct yetty_term_terminal *terminal);
struct yetty_term_terminal_layer *yetty_term_terminal_layer_get(const struct yetty_term_terminal *terminal, size_t index);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_TERM_TERMINAL_H */
