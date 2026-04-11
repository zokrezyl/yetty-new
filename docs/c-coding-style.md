# C Coding Style

Yetty C code follows Linux kernel coding guidelines.

## Naming

- **Types**: `struct yetty_module_thing` (no `_t` suffix)
- **Functions**: `yetty_module_thing_action()`
- **Constants/Enums**: `YETTY_MODULE_CONSTANT`
- **Variables**: `snake_case`

Full hierarchy in names: `project_module_component_subcomponent`

```c
struct yetty_term_terminal
struct yetty_term_terminal_layer
struct yetty_term_terminal_layer_ops
yetty_term_terminal_create()
yetty_term_terminal_layer_create()
```

## No Typedefs for Structs

Always use explicit `struct`:

```c
/* Good */
struct yetty_term_terminal_layer *layer;

/* Bad */
yetty_term_terminal_layer_t *layer;
```

## Polymorphism

### Vtable Pattern

Separate ops struct from object struct:

```c
/* Vtable */
struct yetty_term_terminal_layer_ops {
    void (*destroy)(struct yetty_term_terminal_layer *self);
    void (*write)(struct yetty_term_terminal_layer *self, const char *data, size_t len);
    void (*resize)(struct yetty_term_terminal_layer *self, uint32_t cols, uint32_t rows);
};

/* Object */
struct yetty_term_terminal_layer {
    const struct yetty_term_terminal_layer_ops *ops;
    uint32_t cols;
    uint32_t rows;
    float cell_width;
    float cell_height;
    int dirty;
};
```

### Structural Embedding (Subclassing)

Embed base struct as FIRST member. No `void *priv` pointers.

```c
/* Base */
struct yetty_term_terminal_layer {
    const struct yetty_term_terminal_layer_ops *ops;
    uint32_t cols;
    uint32_t rows;
    float cell_width;
    float cell_height;
    int dirty;
};

/* Subclass */
struct yetty_term_terminal_text_layer {
    struct yetty_term_terminal_layer base;  /* MUST be first */
    VTerm *vterm;
    VTermScreen *screen;
};
```

### Casting

```c
/* Upcast - direct */
struct yetty_term_terminal_layer *layer = &text_layer->base;

/* Downcast - container_of */
struct yetty_term_terminal_text_layer *text_layer =
    container_of(layer, struct yetty_term_terminal_text_layer, base);
```

### Usage

```c
layer->ops->write(layer, data, len);
layer->ops->resize(layer, 80, 24);
```

## Pointers

Space after `*`, attached to variable name:

```c
/* Good */
struct yetty_term_terminal_layer *layer;
const char *data;

/* Bad */
struct yetty_term_terminal_layer* layer;
```

## Memory Allocation

Use `calloc` for zeroed memory. Use explicit struct name in sizeof:

```c
/* Good */
layer = calloc(1, sizeof(struct yetty_term_terminal_text_layer));

/* Bad */
layer = calloc(1, sizeof(*layer));
layer = malloc(sizeof(struct yetty_term_terminal_text_layer));
```

## Result Types

Each module declares its own result types using `YETTY_RESULT_DECLARE`:

```c
/* In terminal.h */
YETTY_RESULT_DECLARE(yetty_term_terminal, struct yetty_term_terminal *);
YETTY_RESULT_DECLARE(yetty_term_terminal_layer, struct yetty_term_terminal_layer *);

/* Generates: struct yetty_term_terminal_result, struct yetty_term_terminal_layer_result */

/* Usage */
struct yetty_term_terminal_result yetty_term_terminal_create(uint32_t cols, uint32_t rows)
{
    struct yetty_term_terminal *terminal;

    terminal = calloc(1, sizeof(struct yetty_term_terminal));
    if (!terminal)
        return YETTY_ERR(yetty_term_terminal, YETTY_ERR_NOMEM, "allocation failed");

    return YETTY_OK(yetty_term_terminal, terminal);
}

/* Checking results */
struct yetty_term_terminal_result res = yetty_term_terminal_create(80, 24);
if (YETTY_IS_ERR(res)) {
    printf("error: %s\n", res.error.msg);
    return;
}
struct yetty_term_terminal *terminal = res.value;
```
