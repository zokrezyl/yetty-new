// local_id 0x0002 — wave

fn shader_glyph_2(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    let numBars = 4.0;
    let barWidth = 0.15;
    let gap = (1.0 - numBars * barWidth) / (numBars + 1.0);

    // X bias from pixel position - wave flows across screen
    let xBias = pixel_pos.x * 0.02;

    var alpha = 0.0;

    for (var i = 0.0; i < numBars; i += 1.0) {
        let barX = gap + i * (barWidth + gap) + barWidth * 0.5;
        let barDist = abs(local_uv.x - barX);

        // Each bar has different phase, plus x position bias for traveling wave
        let phase = i * 0.8 + xBias;
        let height = 0.3 + 0.35 * sin(time * 3.0 - phase);

        // Bar from bottom, centered vertically
        let barBottom = 0.5 - height;
        let barTop = 0.5 + height;

        let inBar = step(barDist, barWidth * 0.5) *
                    step(barBottom, local_uv.y) *
                    step(local_uv.y, barTop);

        alpha = max(alpha, inBar);
    }

    return mix(bg, fg, alpha);
}
