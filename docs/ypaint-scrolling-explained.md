# YPaint Scrolling System Explained

This document explains how YPaint primitives work with terminal scrolling.

## Coordinate System

- Screen origin (0, 0) is at **TOP-LEFT**
- X increases rightward
- Y increases **downward**
- Grid rows are numbered from top (row 0) to bottom

## Primitive Storage

Primitives are stored in **grid lines**. Each grid line corresponds to a terminal row. When a primitive is added, it is stored in the **LOWEST row** (highest row number) that its bounding box overlaps. This ensures that when that row scrolls off the screen, the primitive is deleted.

## Rolling Row System

- `row0Absolute`: Total number of lines that have scrolled off the top
- Each grid line has an implicit absolute row number: `rolling_row = row0Absolute + line_index`
- When scrolling occurs, lines are popped from the front of the buffer and `row0Absolute` increases

## Example: Adding a Circle

### Setup
- `cell_height` = 20 pixels
- Cursor at row 5 (user is typing on terminal row 5)
- Circle added with `cy=30` (30 pixels below cursor), `radius=25`

### Step 1: Calculate Circle Center (Absolute Position)

```
cursor_top_Y = cursor_row * cell_height = 5 * 20 = 100 pixels
circle_center_Y = cursor_top_Y + cy = 100 + 30 = 130 pixels
```

### Step 2: Calculate AABB (Axis-Aligned Bounding Box)

```
AABB_minY = circle_center_Y - radius = 130 - 25 = 105 pixels
AABB_maxY = circle_center_Y + radius = 130 + 25 = 155 pixels
```

### Step 3: Determine Grid Rows

```
min_row = floor(AABB_minY / cell_height) = floor(105 / 20) = row 5
max_row = floor(AABB_maxY / cell_height) = floor(155 / 20) = row 7
```

The circle spans **rows 5, 6, and 7**.

### Step 4: Storage Row

Primitive is stored in **row 7** (the LOWEST/BOTTOM row of the AABB).

Why? When row 7 scrolls off the screen, the entire circle should be deleted. If we stored it in row 5, the circle would be deleted while still partially visible.

### Step 5: Adjust cy for Storage

The primitive's `cy` must be adjusted to be relative to the **storage row's top**, not the cursor.

```
storage_row_top_Y = storage_row * cell_height = 7 * 20 = 140 pixels
adjusted_cy = circle_center_Y - storage_row_top_Y = 130 - 140 = -10 pixels
```

The primitive stores `cy = -10`, meaning the circle center is **10 pixels ABOVE** the top of row 7.

## Serialization for GPU

When building the GPU staging buffer, each primitive is prefixed with its `rolling_row`:

```
[rolling_row][type][z_order][fill][stroke][stroke_width][cx][cy][radius]...
```

For our example (assuming `row0Absolute = 0`):
```
rolling_row = row0Absolute + line_index = 0 + 7 = 7
```

## Shader Rendering

### Before Scrolling (row_origin = 0)

Rendering at pixel Y = 130 (the circle center):

```
y_offset = (rolling_row - row_origin) * cell_height
         = (7 - 0) * 20 = 140

prim_scene_pos.y = pixel_pos.y - y_offset
                 = 130 - 140 = -10

primitive cy = -10

Match! SDF evaluates correctly.
```

### After Scrolling 3 Lines (row_origin = 3)

Lines 0, 1, 2 are deleted. The primitive's line (was index 7) is now index 4, but its absolute row is still 7.

Circle should now appear at screen Y = 130 - (3 * 20) = 70 pixels.

Rendering at pixel Y = 70:

```
rolling_row = row0Absolute + line_index = 3 + 4 = 7 (unchanged)

y_offset = (rolling_row - row_origin) * cell_height
         = (7 - 3) * 20 = 80

prim_scene_pos.y = pixel_pos.y - y_offset
                 = 70 - 80 = -10

primitive cy = -10

Match! SDF evaluates correctly.
```

### Scrolling Until Deletion

When 8 total lines have scrolled (`row_origin = 8`), row 7 scrolls off. The grid line containing our primitive is deleted, and the circle disappears.

## Summary

1. **Primitive cy is relative to cursor** when specified by user
2. **AABB is offset by cursor** to determine grid rows
3. **Primitive stored in LOWEST row** of AABB (for correct deletion on scroll)
4. **cy is adjusted** to be relative to storage row's top
5. **Serialization prepends rolling_row** (absolute row number)
6. **Shader computes y_offset** from `(rolling_row - row_origin) * cell_height`
7. **SDF evaluates** with position adjusted by y_offset
