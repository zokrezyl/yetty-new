# ypaint

Pure C implementation of ypaint for FFI support with other programming languages.

## Example: Circle Rendering with Scrolling

**INPUT (from user via OSC command):**
- cursor is at visible row 5
- cy = 30 (relative to cursor row top)
- radius = 25
- cell_height = 20
- row0_absolute = 0 (no scrolling yet)

---

**CPU (C) - Primitive Creation:**

**STEP 1: Compute AABB in relative coords (as if cursor at row 0)**
```
aabb_min_y_rel = cy - radius = 30 - 25 = 5
aabb_max_y_rel = cy + radius = 30 + 25 = 55

aabb_min_row_rel = floor(5 / 20) = 0
aabb_max_row_rel = floor(55 / 20) = 2
```

**STEP 2: Convert to actual rows**
```
aabb_min_row = cursor_row + aabb_min_row_rel = 5 + 0 = 5
aabb_max_row = cursor_row + aabb_max_row_rel = 5 + 2 = 7

storage_row = aabb_max_row = 7  (lowest row, for deletion tracking)
```

**STEP 3: Get rolling_row at insertion**
```
rolling_row = row0_absolute + cursor_row = 0 + 5 = 5
```

**STEP 4: Register primitive in spatial grid**
```
// Primitive DATA stored in storage_row=7's line (for deletion tracking)
// Grid cells in rows 5, 6, 7 get prim_ref pointing to storage_row=7
grid[row=5].add(prim_ref{lines_ahead=2, prim_index=0})
grid[row=6].add(prim_ref{lines_ahead=1, prim_index=0})
grid[row=7].add(prim_ref{lines_ahead=0, prim_index=0})
```

**STEP 5: Serialize to GPU buffer**
```
[rolling_row=5][type][z][fill][stroke][stroke_w][cx][cy=30][radius=25]
```

**IMPORTANT: Geometry coordinates (cy=30) are stored exactly as user specified. NO transformation. The shader adjusts the TEST POSITION, not the primitive coordinates.**

---

**GPU (WGSL shader) - Fragment shader per pixel:**

**STEP 6: Render pixel at screen Y=130 (circle center)**
```wgsl
pixel_pos_y = 130              // screen pixel Y coordinate
rolling_row = 5                // from buffer[prim_offset + 0]
row_origin = 0                 // uniform (rolling_row_0)

y_offset = (rolling_row - row_origin) * cell_height = (5 - 0) * 20 = 100
prim_scene_pos_y = pixel_pos_y - y_offset = 130 - 100 = 30

cy = 30                        // from primitive buffer (stored as-is)
distance = |prim_scene_pos_y - cy| - radius = |30 - 30| - 25 = -25 < 0 → INSIDE
```

**STEP 7: Render pixel at screen Y=110 (inside AABB, row 5 area)**
```wgsl
pixel_pos_y = 110
y_offset = 100                 // same primitive, same y_offset
prim_scene_pos_y = 110 - 100 = 10

distance = |10 - 30| - 25 = 20 - 25 = -5 < 0 → INSIDE
```

---

## Key Concept: No Coordinate Transformation

Geometry coordinates are stored **exactly as user specified**. The shader handles scrolling by adjusting the **test position**, not the primitive coordinates:

```wgsl
y_offset = (rolling_row - row_origin) * cell_height
prim_scene_pos_y = pixel_pos_y - y_offset
sd_circle(prim_scene_pos, vec2(cx, cy), radius)  // cy used as-is
```

This means:
- CPU does NOT transform cy
- Shader computes y_offset from rolling_row (at insertion)
- Shader adjusts test position to match primitive's coordinate system
- SDF evaluates distance from adjusted test position to stored coordinates

---

## Rolling Offset Optimization

In scrolling mode (terminal), lines scroll off the top. Naive approach updates every primitive's row on scroll - O(n) where n is primitive count.

### The Optimization

1. **Rolling row counter** - A `uint32_t` that only increments, never decrements. Starts at 0, increases by 1 for each new line since terminal start.

2. **Primitive stores rolling_row at creation** - First word of primitive data:
   ```
   [0] rolling_row
   ```
   Where `rolling_row` = row0_absolute + cursor_row (rolling row at insertion).

3. **Geometry stored as-is** - No coordinate transformation. cy, etc. stored exactly as user specified.

4. **Row origin uniform** - Single `uint32_t` passed to GPU: rolling row of top visible line.

