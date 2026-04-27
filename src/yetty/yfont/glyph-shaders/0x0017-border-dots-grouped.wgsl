// local_id 0x0017 — border-dots-grouped

fn borderDotsGrouped_perimeterPos(t: f32, w: f32, h: f32) -> vec2<f32> {
    let perimeter = 2.0 * (w + h);
    let tt = t - floor(t / perimeter) * perimeter;

    if (tt < w) {
        return vec2<f32>(tt, 0.0);
    }
    if (tt < w + h) {
        return vec2<f32>(w, tt - w);
    }
    if (tt < 2.0 * w + h) {
        return vec2<f32>(w - (tt - w - h), h);
    }
    return vec2<f32>(0.0, h - (tt - 2.0 * w - h));
}

fn shader_glyph_23(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    let margin = 0.08;
    let w = 1.0 - 2.0 * margin;
    let h = 1.0 - 2.0 * margin;
    let perimeter = 2.0 * (w + h);

    let uv = local_uv - vec2<f32>(margin);
    let speed = 1.5;
    let dotRadius = 0.09;
    let gap = 0.12;

    var alpha = 0.0;

    // Three dots close together, moving as a group
    for (var i = 0; i < 3; i++) {
        let phase = time * speed + f32(i) * gap;
        let pos = borderDotsGrouped_perimeterPos(phase, w, h);
        let d = length(uv - pos);
        alpha = max(alpha, smoothstep(dotRadius, dotRadius * 0.4, d));
    }

    return mix(bg, fg, alpha);
}
