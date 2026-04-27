// local_id 0x0022 — orbit

fn shader_glyph_34(local_uv: vec2<f32>, time: f32, fg: vec3<f32>, bg: vec3<f32>, pixel_pos: vec2<f32>) -> vec3<f32> {
    let center = vec2<f32>(0.5, 0.5);
    let planetRadius = 0.12;
    let moonRadius = 0.06;
    let orbitRadius = 0.32;
    let orbitSpeed = 1.8;

    // Orbit path (faint ellipse)
    let orbitRX = orbitRadius;
    let orbitRY = orbitRadius * 0.4;

    let angle = time * orbitSpeed;
    let moonPos = center + vec2<f32>(cos(angle) * orbitRX, sin(angle) * orbitRY);

    // Depth: moon behind planet when sin(angle) > 0
    let moonBehind = sin(angle) > 0.0;

    // Faint orbit path
    let orbitDist = length(vec2<f32>((local_uv.x - center.x) / orbitRX, (local_uv.y - center.y) / orbitRY));
    let orbitLine = smoothstep(0.04, 0.0, abs(orbitDist - 1.0)) * 0.15;

    // Planet
    let planetDist = length(local_uv - center);
    let planetAlpha = smoothstep(planetRadius, planetRadius * 0.5, planetDist);

    // Moon
    let moonDist = length(local_uv - moonPos);
    let moonAlpha = smoothstep(moonRadius, moonRadius * 0.3, moonDist);

    var alpha: f32;
    if (moonBehind) {
        // Moon behind: draw moon first, planet on top
        alpha = max(orbitLine, max(moonAlpha * 0.6, planetAlpha));
    } else {
        alpha = max(orbitLine, max(planetAlpha, moonAlpha));
    }

    return mix(bg, fg, alpha);
}
