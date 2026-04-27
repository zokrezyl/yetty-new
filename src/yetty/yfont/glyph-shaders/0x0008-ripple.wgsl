// local_id 0x0008 — ripple

fn shader_glyph_8(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    // fg unused - ripple has its own water colors

    let p = (local_uv - 0.5) * 2.0;
    let d = length(p);

    // Ripple waves
    let wave = sin(d * 15.0 - time * 4.0) * 0.5 + 0.5;
    let fade = 1.0 - smoothstep(0.0, 1.0, d);

    // Blue water color
    let base = vec3<f32>(0.1, 0.3, 0.8);
    let highlight = vec3<f32>(0.4, 0.7, 1.0);

    let waterColor = mix(base, highlight, wave * fade);
    let alpha = fade * (0.6 + wave * 0.4);

    return mix(bg, waterColor, alpha);
}
