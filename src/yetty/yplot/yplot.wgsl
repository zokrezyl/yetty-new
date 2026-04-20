// YPlot Complex Primitive Shader
// Renders plot from primitive buffer data using yfsvm bytecode interpreter
//
// Wire format in primitive buffer (after rolling_row):
//   [0] type (0x80000003)
//   [1] payload_size
//   Payload:
//   [0] bounds_x (f32)
//   [1] bounds_y (f32)
//   [2] bounds_w (f32)
//   [3] bounds_h (f32)
//   [4] flags (u32)
//   [5] function_count (u32)
//   [6-13] colors[8] (u32)
//   [14] bytecode_word_count (u32)
//   [15+] bytecode[]

const YPLOT_FLAG_GRID: u32 = 1u;
const YPLOT_FLAG_AXES: u32 = 2u;
const YPLOT_FLAG_LABELS: u32 = 4u;

const YPLOT_BG_COLOR: vec3<f32> = vec3<f32>(0.102, 0.102, 0.180);
const YPLOT_GRID_COLOR: vec3<f32> = vec3<f32>(0.267, 0.267, 0.267);
const YPLOT_AXIS_COLOR: vec3<f32> = vec3<f32>(0.667, 0.667, 0.667);

// Read yplot data from primitive buffer
// prim_offset points to rolling_row, payload starts at prim_offset + 3
fn yplot_read_bounds_x(prim_offset: u32) -> f32 {
    return bitcast<f32>(storage_buffer[prim_offset + 3u]);
}
fn yplot_read_bounds_y(prim_offset: u32) -> f32 {
    return bitcast<f32>(storage_buffer[prim_offset + 4u]);
}
fn yplot_read_bounds_w(prim_offset: u32) -> f32 {
    return bitcast<f32>(storage_buffer[prim_offset + 5u]);
}
fn yplot_read_bounds_h(prim_offset: u32) -> f32 {
    return bitcast<f32>(storage_buffer[prim_offset + 6u]);
}
fn yplot_read_flags(prim_offset: u32) -> u32 {
    return storage_buffer[prim_offset + 7u];
}
fn yplot_read_func_count(prim_offset: u32) -> u32 {
    return storage_buffer[prim_offset + 8u];
}
fn yplot_read_color(prim_offset: u32, idx: u32) -> u32 {
    return storage_buffer[prim_offset + 9u + idx];
}
fn yplot_read_bytecode_len(prim_offset: u32) -> u32 {
    return storage_buffer[prim_offset + 17u];
}
fn yplot_bytecode_offset(prim_offset: u32) -> u32 {
    return prim_offset + 18u;
}

fn yplot_unpack_color(packed: u32) -> vec3<f32> {
    return vec3<f32>(
        f32((packed >> 16u) & 0xFFu) / 255.0,
        f32((packed >> 8u) & 0xFFu) / 255.0,
        f32(packed & 0xFFu) / 255.0
    );
}

fn yplot_draw_grid(bg: vec3<f32>, plotUV: vec2<f32>) -> vec3<f32> {
    var color = bg;
    let gridX = fract(plotUV.x * 10.0);
    let gridY = fract(plotUV.y * 10.0);
    let lineWidth = 0.015;
    if (gridX < lineWidth || gridX > 1.0 - lineWidth ||
        gridY < lineWidth || gridY > 1.0 - lineWidth) {
        color = mix(color, YPLOT_GRID_COLOR, 0.5);
    }
    return color;
}

fn yplot_draw_axes(bg: vec3<f32>, plotUV: vec2<f32>, xMin: f32, xMax: f32, yMin: f32, yMax: f32) -> vec3<f32> {
    var color = bg;
    let lineWidth = 0.008;

    // Y-axis at x=0
    let xZero = (0.0 - xMin) / (xMax - xMin);
    if (xZero >= 0.0 && xZero <= 1.0 && abs(plotUV.x - xZero) < lineWidth) {
        color = YPLOT_AXIS_COLOR;
    }

    // X-axis at y=0
    let yZero = (0.0 - yMin) / (yMax - yMin);
    if (yZero >= 0.0 && yZero <= 1.0 && abs(plotUV.y - yZero) < lineWidth) {
        color = YPLOT_AXIS_COLOR;
    }

    // Border
    if (plotUV.x < lineWidth || plotUV.x > 1.0 - lineWidth ||
        plotUV.y < lineWidth || plotUV.y > 1.0 - lineWidth) {
        color = YPLOT_AXIS_COLOR * 0.7;
    }

    return color;
}

// Main yplot render function - called by ypaint dispatcher
fn yplot_render(prim_offset: u32, local_pos: vec2<f32>) -> vec4<f32> {
    let bounds_x = yplot_read_bounds_x(prim_offset);
    let bounds_y = yplot_read_bounds_y(prim_offset);
    let bounds_w = yplot_read_bounds_w(prim_offset);
    let bounds_h = yplot_read_bounds_h(prim_offset);

    // Check if pixel is inside plot bounds
    if (local_pos.x < bounds_x || local_pos.x >= bounds_x + bounds_w ||
        local_pos.y < bounds_y || local_pos.y >= bounds_y + bounds_h) {
        return vec4<f32>(0.0);
    }

    // Normalize to 0-1 within plot area
    let plotUV = (local_pos - vec2<f32>(bounds_x, bounds_y)) / vec2<f32>(bounds_w, bounds_h);

    let flags = yplot_read_flags(prim_offset);
    let func_count = yplot_read_func_count(prim_offset);
    let bytecode_len = yplot_read_bytecode_len(prim_offset);
    let bc_offset = yplot_bytecode_offset(prim_offset);

    // Default data range (TODO: read from wire format if needed)
    let xMin = -3.14159;
    let xMax = 3.14159;
    let yMin = -1.5;
    let yMax = 1.5;

    // Background
    var color = YPLOT_BG_COLOR;

    // Grid
    if ((flags & YPLOT_FLAG_GRID) != 0u) {
        color = yplot_draw_grid(color, plotUV);
    }

    // Axes
    if ((flags & YPLOT_FLAG_AXES) != 0u) {
        color = yplot_draw_axes(color, plotUV, xMin, xMax, yMin, yMax);
    }

    // Map plotUV to data coordinates
    let dataX = mix(xMin, xMax, plotUV.x);
    let dataY = mix(yMin, yMax, plotUV.y);

    // Line width in normalized coords
    let lineWidth = 3.0 / bounds_h;

    // Evaluate and render each function
    if (func_count > 0u && bytecode_len > 0u) {
        var samplers: array<f32, 8>;
        samplers[0] = dataX;

        for (var fi = 0u; fi < min(func_count, 8u); fi++) {
            let func_color_packed = yplot_read_color(prim_offset, fi);
            let func_color = yplot_unpack_color(func_color_packed);

            // Evaluate function using yfsvm
            let y = yfsvm_execute(bc_offset, fi, dataX, 0.0, samplers);

            // Map y to plot UV
            let yNorm = (y - yMin) / (yMax - yMin);
            let dist = abs(plotUV.y - yNorm);

            // Anti-aliased line
            if (dist < lineWidth) {
                let alpha = 1.0 - dist / lineWidth;
                color = mix(color, func_color, alpha);
            }
        }
    }

    return vec4<f32>(color, 1.0);
}
