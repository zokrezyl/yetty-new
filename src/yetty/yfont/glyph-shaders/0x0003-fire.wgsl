// local_id 0x0003 — fire

fn shader_glyph_3_hash(p: vec2<f32>) -> f32 {
    return fract(sin(dot(p, vec2<f32>(127.1, 311.7))) * 43758.5453);
}

fn shader_glyph_3_noise(p: vec2<f32>) -> f32 {
    let i = floor(p);
    let f = fract(p);
    let u = f * f * (3.0 - 2.0 * f);

    let a = shader_glyph_3_hash(i);
    let b = shader_glyph_3_hash(i + vec2<f32>(1.0, 0.0));
    let c = shader_glyph_3_hash(i + vec2<f32>(0.0, 1.0));
    let d = shader_glyph_3_hash(i + vec2<f32>(1.0, 1.0));

    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

fn shader_glyph_3_fbm(p: vec2<f32>) -> f32 {
    var v = 0.0;
    var a = 0.5;
    var q = p;
    for (var i = 0; i < 4; i++) {
        v += a * shader_glyph_3_noise(q);
        q = q * 2.0;
        a *= 0.5;
    }
    return v;
}

fn shader_glyph_3(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    // fg unused - fire has its own colors

    var p = local_uv;
    p.y = 1.0 - p.y;  // Flip so fire goes up

    // Animate
    let n = shader_glyph_3_fbm(vec2<f32>(p.x * 4.0, p.y * 3.0 - time * 2.0));

    // Fire shape - wider at bottom
    let shape = 1.0 - p.y;
    let width = 0.3 + shape * 0.4;
    let center = abs(p.x - 0.5);

    var fire = shape * (1.0 - center / width);
    fire = fire * (0.5 + n * 0.5);
    fire = clamp(fire, 0.0, 1.0);

    // Fire colors
    let r = fire;
    let g = fire * fire * 0.7;
    let b = fire * fire * fire * 0.3;

    let alpha = smoothstep(0.0, 0.2, fire);
    let fireColor = vec3<f32>(r, g, b);

    return mix(bg, fireColor, alpha);
}
