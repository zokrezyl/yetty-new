// MSDF font shader — ypaint-layer glue.
//
// Exposes three backend-agnostic helpers. ypaint-layer.wgsl calls them by
// name; the binder merges whichever font shader is active (this one for MSDF
// or raster-font.wgsl for FreeType) so the same caller works with both.
//
// Glyph metadata layout in storage_buffer (6 u32 words per glyph, matches
// struct glyph_meta_gpu in msdf-font.c):
//   [0] size_x       bitmap width  in pixels at base_size
//   [1] size_y       bitmap height in pixels at base_size
//   [2] bearing_x    horizontal bearing (pre-applied by ypaint on the CPU)
//   [3] bearing_y    vertical   bearing (pre-applied by ypaint on the CPU)
//   [4] advance      horizontal advance in pixels at base_size
//   [5] cell_idx     atlas cell index (-1 for empty glyphs like space)
//
// The atlas is a uniform grid of cell_size×cell_size cells; each glyph's
// bitmap is placed *centered* in its cell with per-axis padding. The shader
// samples only that inner bitmap region, so a glyph with small bitmap sits
// naturally inside its caller-provided render rectangle.

fn median3(r: f32, g: f32, b: f32) -> f32 {
    return max(min(r, g), min(max(r, g), b));
}

fn font_base_size() -> f32 {
    return uniforms.msdf_font_base_size;
}

fn font_glyph_size(glyph_index: u32) -> vec2<f32> {
    let base = msdf_font_buffer_offset + glyph_index * 6u;
    return vec2<f32>(
        bitcast<f32>(storage_buffer[base + 0u]),
        bitcast<f32>(storage_buffer[base + 1u])
    );
}

// Alpha coverage for a glyph at normalized glyph-local coords (0..1).
// pixel_scale = rendered_pixels / base_size — drives MSDF antialiasing.
fn font_glyph_sample(glyph_index: u32,
                     glyph_uv: vec2<f32>,
                     pixel_scale: f32) -> f32 {
    let meta_base = msdf_font_buffer_offset + glyph_index * 6u;
    let glyph_size = vec2<f32>(
        bitcast<f32>(storage_buffer[meta_base + 0u]),
        bitcast<f32>(storage_buffer[meta_base + 1u])
    );
    if (glyph_size.x <= 0.0 || glyph_size.y <= 0.0) {
        return 0.0;
    }
    let cell_idx_f = bitcast<f32>(storage_buffer[meta_base + 5u]);
    if (cell_idx_f < 0.0) {
        return 0.0;
    }

    let cell_idx = u32(cell_idx_f);
    let cell_size_px = f32(uniforms.msdf_font_cell_size);
    let atlas_cols = uniforms.msdf_font_atlas_cols;
    let col = cell_idx % atlas_cols;
    let row = cell_idx / atlas_cols;

    let atlas_size = vec2<f32>(textureDimensions(atlas_rgba8_texture, 0));
    let cell_origin_px = vec2<f32>(f32(col), f32(row)) * cell_size_px;
    // Bitmap is centered in the cell — map glyph_uv to the inner region only.
    let padding = (vec2<f32>(cell_size_px) - glyph_size) * 0.5;
    let uv_min = (cell_origin_px + padding) / atlas_size;
    let uv_max = (cell_origin_px + padding + glyph_size) / atlas_size;
    let uv = clamp(glyph_uv, vec2<f32>(0.0), vec2<f32>(1.0));
    let sample_uv = mix(uv_min, uv_max, uv);

    let msdf = textureSampleLevel(atlas_rgba8_texture, atlas_rgba8_sampler,
                                  sample_uv, 0.0);
    let sd = median3(msdf.r, msdf.g, msdf.b);
    let screen_px_range = uniforms.msdf_font_pixel_range * pixel_scale;
    return clamp((sd - 0.5) * screen_px_range + 0.5, 0.0, 1.0);
}
