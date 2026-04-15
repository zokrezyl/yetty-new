// MSDF font shader — provides font_sample()
// Uses RGBA8 atlas texture + per-glyph metadata buffer
// Metadata per glyph: [uvMinX, uvMinY, uvMaxX, uvMaxY, sizeX, sizeY, bearingX, bearingY, advance, _pad]
// = 10 floats = 10 u32 words in storage_buffer

fn median3(r: f32, g: f32, b: f32) -> f32 {
    return max(min(r, g), min(max(r, g), b));
}

fn font_sample(glyph_index: u32, local_px: vec2<f32>, cell_size: vec2<f32>) -> f32 {
    let base = ms_msdf_font_buffer_offset + glyph_index * 10u;
    let uv_min = vec2<f32>(
        bitcast<f32>(storage_buffer[base + 0u]),
        bitcast<f32>(storage_buffer[base + 1u])
    );
    let uv_max = vec2<f32>(
        bitcast<f32>(storage_buffer[base + 2u]),
        bitcast<f32>(storage_buffer[base + 3u])
    );
    let glyph_size = vec2<f32>(
        bitcast<f32>(storage_buffer[base + 4u]),
        bitcast<f32>(storage_buffer[base + 5u])
    );
    let bearing = vec2<f32>(
        bitcast<f32>(storage_buffer[base + 6u]),
        bitcast<f32>(storage_buffer[base + 7u])
    );

    // Empty glyph
    if (glyph_size.x <= 0.0 || glyph_size.y <= 0.0) {
        return 0.0;
    }

    let scale = uniforms.ms_msdf_font_scale;
    let scaled_size = glyph_size * scale;
    let scaled_bearing = bearing * scale;

    // Baseline at 80% of cell height
    let baseline = cell_size.y * 0.8;
    let glyph_top = baseline - scaled_bearing.y;
    let glyph_left = scaled_bearing.x;

    let glyph_min = vec2<f32>(glyph_left, glyph_top);
    let glyph_max = vec2<f32>(glyph_left + scaled_size.x, glyph_top + scaled_size.y);

    // Bounds check
    if (local_px.x < glyph_min.x || local_px.x >= glyph_max.x ||
        local_px.y < glyph_min.y || local_px.y >= glyph_max.y) {
        return 0.0;
    }

    // Map pixel to UV within glyph
    let glyph_local = (local_px - glyph_min) / scaled_size;

    // Map to atlas UV via region
    let region = ms_msdf_font_texture_region;
    let region_size = region.zw - region.xy;
    let glyph_uv = mix(uv_min, uv_max, glyph_local);
    let sample_uv = region.xy + glyph_uv * region_size;

    // Sample MSDF
    let msdf = textureSampleLevel(atlas_rgba8_texture, atlas_rgba8_sampler, sample_uv, 0.0);
    let sd = median3(msdf.r, msdf.g, msdf.b);

    // Anti-aliased edge
    let screen_px_range = uniforms.ms_msdf_font_pixel_range * scale;
    return clamp((sd - 0.5) * screen_px_range + 0.5, 0.0, 1.0);
}