5. **GPU adjusts test position:**
   ```wgsl
   let rolling_row = storage_buffer[prim_offset + 0u];
   let y_offset = f32(i32(rolling_row) - i32(row_origin)) * cell_height;
   let prim_scene_pos_y = pixel_pos_y - y_offset;
   // SDF evaluates: sd_xxx(prim_scene_pos, stored_coords)
   ```

### Scroll Complexity

- **O(1) scroll** - Just increment `row_origin` uniform by number of scrolled lines
- **Zero primitive updates** - All primitives keep their original rolling_row and coordinates
- Pop lines from front of line storage (for memory, scroll-back TBD)

### Uniform

| Name | Type | Description |
|------|------|-------------|
| `row_origin` | `u32` | Rolling row of top visible line |

---

## Buffer Layout

Each primitive is serialized to a GPU buffer as consecutive 32-bit words:

```
[rolling_row][type][z_order][fill_color][stroke_color][stroke_width][geometry...]
```

Where:
- `rolling_row` = rolling row at insertion (for y_offset calculation)
- `type` = primitive type for shader dispatch
- `z_order` = rendering order
- `fill_color` = packed RGBA
- `stroke_color` = packed RGBA
- `stroke_width` = f32
- `geometry` = primitive-specific SDF parameters (stored as-is, no transformation)

---

## Shader Access

Buffer is `array<u32>` in WGSL. Shader reads at fixed offsets:

```
[0] rolling_row   - u32, rolling row at insertion
[1] type          - u32, primitive type for dispatch
[2] z_order       - u32, rendering order
[3] fill_color    - u32, packed RGBA
[4] stroke_color  - u32, packed RGBA
[5] stroke_width  - f32 (bitcast)
[6+] geometry     - primitive-specific args (stored as-is)
```

Shader:
1. Reads `rolling_row` from offset 0
2. Computes `y_offset = (rolling_row - row_origin) * cell_height` (signed arithmetic)
3. Adjusts **test position**: `prim_scene_pos_y = pixel_pos_y - y_offset`
4. Dispatches based on type
5. Extracts geometry at known offsets (used as-is, no transformation)
6. Calls `sd_xxx(prim_scene_pos, ...)` - SDF evaluates adjusted test position against stored coords

---

## Canvas

The canvas manages a spatial grid for GPU culling:

- Lines stored in deque (front = top, back = bottom)
- Each line contains primitives whose base (bottom edge) is on that line
- Supports scrolling mode (cursor-relative) and static mode (absolute coords)

### Spatial Grid

Each grid cell stores an array of `prim_ref`:

```c
struct yetty_yetty_ypaint_canvas_prim_ref {
    uint16_t lines_ahead;  // relative offset to base line (0 = same line)
    uint16_t prim_index;   // index within base line's prims array
};
```

- `lines_ahead`: how many lines below to find the base line (where primitive data lives)
- `prim_index`: index into that base line's `prims` array

Primitives are stored in their **base line** (storage_row = lowest row of AABB). Grid cells in rows above reference down to the base line.

When a line scrolls off, its primitives are deleted and all refs pointing to it become invalid.

### Rolling Row Tracking

Canvas tracks:
- `next_rolling_row` - next rolling row to assign to new lines (increments)
- `row0_absolute` - rolling row of top visible line (for uniform)

On scroll:
1. Pop N lines from front
2. `row0_absolute += N`
3. Done

---

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

---

## Geometry Structs

Only contain SDF-specific parameters. **Coordinates are stored exactly as user specified - no transformation.**

Examples:

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

---

## Complex Primitives

Complex primitives (yplot, nested ypaint, images, video) require more than simple SDF evaluation. They have their own rendering logic and may contain nested content.

### Types

| Type ID | Name | Description |
|---------|------|-------------|
| 0x80000001 | FONT | Font definition (atlas + metrics) |
| 0x80000002 | TEXT_SPAN | Text with font reference |
| 0x80000003 | YPLOT | Function plot with yfsvm bytecode |
| 0x80000004+ | Reserved | Future: images, video, nested ypaint |

### Storage (Simplified vs Simple Primitives)

Unlike simple primitives which register in ALL overlapping grid cells, complex primitives use simplified storage:

**Simple primitives:**
```
grid[row=5].add(prim_ref{lines_ahead=2, prim_index=0})
grid[row=6].add(prim_ref{lines_ahead=1, prim_index=0})
grid[row=7].add(prim_ref{lines_ahead=0, prim_index=0})
```

**Complex primitives:**
```
line[7].complex_prims.add(ptr)  // Only last overlapping line
```

