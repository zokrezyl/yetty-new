// local_id 0x000c — star

fn shader_glyph_12_sdStar(p: vec2<f32>, r: f32, n: i32, m: f32) -> f32 {
    let an = 3.141593 / f32(n);
    let en = 3.141593 / m;
    let acs = vec2<f32>(cos(an), sin(an));
    let ecs = vec2<f32>(cos(en), sin(en));

    var bn = atan2(p.x, p.y) % (2.0 * an) - an;
    var q = length(p) * vec2<f32>(cos(bn), abs(sin(bn)));

    q = q - r * acs;
    q = q + ecs * clamp(-dot(q, ecs), 0.0, r * acs.y / ecs.y);

    return length(q) * sign(q.x);
}

fn shader_glyph_12(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    // fg unused - star has its own golden color

    let p = (local_uv - 0.5) * 2.0;

    // Pulsing glow
    let pulse = 0.8 + 0.2 * sin(time * 3.0);
    let glow = 1.0 + 0.3 * sin(time * 5.0);

    // Star shape
    let star = shader_glyph_12_sdStar(p, 0.5 * pulse, 5, 2.5);

    var color = bg;
    var alpha = 0.0;

    // Outer glow
    if (star < 0.3) {
        let glowIntensity = (0.3 - star) / 0.3;
        color = vec3<f32>(1.0, 0.9, 0.5) * glowIntensity * glow * 0.5;
        alpha = glowIntensity * 0.5;
    }

    // Star body
    if (star < 0.0) {
        color = vec3<f32>(1.0, 0.95, 0.4) * glow;
        alpha = 1.0;
    }

    return mix(bg, color, alpha);
}
