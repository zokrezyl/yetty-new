// =============================================================================
// Vector Font SDF Rendering
// FontRenderMethod::Vector
//
// Renders glyphs using signed distance field calculations from Bezier curves.
// Curves are stored in vectorFontBuffer, offsets in vectorFontOffsetTable.
// =============================================================================

// Bindings (binding numbers set by ShaderManager based on registration order)
// VECTOR_FONT_BINDINGS_PLACEHOLDER

// Unpack a normalized coordinate from u32 (x in high 16 bits, y in low 16 bits)
fn unpackPoint(packed: u32) -> vec2<f32> {
    let x = f32((packed >> 16u) & 0xFFFFu) / 65535.0;
    let y = f32(packed & 0xFFFFu) / 65535.0;
    return vec2<f32>(x, y);
}

// Unsigned distance to a quadratic Bezier curve (Inigo Quilez)
fn sdQuadraticBezier(pos: vec2<f32>, p0: vec2<f32>, p1: vec2<f32>, p2: vec2<f32>) -> f32 {
    let a = p1 - p0;
    let b = p0 - 2.0 * p1 + p2;
    let c = a * 2.0;
    let d = p0 - pos;

    let kk = 1.0 / dot(b, b);
    let kx = kk * dot(a, b);
    let ky = kk * (2.0 * dot(a, a) + dot(d, b)) / 3.0;
    let kz = kk * dot(d, a);

    let p = ky - kx * kx;
    let q = kx * (2.0 * kx * kx - 3.0 * ky) + kz;
    let p3 = p * p * p;
    let h = q * q + 4.0 * p3;

    var res: f32;
    if (h >= 0.0) {
        let sqrtH = sqrt(h);
        let x = (vec2<f32>(sqrtH, -sqrtH) - q) / 2.0;
        let uv = sign(x) * pow(abs(x), vec2<f32>(1.0 / 3.0));
        let t = clamp(uv.x + uv.y - kx, 0.0, 1.0);
        let qpos = d + (c + b * t) * t;
        res = dot(qpos, qpos);
    } else {
        let z = sqrt(-p);
        let v = acos(q / (p * z * 2.0)) / 3.0;
        let m = cos(v);
        let n = sin(v) * 1.732050808;
        let t0 = clamp((m + m) * z - kx, 0.0, 1.0);
        let t1 = clamp((-n - m) * z - kx, 0.0, 1.0);
        let t2 = clamp((n - m) * z - kx, 0.0, 1.0);
        let qpos0 = d + (c + b * t0) * t0;
        let qpos1 = d + (c + b * t1) * t1;
        let qpos2 = d + (c + b * t2) * t2;
        res = min(min(dot(qpos0, qpos0), dot(qpos1, qpos1)), dot(qpos2, qpos2));
    }
    return sqrt(res);
}

// Ray-cast winding number for inside/outside test
fn windingNumber(glyphIndex: u32, pos: vec2<f32>) -> i32 {
    let codepoint = glyphIndex;
    let offset = vectorFontOffsetTable[codepoint];
    if (offset == 0xFFFFFFFFu) { return 0; }

    let header = vectorFontBuffer[offset];
    let curveCount = (header >> 16u) & 0xFFFFu;
    if (curveCount == 0u) { return 0; }

    var crossings = 0;
    let curveBase = offset + 1u;

    for (var i = 0u; i < curveCount; i++) {
        let curveOffset = curveBase + i * 3u;
        let p0 = unpackPoint(vectorFontBuffer[curveOffset]);
        let p1 = unpackPoint(vectorFontBuffer[curveOffset + 1u]);
        let p2 = unpackPoint(vectorFontBuffer[curveOffset + 2u]);

        // Solve for t where curve.y = pos.y
        let y0 = p0.y - pos.y;
        let y1 = p1.y - pos.y;
        let y2 = p2.y - pos.y;

        let aa = y0 - 2.0 * y1 + y2;
        let bb = 2.0 * (y1 - y0);
        let cc = y0;

        if (abs(aa) < 1e-6) {
            // Linear case
            if (abs(bb) > 1e-6) {
                let t = -cc / bb;
                if (t >= 0.0 && t < 1.0) {
                    let mt = 1.0 - t;
                    let x = mt * mt * p0.x + 2.0 * mt * t * p1.x + t * t * p2.x;
                    if (x > pos.x) {
                        let dy = 2.0 * mt * (p1.y - p0.y) + 2.0 * t * (p2.y - p1.y);
                        crossings += select(-1, 1, dy > 0.0);
                    }
                }
            }
        } else {
            let disc = bb * bb - 4.0 * aa * cc;
            if (disc >= 0.0) {
                let sqrtDisc = sqrt(disc);
                let t1 = (-bb - sqrtDisc) / (2.0 * aa);
                let t2 = (-bb + sqrtDisc) / (2.0 * aa);

                if (t1 >= 0.0 && t1 < 1.0) {
                    let mt = 1.0 - t1;
                    let x = mt * mt * p0.x + 2.0 * mt * t1 * p1.x + t1 * t1 * p2.x;
                    if (x > pos.x) {
                        let dy = 2.0 * mt * (p1.y - p0.y) + 2.0 * t1 * (p2.y - p1.y);
                        crossings += select(-1, 1, dy > 0.0);
                    }
                }
                if (t2 >= 0.0 && t2 < 1.0 && abs(t2 - t1) > 1e-6) {
                    let mt = 1.0 - t2;
                    let x = mt * mt * p0.x + 2.0 * mt * t2 * p1.x + t2 * t2 * p2.x;
                    if (x > pos.x) {
                        let dy = 2.0 * mt * (p1.y - p0.y) + 2.0 * t2 * (p2.y - p1.y);
                        crossings += select(-1, 1, dy > 0.0);
                    }
                }
            }
        }
    }
    return crossings;
}

