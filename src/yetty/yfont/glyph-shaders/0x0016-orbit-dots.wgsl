// local_id 0x0016 — orbit-dots

fn shader_glyph_22(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    let center = vec2<f32>(0.5, 0.5);
    let radiusX = 0.44;
    let radiusY = 0.38;
    let speed = 2.5;
    let dotRadius = 0.08;
    let dotSpacing = 0.18;
    let easeStrength = 0.8;

    var alpha = 0.0;

    for (var i = 0; i < 5; i++) {
        let rawAngle = time * speed - f32(i) * dotSpacing;
        // Non-linear easing: fast on right, slow/bunching on left (at PI)
        let easedAngle = rawAngle + easeStrength * sin(rawAngle);

        let pos = center + vec2<f32>(
            cos(easedAngle) * radiusX,
            sin(easedAngle) * radiusY
        );
        let d = length(local_uv - pos);
        alpha = max(alpha, smoothstep(dotRadius, dotRadius * 0.3, d));
    }

    return mix(bg, fg, alpha);
}
