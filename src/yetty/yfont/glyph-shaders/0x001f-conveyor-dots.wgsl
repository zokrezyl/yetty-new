// local_id 0x001f — conveyor-dots

fn shader_glyph_31(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    let dotRadius = 0.08;
    let numDots = 4;
    let spacing = 1.0 / f32(numDots);
    let speed = 0.4;
    let cy = 0.5;

    var alpha = 0.0;

    for (var i = 0; i < 4; i++) {
        // Each dot starts at evenly spaced positions, drifts right and wraps
        var x = f32(i) * spacing + time * speed;
        x = x - floor(x); // wrap to [0, 1)

        let pos = vec2<f32>(x, cy);
        let d = length(local_uv - pos);
        alpha = max(alpha, smoothstep(dotRadius, dotRadius * 0.3, d));
    }

    return mix(bg, fg, alpha);
}