// Render a vector glyph using SDF curves
// glyphIndex: glyph index from cell
// localUV: position within cell, normalized [0,1]
// Returns: signed distance (negative = inside, positive = outside)
fn sampleVectorGlyph(glyphIndex: u32, localUV: vec2<f32>) -> f32 {
    let codepoint = glyphIndex;

    // Look up offset in the offset table
    let offset = vectorFontOffsetTable[codepoint];
    if (offset == 0xFFFFFFFFu) {
        return 1.0;  // Glyph not present, return "outside"
    }

    // Read header: curveCount (upper 16 bits) | flags (lower 16 bits)
    let header = vectorFontBuffer[offset];
    let curveCount = (header >> 16u) & 0xFFFFu;

    if (curveCount == 0u) {
        return 1.0;  // No curves, empty glyph
    }

    // Find minimum distance to all curves
    var minDist = 1000.0;
    let curveBase = offset + 1u;

    for (var i = 0u; i < curveCount; i = i + 1u) {
        let curveOffset = curveBase + i * 3u;
        let p0 = unpackPoint(vectorFontBuffer[curveOffset]);
        let p1 = unpackPoint(vectorFontBuffer[curveOffset + 1u]);
        let p2 = unpackPoint(vectorFontBuffer[curveOffset + 2u]);

        // Early-out: compute distance to curve bounding box
        // If box is farther than current minDist, skip expensive SDF calculation
        let boxMin = min(min(p0, p1), p2);
        let boxMax = max(max(p0, p1), p2);
        let dBox = max(boxMin - localUV, localUV - boxMax);
        let distToBox = length(max(dBox, vec2<f32>(0.0)));

        if (distToBox < minDist) {
            let dist = sdQuadraticBezier(localUV, p0, p1, p2);
            minDist = min(minDist, dist);
        }
    }

    // Use winding number for inside/outside (non-zero = inside)
    let winding = windingNumber(glyphIndex, localUV);
    if (winding != 0) {
        return -minDist;
    }
    return minDist;
}

// Render vector SDF glyph
// Returns: (color, hasGlyph)
fn renderVectorGlyph(
    glyphIndex: u32,
    localPx: vec2<f32>,
    cellSize: vec2<f32>,
    bgColor: vec3<f32>,
    fgColor: vec3<f32>
) -> vec4<f32> {
    // Curves are normalized to font metrics (ascender/descender/advance)
    // Just flip Y for screen coordinates
    let localUV = vec2<f32>(
        localPx.x / cellSize.x,
        1.0 - localPx.y / cellSize.y
    );

    let sd = sampleVectorGlyph(glyphIndex, localUV);

    // Anti-aliasing: compute pixel width in UV space from cell size
    // One pixel in UV space = 1.0 / cellSize
    let pixelWidth = 1.0 / min(cellSize.x, cellSize.y);

    // Smooth anti-aliasing with ~1.5 pixel transition zone
    let alpha = clamp(0.5 - sd / (pixelWidth * 1.5), 0.0, 1.0);

    let color = mix(bgColor, fgColor, alpha);
    let hasGlyph = select(0.0, 1.0, alpha > 0.01);
    return vec4<f32>(color, hasGlyph);
}
