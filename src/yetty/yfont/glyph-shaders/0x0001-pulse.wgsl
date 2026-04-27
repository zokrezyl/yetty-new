// local_id 0x0001 — pulse

fn shader_glyph_1(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    let center = vec2<f32>(0.5, 0.5);
    let uv = local_uv - center;
    let dist = length(uv);

    // Pulsing radius
    let baseRadius = 0.3;
    let pulseAmount = 0.1;
    let pulseSpeed = 2.0;
    let radius = baseRadius + sin(time * pulseSpeed) * pulseAmount;

    // Soft circle
    let alpha = smoothstep(radius + 0.05, radius - 0.05, dist);

    return mix(bg, fg, alpha);
}
