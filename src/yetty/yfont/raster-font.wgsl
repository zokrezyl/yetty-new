// Non-monospace raster font shader — provides font_sample()
// Uses R8 atlas texture with uniform cell grid + per-glyph metadata buffer.
// Metadata per glyph: [sizeX, sizeY, bearingX, bearingY, advance, cell_idx]
// = 6 floats = 6 u32 words in storage_buffer.
// UV is computed from cell_idx using cell_size and atlas_cols uniforms.

fn font_sample(glyph_index: u32, local_px: vec2<f32>, render_size: vec2<f32>) -> f32 {
    let base = raster_font_buffer_offset + glyph_index * 6u;
    let glyph_size = vec2<f32>(
        bitcast<f32>(storage_buffer[base + 0u]),
        bitcast<f32>(storage_buffer[base + 1u])
    );
    let bearing = vec2<f32>(
        bitcast<f32>(storage_buffer[base + 2u]),
        bitcast<f32>(storage_buffer[base + 3u])
    );
    let cell_idx_f = bitcast<f32>(storage_buffer[base + 5u]);

    // Empty glyph (space, etc.)
    if (glyph_size.x <= 0.0 || glyph_size.y <= 0.0 || cell_idx_f < 0.0) {
        return 0.0;
    }

    let cell_idx = u32(cell_idx_f);
    let cell_size = f32(uniforms.raster_font_cell_size);
    let atlas_cols = uniforms.raster_font_atlas_cols;
    let col = cell_idx % atlas_cols;
    let row = cell_idx / atlas_cols;

    let atlas_size = vec2<f32>(textureDimensions(atlas_r8_texture, 0));
    let cell_uv_size = cell_size / atlas_size;
    let uv_min = vec2<f32>(f32(col), f32(row)) * cell_uv_size;
    let uv_max = uv_min + cell_uv_size;

    // Scale from base_size to render_size
    let scale = render_size.y / uniforms.raster_font_base_size;
    let scaled_size = glyph_size * scale;
    let scaled_bearing = bearing * scale;

    // Baseline at 80% of render height (matches msdf-font placement)
    let baseline = render_size.y * 0.8;
    let glyph_top = baseline - scaled_bearing.y;
    let glyph_left = scaled_bearing.x;

    let glyph_min = vec2<f32>(glyph_left, glyph_top);
    let glyph_max = vec2<f32>(glyph_left + scaled_size.x, glyph_top + scaled_size.y);

    if (local_px.x < glyph_min.x || local_px.x >= glyph_max.x ||
        local_px.y < glyph_min.y || local_px.y >= glyph_max.y) {
        return 0.0;
    }

    let glyph_local = (local_px - glyph_min) / scaled_size;
    let sample_uv = mix(uv_min, uv_max, glyph_local);

    return textureSampleLevel(atlas_r8_texture, atlas_r8_sampler, sample_uv, 0.0).r;
}
