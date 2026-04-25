// =============================================================================
// ymgui Layer Shader — draw a CPU-rasterised ImGui frame at the cursor
// anchor, synced with terminal scroll via the rolling-row model.
// =============================================================================
// Textures go through the shared per-format atlas (yrender's resource
// binder packs them there). We bind one RGBA8 texture named "ymgui_raster"
// in namespace "ymgui"; the binder injects:
//   @group(0) @binding(...) var atlas_rgba8_texture: texture_2d<f32>;
//   @group(0) @binding(...) var atlas_rgba8_sampler: sampler;
//   const ymgui_raster_region: vec4<f32> = vec4(u0,v0,u1,v1);
//
// Uniforms (all ns=ymgui → prefix ymgui_ymgui_):
//   ymgui_grid_size      vec2  cols,rows
//   ymgui_cell_size      vec2  cell_w,cell_h
//   ymgui_raster_size    vec2  raster pixel size locked at create-time;
//                              MUST be used for atlas-UV math because the
//                              atlas region was baked at this size.
//   ymgui_row_origin     u32   rolling row of top visible line
//   ymgui_frame_rolling  u32   rolling row where frame is anchored
//   ymgui_frame_size     vec2  DisplaySize in pixels
//   ymgui_frame_present  u32   1 if a frame is visible
//   ymgui_vz_scale       f32   visual zoom
//   ymgui_vz_off         vec2  visual zoom offset
//
// Positioning: frame top-left sits at
//     (0,  (frame_rolling_row - row_origin) * cell_height)
// in grid pixel space.
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
    if (uniforms.ymgui_ymgui_frame_present == 0u) {
        return vec4<f32>(0.0, 0.0, 0.0, 0.0);
    }

    let cell = uniforms.ymgui_ymgui_cell_size;
    // Raster pixel size — locked at allocation time. The atlas region for
    // this raster was baked into the shader at pipeline-compile time, so
    // we MUST sample using THIS size, not grid*cell (the grid may have
    // resized after allocation; the raster did not).
    let raster_size = uniforms.ymgui_ymgui_raster_size;

    let vz_scale = uniforms.ymgui_ymgui_vz_scale;
    let vz_off   = uniforms.ymgui_ymgui_vz_off;
    let px = (frag_pos.xy - vz_off) / max(vz_scale, 1e-6);

    let row_diff = i32(uniforms.ymgui_ymgui_frame_rolling) -
                   i32(uniforms.ymgui_ymgui_row_origin);
    let frame_top  = vec2<f32>(0.0, f32(row_diff) * cell.y);
    let frame_size = uniforms.ymgui_ymgui_frame_size;

    // Pixel coordinate within the raster (the rasterizer paints at top-left
    // up to `frame_size` pixels; rest is transparent).
    let raster_px = px - frame_top;

    // Sample unconditionally (WGSL forbids textureSample behind non-uniform
    // control flow — implicit derivatives need uniform lanes in the quad).
    let raster_uv = clamp(raster_px / max(raster_size, vec2<f32>(1.0)),
                          vec2<f32>(0.0), vec2<f32>(1.0));
    let region   = ymgui_raster_region;
    let atlas_uv = mix(region.xy, region.zw, raster_uv);
    let c = textureSample(atlas_rgba8_texture, atlas_rgba8_sampler, atlas_uv);

    // Mask to the painted area (frame_size). Outside the frame rectangle
    // the raster is transparent anyway, but we mask explicitly so the
    // mask-clamp at edges doesn't bleed.
    let inside = f32(raster_px.x >= 0.0 && raster_px.x < frame_size.x &&
                     raster_px.y >= 0.0 && raster_px.y < frame_size.y);
    return c * inside;
}
