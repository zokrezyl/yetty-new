// local_id 0x001a — radar-sweep

fn shader_glyph_26(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    let center = vec2<f32>(0.5, 0.5);
    let uv = local_uv - center;
    let dist = length(uv);
    let angle = atan2(uv.y, uv.x);

    let sweepSpeed = 2.0;
    let sweepAngle = time * sweepSpeed;
    let trailLength = 2.0;

    // Ring mask
    let outerRadius = 0.45;
    let ringMask = smoothstep(outerRadius + 0.03, outerRadius, dist);

    // Sweep trail
    var diff = angle - sweepAngle;
    diff = diff - floor(diff / 6.28318) * 6.28318;
    let trail = smoothstep(trailLength, 0.0, diff) * ringMask;

    // Center dot
    let centerDot = smoothstep(0.05, 0.02, dist);

    // Faint circle outline
    let ring = smoothstep(0.025, 0.0, abs(dist - outerRadius)) * 0.2;

    let alpha = max(max(trail, centerDot), ring);

    return mix(bg, fg, alpha);
}
