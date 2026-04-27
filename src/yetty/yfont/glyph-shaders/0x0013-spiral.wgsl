// local_id 0x0013 — spiral

const SP_TAU: f32 = 6.28318530;

fn shader_glyph_19(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    var p = (local_uv - 0.5) * 2.2;
    p.y = -p.y;

    let r = length(p);
    let angle = atan2(p.y, p.x);

    // Archimedean spiral: r = growth * (theta)
    // For multi-arm: each arm offset by TAU/arms
    let arms = 3.0;
    let growth = 0.12;
    let rotation = time * 1.5;

    // Analytical distance to spiral:
    // The spiral arm nearest to polar coord (r, angle) satisfies:
    //   r_spiral = growth * (angle - rotation + TAU * k + TAU * arm/arms)
    // Solve for k: k = (r/growth - angle + rotation - TAU*arm/arms) / TAU
    // Round to nearest integer to get closest arm passage

    var minDist = 1e10;
    for (var arm = 0.0; arm < 3.0; arm += 1.0) {
        let armOffset = SP_TAU * arm / arms;
        let rawK = (r / growth - angle + rotation - armOffset) / SP_TAU;
        // Check two nearest integers
        for (var di = 0.0; di <= 1.0; di += 1.0) {
            let k = floor(rawK) + di;
            let spiralR = growth * (angle - rotation + armOffset + SP_TAU * k);
            if (spiralR > 0.0 && spiralR < 1.1) {
                // Distance between concentric circles at r and spiralR
                let d = abs(r - spiralR);
                minDist = min(minDist, d);
            }
        }
    }

    // Fade out at edges
    let fade = 1.0 - smoothstep(0.8, 1.0, r);

    let lineW = 0.04;
    let alpha = (1.0 - smoothstep(lineW - 0.02, lineW + 0.02, minDist)) * fade;

    // Soft glow
    let glow = 0.008 / (minDist + 0.008) * fade * 0.25;

    return mix(bg, fg, clamp(alpha + glow, 0.0, 1.0));
}
