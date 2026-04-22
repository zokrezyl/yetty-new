# C Coding Style

Yetty C code follows Linux kernel coding guidelines.

## Naming

- **Types**: `struct yetty_module_thing` (no `_t` suffix)
- **Functions**: `yetty_module_thing_action()`
- **Constants/Enums**: `YETTY_MODULE_CONSTANT`
- **Variables**: `snake_case`

Full hierarchy in names: `project_module_component_subcomponent`

```c
struct yetty_yterm_terminal
struct yetty_yterm_terminal_layer
struct yetty_yterm_terminal_layer_ops
yetty_yterm_terminal_create()
yetty_yterm_terminal_layer_create()
```

## No Typedefs for Structs

Always use explicit `struct`:

```c
/* Good */
struct yetty_yterm_terminal_layer *layer;

/* Bad */
yetty_yterm_terminal_layer_t *layer;
```

## Polymorphism

### Vtable Pattern

Separate ops struct from object struct:

```c
/* Vtable */
struct yetty_yterm_terminal_layer_ops {
    void (*destroy)(struct yetty_yterm_terminal_layer *self);
    void (*write)(struct yetty_yterm_terminal_layer *self, const char *data, size_t len);
    void (*resize)(struct yetty_yterm_terminal_layer *self, uint32_t cols, uint32_t rows);
};

/* Object */
struct yetty_yterm_terminal_layer {
    const struct yetty_yterm_terminal_layer_ops *ops;
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
struct yetty_yterm_terminal_layer {
    const struct yetty_yterm_terminal_layer_ops *ops;
    uint32_t cols;
    uint32_t rows;
    float cell_width;
    float cell_height;
    int dirty;
};

/* Subclass */
struct yetty_yterm_terminal_text_layer {
    struct yetty_yterm_terminal_layer base;  /* MUST be first */
    VTerm *vterm;
    VTermScreen *screen;
};
```

### Casting

```c
/* Upcast - direct */
struct yetty_yterm_terminal_layer *layer = &text_layer->base;

/* Downcast - container_of */
struct yetty_yterm_terminal_text_layer *text_layer =
    container_of(layer, struct yetty_yterm_terminal_text_layer, base);
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
struct yetty_yterm_terminal_layer *layer;
const char *data;

/* Bad */
struct yetty_yterm_terminal_layer* layer;
```

## Memory Allocation

Use `calloc` for zeroed memory. Use explicit struct name in sizeof:

```c
/* Good */
layer = calloc(1, sizeof(struct yetty_yterm_terminal_text_layer));

/* Bad */
layer = calloc(1, sizeof(*layer));
layer = malloc(sizeof(struct yetty_yterm_terminal_text_layer));
```

## Object Lifecycle: create/destroy

Objects follow the `create`/`destroy` pattern:

```c
/* Creation - returns result type */
struct yetty_thing_result yetty_thing_create(...);

/* Destruction - void, handles NULL, propagates to children */
void yetty_thing_destroy(struct yetty_thing *thing);
```

### destroy Rules

1. **Handle NULL**: `destroy(NULL)` must be safe (no-op)
2. **Propagate**: destroy children before freeing self
3. **Idempotent**: safe to call multiple times
4. **No return value**: void, errors logged but not returned

```c
void yetty_yterm_terminal_destroy(struct yetty_yterm_terminal *terminal)
{
    if (!terminal)
        return;

    /* Destroy children first */
    for (size_t i = 0; i < terminal->layer_count; i++) {
        if (terminal->layers[i])
            terminal->layers[i]->ops->destroy(terminal->layers[i]);
    }

    /* Destroy owned resources */
    if (terminal->blender)
        terminal->blender->ops->destroy(terminal->blender);

    /* Free self */
    free(terminal);
}
```

### SHUTDOWN Event

`YETTY_EVENT_SHUTDOWN` triggers graceful destroy chain:

```
Window close → SHUTDOWN event → event_loop stops → caller destroys terminal
```

## Result Types

**See [docs/result.md](result.md) for full documentation.**

**Rule: Any C function that can error must return a Result type, even if the success value is void.**

For void functions that can fail, use `struct yetty_core_void_result`:

```c
struct yetty_core_void_result yetty_thing_init(struct yetty_thing *thing)
{
    if (!thing)
        return YETTY_ERR(yetty_core_void, "thing is NULL");

    /* ... initialization ... */

    return YETTY_OK_VOID();
}
```

Each module declares its own result types using `YETTY_RESULT_DECLARE`:

```c
/* In terminal.h */
YETTY_RESULT_DECLARE(yetty_yterm_terminal, struct yetty_yterm_terminal *);
YETTY_RESULT_DECLARE(yetty_yterm_terminal_layer, struct yetty_yterm_terminal_layer *);

/* Generates: struct yetty_yterm_terminal_result, struct yetty_yterm_terminal_layer_result */

/* Usage */
struct yetty_yterm_terminal_result yetty_yterm_terminal_create(uint32_t cols, uint32_t rows)
{
    struct yetty_yterm_terminal *terminal;

    terminal = calloc(1, sizeof(struct yetty_yterm_terminal));
    if (!terminal)
        return YETTY_ERR(yetty_yterm_terminal, YETTY_ERR_NOMEM, "allocation failed");

    return YETTY_OK(yetty_yterm_terminal, terminal);
}

/* Checking results */
struct yetty_yterm_terminal_result res = yetty_yterm_terminal_create(80, 24);
if (YETTY_IS_ERR(res)) {
    printf("error: %s\n", res.error.msg);
    return;
}
struct yetty_yterm_terminal *terminal = res.value;
```