Why simplified:
- Complex prims render to atlas, not via grid dispatch
- Last line reference sufficient for lifetime management
- When last line scrolls out → prim out of scope → release

### Atlas-Based Rendering

Complex primitives use a render-to-texture approach with atlas composition:

```
┌─────────────────────────────────────────────────────┐
│                  Render Target                       │
│  ┌───────────────────────────────────────────────┐  │
│  │ Simple prims layer (SDF + glyphs via grid)    │  │
│  └───────────────────────────────────────────────┘  │
│                        ↓ blend                       │
│  ┌───────────────────────────────────────────────┐  │
│  │ Complex prims atlas (pre-rendered textures)   │  │
│  │  ┌────────┬────────┬────────┐                 │  │
│  │  │ yplot1 │ yplot2 │ image1 │                 │  │
│  │  ├────────┴────────┴────────┤                 │  │
│  │  │    nested_ypaint         │                 │  │
│  │  └──────────────────────────┘                 │  │
│  └───────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
```

**Flow:**
1. Simple prims → rendered via grid dispatch (existing flow)
2. Complex prims → each rendered to atlas region
3. Atlas texture blended with simple prims layer
4. Result → render target (surface or ymux)

### Atlas Lifecycle

**Creation:**
- Atlas created on first render of complex prims
- Regions packed (simple bin packing)
- Each complex prim renders to its region

**Caching:**
- Atlas texture cached between frames
- Prim dirty flag → re-render only that region (or full atlas if simpler)
- No re-render if nothing changed

**Release:**
- Complex prim attached to last overlapping line
- Line scrolls out → prim released
- Atlas region freed (or full atlas recreated)

### Recursive Composition

Complex primitives can contain nested ypaint (recursive):

```
Terminal
  └── ypaint layer
        ├── simple prims → grid dispatch
        └── complex prims → atlas
              └── nested ypaint → nested atlas (cached)
                    ├── simple prims → nested grid dispatch
                    └── complex prims → nested-nested atlas
                          └── ...recursive
```

Each nesting level:
- Has its own atlas
- Renders independently
- Cached separately
- Blended into parent's atlas

### Atlas Sizing

| Scenario | Atlas Size | Memory |
|----------|-----------|--------|
| Typical (5-20 prims) | 2048×2048 | 16MB |
| Busy (50+ prims) | 4096×4096 | 64MB |
| Extreme (100+ prims) | 8192×8192 | 256MB |

Query `device.limits.maxTextureDimension2D` for GPU limit. Safe baseline: 4096×4096.

### Render Target Integration

The render target abstraction handles both local GPU and remote ymux:

**Surface target (local GPU):**
```
layer_renderer[0] → simple prims texture
complex_prim_atlas → pre-rendered complex prims
         ↓
    blend_pass → surface
```

**ymux target (remote):**
```
Serialize complex prim data → send to remote
Remote renders using same atlas approach
```

### Wire Format (FAM - Flexible Array Member)

Complex primitives in buffer use FAM format:

```
[type: u32][payload_size: u32][data: u8[payload_size]]
```

Type IDs use upper half of u32 range (≥ 0x80000000) to distinguish from simple SDF types (0-255).

### Flyweight Pattern

Complex primitive rendering follows flyweight pattern:

**Intrinsic (shared per type):**
- Shader code
- Uniforms (constants)

**Extrinsic (per instance):**
- Bounds, colors, content data
- Atlas region (render artifact)

Type registration provides intrinsic parts. Instance data stored in buffer. Atlas is render-time artifact, cached but separate from data model.

---

## Flyweight Registry Architecture

The flyweight registry is the central mechanism for handling different primitive types. It decouples primitive data storage (buffer) from primitive operations (size, aabb, destroy).

### Module Structure

```
ypaint-core/flyweight.h    # Types and instance-based API
ypaint-core/flyweight.c    # Registry implementation
ypaint/flyweight.h         # Factory function
ypaint/flyweight.c         # Creates configured registry with all handlers
```

### Core Types

```c
// Primitive ops vtable
struct yetty_ypaint_prim_ops {
    struct yetty_ycore_size_result (*size)(const uint32_t *prim);
    struct rectangle_result (*aabb)(const uint32_t *prim);
    void (*destroy)(void *cache);  // optional, NULL for simple prims
    struct yetty_yrender_gpu_resource_set_result (*get_gpu_resource_set)(
        const uint32_t *prim, void **cache_ptr);  // optional, NULL for SDF prims
};

// Flyweight - wraps pointer to primitive data + ops
struct yetty_ypaint_prim_flyweight {
    const uint32_t *data;  // type at data[0]
    const struct yetty_ypaint_prim_ops *ops;
};

// Handler function - returns flyweight (ops=NULL if not handled)
typedef struct yetty_ypaint_prim_flyweight (*yetty_ypaint_prim_handler_fn)(
    const uint32_t *prim);
```

