// local_id 0x0026 — heartbeat

fn heartbeat_wave(x: f32) -> f32 {
    // ECG-like waveform over one cycle [0, 1]
    // Flat baseline with a sharp spike
    let t = fract(x);

    // P wave (small bump)
    let p = 0.08 * smoothstep(0.05, 0.1, t) * smoothstep(0.2, 0.15, t);

    // QRS complex (sharp spike)
    let q = -0.05 * smoothstep(0.28, 0.32, t) * smoothstep(0.36, 0.32, t);
    let r = 0.45 * smoothstep(0.32, 0.37, t) * smoothstep(0.42, 0.37, t);
    let s = -0.1 * smoothstep(0.38, 0.43, t) * smoothstep(0.48, 0.43, t);

    // T wave (broad bump)
    let tw = 0.12 * smoothstep(0.5, 0.58, t) * smoothstep(0.72, 0.64, t);

    return p + q + r + s + tw;
}

fn shader_glyph_38(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    let scrollSpeed = 0.4;
    let x = local_uv.x + time * scrollSpeed;
    let baseline = 0.5;
    let waveY = baseline - heartbeat_wave(x);
    let lineWidth = 0.03;

    let dist = abs(local_uv.y - waveY);
    let alpha = smoothstep(lineWidth, lineWidth * 0.3, dist);

    return mix(bg, fg, alpha);
}
