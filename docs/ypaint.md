# ypaint

Pure C implementation of ypaint for FFI support with other programming languages.

## Rolling Offset Optimization

In scrolling mode (terminal), lines scroll off the top. Naive approach updates every primitive's row on scroll - O(n) where n is primitive count.

### The Optimization

1. **Rolling row counter** - A `uint32_t` that only increments, never decrements. Starts at 0, increases by 1 for each new line since terminal start.

2. **Primitive stores rolling row at creation** - First word of primitive data:
   ```
   packed_offset = col | (rolling_row << 16)
   ```
   Where `rolling_row` is the absolute rolling position when primitive was created.

3. **Row origin uniform** - Single `uint32_t` passed to GPU: the rolling row of the top visible line.

4. **GPU computes visible position:**
   ```wgsl
   let prim_rolling_row = (prim_data[0] >> 16u) & 0xFFFFu;
   let visible_row = prim_rolling_row - uniforms.row_origin;
   ```

### Scroll Complexity

- **O(1) scroll** - Just increment `row_origin` uniform by number of scrolled lines
- **Zero primitive updates** - All primitives keep their original rolling_row
- Pop lines from front of line storage (for memory, scroll-back TBD)

### Uniform

| Name | Type | Description |
|------|------|-------------|
| `row_origin` | `u32` | Rolling row of top visible line |

## Buffer Layout

Each primitive is serialized to a GPU buffer as consecutive 32-bit words:

```
[packed_offset][type][attrs][style][geometry...]
```

Where:
- `packed_offset` = `col | (rolling_row << 16)` - grid position with rolling row
- `type` = primitive type for shader dispatch
- `attrs` = rendering attributes
- `style` = fill/stroke colors and width
- `geometry` = primitive-specific SDF parameters

## Common Structs

### struct yetty_ypaint_type

Primitive type identifier for shader dispatch:

```c
struct yetty_ypaint_type {
    uint32_t type;
};
```

### struct yetty_ypaint_attrs

Rendering attributes (not used by SDF functions):

```c
struct yetty_ypaint_attrs {
    uint32_t z_order;
};
```

### struct yetty_ypaint_style

Common rendering parameters for all primitives (used after SDF evaluation):

```c
struct yetty_ypaint_style {
    uint32_t fill_color;
    uint32_t stroke_color;
    float stroke_width;
};
```

## Geometry Structs

Only contain SDF-specific parameters. Examples:

```c
struct yetty_ypaint_circle {
    float cx;
    float cy;
    float r;
};

struct yetty_ypaint_box {
    float cx;
    float cy;
    float hw;
    float hh;
    float round;
};
```

## Shader Access

Buffer is `array<f32>` in WGSL. Shader reads at fixed offsets:

```
[0] packed_offset - col | (rolling_row << 16)
[1] type          - bitcast<u32> for dispatch
[2] z_order       - rendering order
[3] fill_color    - bitcast<u32>
[4] stroke_color  - bitcast<u32>
[5] stroke_width  - f32
[6+] geometry     - primitive-specific args
```

Shader:
1. Extracts `rolling_row` from `packed_offset`
2. Computes `visible_row = rolling_row - uniforms.row_origin`
3. Dispatches based on type
4. Extracts geometry at known offsets
5. Calls `sd_xxx()` functions

## Canvas

The canvas manages a spatial grid for GPU culling:

- Lines stored in deque (front = top, back = bottom)
- Each line contains primitives whose base (bottom edge) is on that line
- Grid cells store references to overlapping primitives
- Supports scrolling mode (cursor-relative) and static mode (absolute coords)

### struct yetty_ypaint_canvas

Base interface with ops for:
- Configuration (scene bounds, cell size)
- Primitive management (add, clear)
- Scrolling (scroll_lines)
- GPU staging (rebuild_packed_grid, grid_staging)

### Rolling Row Tracking

Canvas tracks:
- `rolling_row` - current rolling row (increments with each new line)
- `row_origin` - rolling row of top visible line (for uniform)

On scroll:
1. Pop N lines from front
2. `row_origin += N`
3. Done