### Registry API

```c
// Create/destroy registry instance
struct yetty_ypaint_flyweight_registry_ptr_result
yetty_ypaint_flyweight_registry_create(void);

void yetty_ypaint_flyweight_registry_destroy(
    struct yetty_ypaint_flyweight_registry *reg);

// Set default handler (SDF) - tried first, fast path
void yetty_ypaint_flyweight_registry_set_default(
    struct yetty_ypaint_flyweight_registry *reg,
    yetty_ypaint_prim_handler_fn handler);

// Register handler for type range [type_min, type_max]
struct yetty_ycore_void_result yetty_ypaint_flyweight_registry_add(
    struct yetty_ypaint_flyweight_registry *reg,
    uint32_t type_min, uint32_t type_max,
    yetty_ypaint_prim_handler_fn handler);

// Get flyweight for primitive
struct yetty_ypaint_prim_flyweight yetty_ypaint_flyweight_registry_get(
    const struct yetty_ypaint_flyweight_registry *reg,
    const uint32_t *prim);
```

### Handler Lookup Order

1. **Default handler** (SDF) - tried first for fast path
2. **Type range handlers** - searched if default returns ops=NULL

```c
// Lookup logic in yetty_ypaint_flyweight_registry_get:
if (default_handler) {
    fw = default_handler(prim);
    if (fw.ops) return fw;
}
uint32_t type = prim[0];
for (each registered handler) {
    if (type >= handler.type_min && type <= handler.type_max) {
        fw = handler.handler(prim);
        if (fw.ops) return fw;
    }
}
return {NULL, NULL};  // unhandled
```

### Canvas Ownership

Canvas owns the flyweight registry instance:

```c
struct yetty_yetty_ypaint_canvas {
    // ...
    struct yetty_ypaint_flyweight_registry *flyweight_registry;
};

// In canvas_create:
struct yetty_ypaint_flyweight_registry_ptr_result fw_res =
    yetty_ypaint_flyweight_create();  // from ypaint/flyweight.c
canvas->flyweight_registry = fw_res.value;

// In canvas_destroy:
line_buffer_free(&canvas->lines, canvas->flyweight_registry);
yetty_ypaint_flyweight_registry_destroy(canvas->flyweight_registry);
```

### Buffer Read/Write Modes

**Write mode** (adding primitives):
- No registry needed
- Buffer is pure data container
- `yetty_ypaint_core_buffer_add_prim()` just appends bytes

**Read mode** (iterating primitives):
- Registry required for size lookup
- Passed as parameter to iteration functions

```c
// Iteration with registry
struct yetty_ypaint_core_primitive_iter_result
yetty_ypaint_core_buffer_prim_first(
    const struct yetty_ypaint_core_buffer *buf,
    const struct yetty_ypaint_flyweight_registry *reg);

struct yetty_ypaint_core_primitive_iter_result
yetty_ypaint_core_buffer_prim_next(
    const struct yetty_ypaint_core_buffer *buf,
    const struct yetty_ypaint_flyweight_registry *reg,
    const struct yetty_ypaint_core_primitive_iter *iter);
```

### Handler Implementation Example

Each module implements its own handler:

**SDF handler (ysdf/handler.h):**
```c
static inline struct yetty_ypaint_prim_flyweight
yetty_ysdf_handler(const uint32_t *prim) {
    uint32_t type = prim[0];
    if (type > YETTY_YSDF_TYPE_MAX)
        return (struct yetty_ypaint_prim_flyweight){NULL, NULL};
    return (struct yetty_ypaint_prim_flyweight){prim, &yetty_ysdf_prim_ops};
}
```

**yplot handler (yplot/yplot.c):**
```c
struct yetty_ypaint_prim_flyweight
yetty_yplot_handler(const uint32_t *prim) {
    uint32_t type = prim[0];
    if (type != YETTY_YPLOT_PRIM_TYPE_ID)
        return (struct yetty_ypaint_prim_flyweight){NULL, NULL};
    return (struct yetty_ypaint_prim_flyweight){prim, &yplot_prim_ops};
}
```

