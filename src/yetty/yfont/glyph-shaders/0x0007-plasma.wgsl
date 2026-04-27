// local_id 0x0007 — plasma

fn shader_glyph_7(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    // fg/bg unused - plasma has its own colors
    // Use pixel position for seamless tiling across cells
    // Scale down for nice pattern size
    let p = pixel_pos * 0.02;
    let t = time * 0.5;

    var v = 0.0;
    v += sin(p.x + t);
    v += sin(p.y + t * 0.5);
    v += sin(p.x + p.y + t * 0.3);
    v += sin(sqrt(p.x * p.x + p.y * p.y) * 0.5 + t);
    v /= 4.0;

    // Rainbow colors
    let r = sin(v * 3.14159 + 0.0) * 0.5 + 0.5;
    let g = sin(v * 3.14159 + 2.094) * 0.5 + 0.5;
    let b = sin(v * 3.14159 + 4.188) * 0.5 + 0.5;

    return vec3<f32>(r, g, b);
}
