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
packed_offset = col | (rolling_row << 16) = col | (5 << 16)
[packed_offset][type][z][fill][stroke][stroke_w][cx][cy=30][radius=25]
```

**IMPORTANT: Geometry coordinates (cy=30) are stored exactly as user specified. NO transformation. The shader adjusts the TEST POSITION, not the primitive coordinates.**

---

**GPU (WGSL shader) - Fragment shader per pixel:**

**STEP 6: Render pixel at screen Y=130 (circle center)**
```wgsl
pixel_pos_y = 130              // screen pixel Y coordinate
rolling_row = 5                // from packed_offset
row_origin = 0                 // uniform

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
   packed_offset = col | (rolling_row << 16)
   ```
   Where `rolling_row` = row0_absolute + cursor_row (rolling row at insertion).

3. **Geometry stored as-is** - No coordinate transformation. cy, etc. stored exactly as user specified.

4. **Row origin uniform** - Single `uint32_t` passed to GPU: rolling row of top visible line.

5. **GPU adjusts test position:**
   ```wgsl
   let rolling_row = (packed_offset >> 16u) & 0xFFFFu;
   let y_offset = f32(rolling_row - row_origin) * cell_height;
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
[packed_offset][type][attrs][style][geometry...]
```

Where:
- `packed_offset` = `col | (rolling_row << 16)` - column and rolling row at insertion
- `type` = primitive type for shader dispatch
- `attrs` = rendering attributes
- `style` = fill/stroke colors and width
- `geometry` = primitive-specific SDF parameters (stored as-is, no transformation)

---

## Shader Access

Buffer is `array<f32>` in WGSL. Shader reads at fixed offsets:

```
[0] packed_offset - col | (rolling_row << 16)
[1] type          - bitcast<u32> for dispatch
[2] z_order       - rendering order
[3] fill_color    - bitcast<u32>
[4] stroke_color  - bitcast<u32>
[5] stroke_width  - f32
[6+] geometry     - primitive-specific args (stored as-is)
```

Shader:
1. Extracts `rolling_row` from `packed_offset`
2. Computes `y_offset = (rolling_row - row_origin) * cell_height`
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
