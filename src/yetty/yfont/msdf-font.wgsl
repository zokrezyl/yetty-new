// MSDF font shader — provides font_sample()
// Uses RGBA8 atlas texture with uniform cell grid + per-glyph metadata buffer
// Metadata per glyph: [sizeX, sizeY, bearingX, bearingY, advance, _pad]
// = 6 floats = 6 u32 words in storage_buffer
// UV is computed from glyph_index using cell_size and atlas_cols uniforms

fn median3(r: f32, g: f32, b: f32) -> f32 {
    return max(min(r, g), min(max(r, g), b));
}

fn font_sample(glyph_index: u32, local_px: vec2<f32>, render_size: vec2<f32>) -> f32 {
    let base = msdf_font_buffer_offset + glyph_index * 6u;
    let glyph_size = vec2<f32>(
        bitcast<f32>(storage_buffer[base + 0u]),
        bitcast<f32>(storage_buffer[base + 1u])
    );
    let bearing = vec2<f32>(
        bitcast<f32>(storage_buffer[base + 2u]),
        bitcast<f32>(storage_buffer[base + 3u])
    );

    // Empty glyph
    if (glyph_size.x <= 0.0 || glyph_size.y <= 0.0) {
        return 0.0;
    }

    // Compute UV from glyph_index using uniform cell grid
    let cell_size = f32(uniforms.msdf_font_cell_size);
    let atlas_cols = uniforms.msdf_font_atlas_cols;
    let col = glyph_index % atlas_cols;
    let row = glyph_index / atlas_cols;

    // Cell position in atlas (normalized 0-1)
    let atlas_size = vec2<f32>(textureDimensions(atlas_rgba8_texture, 0));
    let cell_uv_size = cell_size / atlas_size;
    let uv_min = vec2<f32>(f32(col), f32(row)) * cell_uv_size;
    let uv_max = uv_min + cell_uv_size;

    // Scale from base_size to render_size
    let scale = render_size.y / uniforms.msdf_font_base_size;
    let scaled_size = glyph_size * scale;
    let scaled_bearing = bearing * scale;

    // Baseline at 80% of render height
    let baseline = render_size.y * 0.8;
    let glyph_top = baseline - scaled_bearing.y;
    let glyph_left = scaled_bearing.x;

    let glyph_min = vec2<f32>(glyph_left, glyph_top);
    let glyph_max = vec2<f32>(glyph_left + scaled_size.x, glyph_top + scaled_size.y);

    // Bounds check
    if (local_px.x < glyph_min.x || local_px.x >= glyph_max.x ||
        local_px.y < glyph_min.y || local_px.y >= glyph_max.y) {
        return 0.0;
    }

    // Map pixel to UV within glyph cell
    let glyph_local = (local_px - glyph_min) / scaled_size;
    let sample_uv = mix(uv_min, uv_max, glyph_local);

    // Sample MSDF
    let msdf = textureSampleLevel(atlas_rgba8_texture, atlas_rgba8_sampler, sample_uv, 0.0);
    let sd = median3(msdf.r, msdf.g, msdf.b);

    // Anti-aliased edge
    let screen_px_range = uniforms.msdf_font_pixel_range * scale;
    return clamp((sd - 0.5) * screen_px_range + 0.5, 0.0, 1.0);
}
