// =============================================================================
// Terminal Screen Shader - Text Grid
// =============================================================================
// Generated constants (prepended by binder):
//   uniforms.text_grid_*          — grid uniforms
//   raster_font_buffer_offset     — u32 offset of glyph UVs in storage_buffer
//   text_grid_buffer_offset       — u32 offset of cells in storage_buffer
//   raster_font_texture_region    — vec4 UV region in atlas
//   atlas_r8_texture / atlas_r8_sampler

// RENDER_LAYER_BINDINGS_PLACEHOLDER

struct VertexInput {
    @location(0) position: vec2<f32>,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
};

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    output.position = vec4<f32>(input.position, 0.0, 1.0);
    return output;
}

// VTermScreenCell layout (12 bytes):
//   u32[0]: glyph_index
//   u32[1]: fg.r | fg.g<<8 | fg.b<<16 | bg.r<<24
//   u32[2]: bg.g | bg.b<<8 | attrs<<16
fn read_cell_glyph(cell_index: u32) -> u32 {
    return storage_buffer[text_grid_buffer_offset + cell_index * 3u];
}

fn read_cell_fg(cell_index: u32) -> vec3<f32> {
    let packed = storage_buffer[text_grid_buffer_offset + cell_index * 3u + 1u];
    return vec3<f32>(
        f32(packed & 0xFFu) / 255.0,
        f32((packed >> 8u) & 0xFFu) / 255.0,
        f32((packed >> 16u) & 0xFFu) / 255.0
    );
}

fn read_cell_bg(cell_index: u32) -> vec3<f32> {
    let packed1 = storage_buffer[text_grid_buffer_offset + cell_index * 3u + 1u];
    let packed2 = storage_buffer[text_grid_buffer_offset + cell_index * 3u + 2u];
    return vec3<f32>(
        f32((packed1 >> 24u) & 0xFFu) / 255.0,
        f32(packed2 & 0xFFu) / 255.0,
        f32((packed2 >> 8u) & 0xFFu) / 255.0
    );
}

fn read_glyph_uv(glyph_index: u32) -> vec2<f32> {
    let base = raster_font_buffer_offset + glyph_index * 2u;
    return vec2<f32>(
        bitcast<f32>(storage_buffer[base]),
        bitcast<f32>(storage_buffer[base + 1u])
    );
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let pixel_pos = input.position.xy;
    let grid_size = uniforms.text_grid_grid_size;
    let cell_size = uniforms.text_grid_cell_size;

    let grid_pixel_w = grid_size.x * cell_size.x;
    let grid_pixel_h = grid_size.y * cell_size.y;

    if (pixel_pos.x < 0.0 || pixel_pos.y < 0.0 ||
        pixel_pos.x >= grid_pixel_w || pixel_pos.y >= grid_pixel_h) {
        return vec4<f32>(0.1, 0.1, 0.1, 1.0);
    }

    let cell_col = floor(pixel_pos.x / cell_size.x);
    let cell_row = floor(pixel_pos.y / cell_size.y);
    let cell_index = u32(cell_row) * u32(grid_size.x) + u32(cell_col);

    let local_px = vec2<f32>(
        pixel_pos.x - cell_col * cell_size.x,
        pixel_pos.y - cell_row * cell_size.y
    );

    let glyph = read_cell_glyph(cell_index);
    let fg_color = read_cell_fg(cell_index);
    let bg_color = read_cell_bg(cell_index);

    var final_color = bg_color;

    if (glyph != 0u) {
        let glyph_uv = read_glyph_uv(glyph);
        if (glyph_uv.x >= 0.0) {
            // Map local pixel to atlas UV via region
            let region = raster_font_texture_region;
            let region_size = region.zw - region.xy;
            let atlas_size = vec2<f32>(f32(textureDimensions(atlas_r8_texture).x),
                                       f32(textureDimensions(atlas_r8_texture).y));
            // glyph_uv is in [0,1] relative to the original font atlas
            // region maps that to the packed atlas position
            let glyph_size_uv = cell_size / atlas_size;
            let local_uv = local_px / cell_size;
            let sample_uv = region.xy + glyph_uv * region_size + local_uv * glyph_size_uv;
            let alpha = textureSampleLevel(atlas_r8_texture, atlas_r8_sampler, sample_uv, 0.0).r;
            final_color = mix(bg_color, fg_color, alpha);

            // DEBUG: Show red tint where glyph should appear
            // final_color = vec3<f32>(final_color.r + 0.3, final_color.g, final_color.b);
        }
    }


    // Cursor
    let cursor_pos = uniforms.text_grid_cursor_pos;
    if (uniforms.text_grid_cursor_visible > 0.5 &&
        u32(cell_col) == u32(cursor_pos.x) &&
        u32(cell_row) == u32(cursor_pos.y)) {
        let local_uv = local_px / cell_size;
        let shape = i32(uniforms.text_grid_cursor_shape);
        var draw_cursor = false;
        if (shape == 2) {
            draw_cursor = local_uv.y > 0.85;
        } else if (shape == 3) {
            draw_cursor = local_uv.x < 0.1;
        } else {
            draw_cursor = true;
        }
        if (draw_cursor) {
            final_color = vec3<f32>(1.0, 1.0, 1.0) - final_color;
        }
    }

    return vec4<f32>(final_color, 1.0);
}
