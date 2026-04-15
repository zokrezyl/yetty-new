// Raster font shader — provides font_sample()
// Uses R8 atlas texture + per-glyph UV buffer

fn font_sample(glyph_index: u32, local_uv: vec2<f32>, cell_size: vec2<f32>) -> f32 {
    let base = raster_font_buffer_offset + glyph_index * 2u;
    let glyph_uv = vec2<f32>(
        bitcast<f32>(storage_buffer[base]),
        bitcast<f32>(storage_buffer[base + 1u])
    );

    if (glyph_uv.x < 0.0) {
        return 0.0;
    }

    let region = raster_font_texture_region;
    let region_size = region.zw - region.xy;
    let atlas_size = vec2<f32>(f32(textureDimensions(atlas_r8_texture).x),
                               f32(textureDimensions(atlas_r8_texture).y));
    let glyph_size_uv = cell_size / atlas_size;
    let sample_uv = region.xy + glyph_uv * region_size + local_uv * glyph_size_uv;
    return textureSampleLevel(atlas_r8_texture, atlas_r8_sampler, sample_uv, 0.0).r;
}