### Wiring Layer (ypaint/flyweight.c)

Creates configured registry with all handlers:

```c
struct yetty_ypaint_flyweight_registry_ptr_result
yetty_ypaint_flyweight_create(void) {
    struct yetty_ypaint_flyweight_registry_ptr_result res =
        yetty_ypaint_flyweight_registry_create();
    if (YETTY_IS_ERR(res)) return res;

    // Default handler for SDF (fast path)
    yetty_ypaint_flyweight_registry_set_default(res.value, yetty_ysdf_handler);

    // Complex primitive handlers by type range
    yetty_ypaint_flyweight_registry_add(res.value,
        YETTY_YPAINT_TYPE_YPLOT, YETTY_YPAINT_TYPE_YPLOT,
        yetty_yplot_handler);

    // Future: yimage, yvideo handlers

    return res;
}
```

### Complex Prim Cleanup

When grid lines are freed, complex prim caches need cleanup via flyweight ops:

```c
// In grid_line_free:
for (each complex_prim) {
    if (cp->cache) {
        struct yetty_ypaint_prim_flyweight fw =
            yetty_ypaint_flyweight_registry_get(reg, cp->data);
        if (fw.ops && fw.ops->destroy)
            fw.ops->destroy(cp->cache);
    }
    free(cp->data);
}
```

### Canvas Complex Prim Access

Canvas exposes visible complex prims for rendering:

```c
// Get count of visible complex prims
uint32_t yetty_yetty_ypaint_canvas_complex_prim_count(canvas);

// Get complex prim reference by index
struct yetty_ypaint_canvas_complex_prim_ref {
    const uint32_t *data;  // FAM: [type][payload_size][payload...]
    void **cache_ptr;      // Pointer to cache storage
};
struct yetty_ypaint_canvas_complex_prim_ref
yetty_yetty_ypaint_canvas_get_complex_prim(canvas, index);
```

### Layer Integration

ypaint-layer collects complex prim resource sets:

```c
// In ypaint_layer_get_gpu_resource_set:
for (each visible complex prim) {
    struct yetty_ypaint_prim_flyweight fw =
        yetty_ypaint_flyweight_registry_get(reg, ref.data);
    if (fw.ops && fw.ops->get_gpu_resource_set) {
        struct yetty_yrender_gpu_resource_set_result rs =
            fw.ops->get_gpu_resource_set(ref.data, ref.cache_ptr);
        layer->rs.children[child_idx++] = rs.value;
    }
}
```

---

## Complex Prim Shader Dispatch

Complex primitives are rendered via shader dispatch in the main ypaint fragment shader. The shader checks for complex prim types (>= 0x80000000) and calls the appropriate render function.

```wgsl
// In ypaint-layer.wgsl fragment shader:
const YPAINT_COMPLEX_TYPE_BASE: u32 = 0x80000000u;
const YPAINT_TYPE_YPLOT: u32 = 0x80000003u;

// In the primitive loop:
if (prim_type >= YPAINT_COMPLEX_TYPE_BASE) {
    var complex_color = vec4<f32>(0.0);

    if (prim_type == YPAINT_TYPE_YPLOT) {
        complex_color = yplot_render(prim_offset, local_pos);
    }
    // Add other complex types here as needed

    if (complex_color.a > 0.0) {
        result_color = mix(result_color, complex_color.rgb, complex_color.a);
        result_alpha = max(result_alpha, complex_color.a);
    }
    continue;
}
```

The `yplot_render()` function is defined in `yplot.wgsl` and merged by the binder. Each complex prim type provides its own render function that:
- Takes `prim_offset` and `local_pos` (pixel position in primitive-local coords)
- Returns `vec4<f32>` color (premultiplied alpha)
- Does its own bounds checking internally

---

## Implementation Status

### Completed

1. **Flyweight registry** - instance-based with handlers for size, aabb, destroy, get_gpu_resource_set
2. **yplot handler** - implements all flyweight ops
3. **Canvas complex prim access** - exposes visible complex prims for rendering
4. **Layer integration** - collects complex prim resource sets as children
5. **Shader dispatch** - ypaint shader dispatches to complex prim render functions (e.g., `yplot_render()`)

### Remaining (Atlas Rendering - Future)

Atlas-based rendering is an alternative approach for complex prims that:
- Have expensive per-pixel evaluation (images, video)
- Benefit from texture caching
- Need to composite with effects

For now, yplot uses inline shader dispatch (fast enough). Atlas rendering will be added for yimage/yvideo.
