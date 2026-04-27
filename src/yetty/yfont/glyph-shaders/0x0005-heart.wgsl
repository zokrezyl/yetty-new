// local_id 0x0005 — heart

fn shader_glyph_5_sdHeart(p: vec2<f32>) -> f32 {
    let q = vec2<f32>(abs(p.x), p.y);
    let w = q - vec2<f32>(0.25, 0.75);

    if (q.x + q.y > 1.0) {
        return sqrt(dot(w, w)) - 0.25;
    }

    let b = vec2<f32>(0.0, 1.0) - q;
    let c = vec2<f32>(-0.25, 0.75) - q;

    return sqrt(min(dot(b, b), dot(c, c))) * sign(q.x - q.y);
}

fn shader_glyph_5(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    // fg unused - heart has its own red color

    var p = (local_uv - 0.5) * 2.2;
    p.y -= 0.1;

    // Heartbeat animation
    let beat = 1.0 + 0.1 * sin(time * 6.0) * exp(-2.0 * fract(time * 1.5));
    p /= beat;

    let d = shader_glyph_5_sdHeart(p);

    // Red heart with soft edge
    let heartColor = vec3<f32>(0.9, 0.1, 0.2);
    let alpha = 1.0 - smoothstep(-0.02, 0.02, d);

    // Add glow
    let glow = 0.02 / (abs(d) + 0.02);
    let glowColor = heartColor * glow * 0.5;

    let finalAlpha = alpha + glow * 0.3;
    let finalColor = heartColor * alpha + glowColor;

    return mix(bg, finalColor, clamp(finalAlpha, 0.0, 1.0));
}
