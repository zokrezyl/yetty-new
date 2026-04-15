// MSDF font shader — provides font_sample()
// Uses RGBA8 atlas texture + per-glyph metadata buffer
// Metadata per glyph: [uvMinX, uvMinY, uvMaxX, uvMaxY, sizeX, sizeY, bearingX, bearingY, advance, _pad]
// = 10 floats = 10 u32 words in storage_buffer

fn font_sample(glyph_index: u32, local_uv: vec2<f32>, cell_size: vec2<f32>) -> f32 {
    let base = ms_msdf_font_buffer_offset + glyph_index * 10u;
    let uv_min = vec2<f32>(
        bitcast<f32>(storage_buffer[base + 0u]),
        bitcast<f32>(storage_buffer[base + 1u])
    );
    let uv_max = vec2<f32>(
        bitcast<f32>(storage_buffer[base + 2u]),
        bitcast<f32>(storage_buffer[base + 3u])
    );

    // Empty glyph
    if (uv_min.x == 0.0 && uv_max.x == 0.0) {
        return 0.0;
    }

    // Map local_uv [0,1] within the cell to the glyph's atlas UV range
    let region = ms_msdf_font_texture_region;
    let region_size = region.zw - region.xy;
    let glyph_uv_size = uv_max - uv_min;
    let sample_uv = region.xy + (uv_min + local_uv * glyph_uv_size) * region_size;

    // Sample MSDF
    let msdf = textureSampleLevel(atlas_rgba8_texture, atlas_rgba8_sampler, sample_uv, 0.0);

    // Median of three channels
    let sd = max(min(msdf.r, msdf.g), min(max(msdf.r, msdf.g), msdf.b));

    // Convert to screen-space distance and threshold
    let screen_px_range = uniforms.ms_msdf_font_pixel_range * (cell_size.y / uniforms.ms_msdf_font_glyph_cell_size.y);
    let screen_dist = screen_px_range * (sd - 0.5);
    return clamp(screen_dist + 0.5, 0.0, 1.0);
}
