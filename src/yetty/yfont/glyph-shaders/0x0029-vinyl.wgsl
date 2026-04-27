// local_id 0x0029 — vinyl

fn shader_glyph_41(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    let center = vec2<f32>(0.5, 0.5);
    let uv = local_uv - center;
    let dist = length(uv);
    let angle = atan2(uv.y, uv.x);

    let discRadius = 0.44;
    let labelRadius = 0.12;
    let holeRadius = 0.03;

    // Disc mask
    let disc = smoothstep(discRadius + 0.02, discRadius, dist);

    // Center hole
    let hole = smoothstep(holeRadius, holeRadius + 0.015, dist);

    // Label area (brighter center)
    let label = smoothstep(labelRadius + 0.02, labelRadius, dist) * 0.6;

    // Grooves: concentric rings modulated by angle (spinning)
    let spin = time * 3.0;
    let grooveFreq = 60.0;
    let groove = sin(dist * grooveFreq + angle * 0.5 + spin) * 0.5 + 0.5;
    let grooveMask = step(labelRadius + 0.03, dist) * step(dist, discRadius - 0.01);
    let grooveAlpha = groove * 0.15 * grooveMask;

    // Specular highlight that rotates
    let hlAngle = angle - time * 0.5;
    let hl = smoothstep(0.3, 0.0, abs(sin(hlAngle))) * 0.2 *
             smoothstep(labelRadius, labelRadius + 0.05, dist) *
             step(dist, discRadius);

    let alpha = (disc * hole * 0.7 + label + grooveAlpha + hl);
    return mix(bg, fg, clamp(alpha, 0.0, 1.0));
}
