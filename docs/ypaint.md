# ypaint-c

Migration of ypaint from C++ to pure C for simple FFI support with other programming languages.

## Buffer Layout

Each primitive is serialized to a GPU buffer as consecutive 32-bit words:

```
[YPaintType][YPaintAttrs][YPaintStyle][Geometry...]
```

Buffer size in bytes:
```c
sizeof(YPaintType) + sizeof(YPaintAttrs) + sizeof(YPaintStyle) + sizeof(YPaintGeometry)
```

## Common Structs

### YPaintType

Primitive type identifier for shader dispatch:

```c
typedef struct YPaintType {
    uint32_t type;
} YPaintType;
```

### YPaintAttrs

Rendering attributes (not used by SDF functions):

```c
typedef struct YPaintAttrs {
    uint32_t zOrder;
} YPaintAttrs;
```

### YPaintStyle

Common rendering parameters for all primitives (used after SDF evaluation):

```c
typedef struct YPaintStyle {
    uint32_t fillColor;
    uint32_t strokeColor;
    float strokeWidth;
} YPaintStyle;
```

## Geometry Structs

Only contain SDF-specific parameters. Examples:

```c
typedef struct YPaintCircle {
    float cx;
    float cy;
    float r;
} YPaintCircle;

typedef struct YPaintBox {
    float cx;
    float cy;
    float hw;
    float hh;
    float round;
} YPaintBox;
```

## Shader Access

Buffer is `array<f32>` in WGSL. Shader reads at fixed offsets:

```
[0] type         - bitcast<u32> for dispatch
[1] zOrder       - rendering order (not used in SDF calc)
[2] fillColor    - bitcast<u32>
[3] strokeColor  - bitcast<u32>
[4] strokeWidth  - f32
[5+] geometry    - primitive-specific args
```

Shader dispatches based on type, extracts geometry at known offsets, calls `sdXxx()` functions.
