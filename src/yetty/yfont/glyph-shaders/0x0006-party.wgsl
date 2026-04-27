// local_id 0x0006 — party

fn shader_glyph_6_hash(p: vec2<f32>) -> f32 {
    return fract(sin(dot(p, vec2<f32>(127.1, 311.7))) * 43758.5453);
}

fn shader_glyph_6(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    // fg unused - confetti has its own colors

    let p = (local_uv - 0.5) * 2.0;
    var color = bg;
    var alpha = 0.0;

    // Confetti particles
    for (var i = 0; i < 15; i++) {
        let fi = f32(i);
        let seed = vec2<f32>(fi * 1.23, fi * 2.34);
        let angle = shader_glyph_6_hash(seed) * 5.0 - 0.5;
        let speed = 0.4 + shader_glyph_6_hash(seed + 1.0) * 0.4;
        let t = fract(time * 0.7 + shader_glyph_6_hash(seed + 2.0));

        let origin = vec2<f32>(-0.3, -0.5);
        let dir = vec2<f32>(cos(angle), sin(angle));
        let pos = origin + dir * speed * t + vec2<f32>(0.0, t * t * 0.8);

        if (length(p - pos) < 0.1) {
            let h = shader_glyph_6_hash(seed + 3.0);
            var confettiColor: vec3<f32>;
            if (h < 0.33) { confettiColor = vec3<f32>(1.0, 0.3, 0.4); }
            else if (h < 0.66) { confettiColor = vec3<f32>(0.3, 0.8, 1.0); }
            else { confettiColor = vec3<f32>(1.0, 0.9, 0.2); }
            alpha = 1.0 - t * 0.5;
            color = confettiColor;
        }
    }

    return mix(bg, color, alpha);
}
