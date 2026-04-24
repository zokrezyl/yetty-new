// =============================================================================
// ymgui Layer Shader — draw a CPU-rasterised ImGui frame at the cursor
// anchor, synced with terminal scroll via the rolling-row model.
// =============================================================================
// The layer's resource set contains:
//   texture: ymgui_raster (RGBA8, sized exactly to DisplaySize)
//   uniforms:
//     ymgui_grid_size      vec2(cols, rows)
//     ymgui_cell_size      vec2(cell_w, cell_h)
//     ymgui_row_origin     u32   rolling row of top visible line
//     ymgui_frame_rolling  u32   rolling row where frame is anchored
//     ymgui_frame_size     vec2  DisplaySize in pixels
//     ymgui_frame_present  u32   1 if a frame is visible, 0 → discard-all
//     ymgui_vz_scale       f32   visual zoom
//     ymgui_vz_off         vec2  visual zoom offset
//
// Positioning: the frame's top-left sits at
//     (0,  (frame_rolling_row - row_origin) * cell_height)
// in grid pixel space. Horizontal placement is column 0 for v1.
//
// RENDER_LAYER_BINDINGS_PLACEHOLDER is replaced by the binder at compile
// time with the concrete @group/@binding declarations — same mechanism the
// other layers use.
// =============================================================================

// RENDER_LAYER_BINDINGS_PLACEHOLDER

struct VertexInput {
    @location(0) position: vec2<f32>,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
};

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var out: VertexOutput;
    out.position = vec4<f32>(input.position, 0.0, 1.0);
    return out;
}

@fragment
fn fs_main(@builtin(position) frag_pos: vec4<f32>) -> @location(0) vec4<f32> {
    // Grid pixel size.
    let grid = uniforms.ymgui_ymgui_grid_size;
    let cell = uniforms.ymgui_ymgui_cell_size;
    let grid_px = grid * cell;

    // Early-out if no frame present.
    if (uniforms.ymgui_ymgui_frame_present == 0u) {
        return vec4<f32>(0.0, 0.0, 0.0, 0.0);
    }

    // Apply visual zoom (same mechanism as ypaint-layer).
    let vz_scale = uniforms.ymgui_ymgui_vz_scale;
    let vz_off   = uniforms.ymgui_ymgui_vz_off;
    let px = (frag_pos.xy - vz_off) / max(vz_scale, 1e-6);

    // Where does the frame sit in grid pixels?
    let row_diff = i32(uniforms.ymgui_ymgui_frame_rolling) -
                   i32(uniforms.ymgui_ymgui_row_origin);
    let frame_top_x = 0.0;
    let frame_top_y = f32(row_diff) * cell.y;
    let frame_size  = uniforms.ymgui_ymgui_frame_size;

    // Sample UV within the frame's raster.
    let uv = vec2<f32>(
        (px.x - frame_top_x) / max(frame_size.x, 1.0),
        (px.y - frame_top_y) / max(frame_size.y, 1.0),
    );

    // Outside the frame rectangle → transparent.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return vec4<f32>(0.0, 0.0, 0.0, 0.0);
    }

    return textureSample(ymgui_raster, ymgui_sampler, uv);
}
