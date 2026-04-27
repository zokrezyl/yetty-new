// local_id 0x0027 — typing-dots

fn shader_glyph_39(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    let dotRadius = 0.08;
    let spacing = 0.22;
    let baseY = 0.55;
    let bounceHeight = 0.15;
    let speed = 4.0;
    let stagger = 0.5;

    var alpha = 0.0;

    for (var i = 0; i < 3; i++) {
        let fi = f32(i);
        let dotX = 0.5 + (fi - 1.0) * spacing;

        // Each dot bounces with a staggered phase
        let phase = time * speed - fi * stagger;
        // Smooth bounce: only the positive half of sin, rest stays at baseline
        let bounce = max(sin(phase), 0.0);
        let dotY = baseY - bounce * bounceHeight;

        let d = length(local_uv - vec2<f32>(dotX, dotY));
        alpha = max(alpha, smoothstep(dotRadius, dotRadius * 0.3, d));
    }

    return mix(bg, fg, alpha);
}
