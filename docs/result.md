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
            struct yetty_ycore_error error; \
        }; \
    }
```

## Declaring Result Types

Each module declares its own result types in its header:

```c
/* terminal.h */
YETTY_RESULT_DECLARE(yetty_yterm_terminal, struct yetty_yterm_terminal *);
YETTY_RESULT_DECLARE(yetty_yterm_terminal_layer, struct yetty_yterm_terminal_layer *);

/* Generates:
 *   struct yetty_yterm_terminal_result
 *   struct yetty_yterm_terminal_layer_result
 */
```

## Common Result Types

Defined in `include/yetty/core/result.h`:

```c
YETTY_RESULT_DECLARE(yetty_ycore_void, int);   /* void result — success/failure only */
YETTY_RESULT_DECLARE(yetty_ycore_int, int);
YETTY_RESULT_DECLARE(yetty_ycore_size, size_t);
```

## Creating Results

```c
/* Success */
return YETTY_OK(yetty_yterm_terminal, terminal);

/* Success (void) */
return YETTY_OK_VOID();

/* Error */
return YETTY_ERR(yetty_yterm_terminal, "failed to allocate");
```

## Checking and Propagating

```c
struct yetty_yterm_terminal_result res = yetty_yterm_terminal_create(80, 24);
if (YETTY_IS_ERR(res)) {
    yerror("terminal: %s", res.error.msg);
    return YETTY_ERR(yetty_ycore_void, res.error.msg);
}
struct yetty_yterm_terminal *terminal = res.value;
```

Error propagation is explicit — each caller decides whether to handle, wrap, or forward the error.

## Header

`include/yetty/core/result.h`
