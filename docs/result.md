# Result Types

Yetty uses typed result unions for error propagation — no exceptions, no errno.

## YETTY_RESULT_DECLARE

Generates a type-specific result struct containing a tagged union of value or error:

```c
#define YETTY_RESULT_DECLARE(name, value_type) \
    struct name##_result { \
        int ok; \
        union { \
            value_type value; \
            struct yetty_core_error error; \
        }; \
    }
```

## Declaring Result Types

Each module declares its own result types in its header:

```c
/* terminal.h */
YETTY_RESULT_DECLARE(yetty_term_terminal, struct yetty_term_terminal *);
YETTY_RESULT_DECLARE(yetty_term_terminal_layer, struct yetty_term_terminal_layer *);

/* Generates:
 *   struct yetty_term_terminal_result
 *   struct yetty_term_terminal_layer_result
 */
```

## Common Result Types

Defined in `include/yetty/core/result.h`:

```c
YETTY_RESULT_DECLARE(yetty_core_void, int);   /* void result — success/failure only */
YETTY_RESULT_DECLARE(yetty_core_int, int);
YETTY_RESULT_DECLARE(yetty_core_size, size_t);
```

## Creating Results

```c
/* Success */
return YETTY_OK(yetty_term_terminal, terminal);

/* Success (void) */
return YETTY_OK_VOID();

/* Error */
return YETTY_ERR(yetty_term_terminal, "failed to allocate");
```

## Checking and Propagating

```c
struct yetty_term_terminal_result res = yetty_term_terminal_create(80, 24);
if (YETTY_IS_ERR(res)) {
    yerror("terminal: %s", res.error.msg);
    return YETTY_ERR(yetty_core_void, res.error.msg);
}
struct yetty_term_terminal *terminal = res.value;
```

Error propagation is explicit — each caller decides whether to handle, wrap, or forward the error.

## Header

`include/yetty/core/result.h`
