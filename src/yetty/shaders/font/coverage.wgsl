// =============================================================================
// Vector Coverage Font Rendering
// FontRenderMethod::Coverage
//
// Renders glyphs using multi-sample coverage calculation from Bezier curves.
// More accurate anti-aliasing than SDF, better at small sizes.
// Curves are stored in coverageFontBuffer, offsets in coverageFontOffsetTable.
// =============================================================================

// Bindings (binding numbers set by ShaderManager based on registration order)
// COVERAGE_FONT_BINDINGS_PLACEHOLDER

// Winding number for coverage font (uses coverageFontBuffer)
fn coverageWindingNumber(glyphIndex: u32, pos: vec2<f32>) -> i32 {
    let codepoint = glyphIndex;
    let offset = coverageFontOffsetTable[codepoint];
    if (offset == 0xFFFFFFFFu) { return 0; }

    let header = coverageFontBuffer[offset];
    let curveCount = (header >> 16u) & 0xFFFFu;
    if (curveCount == 0u) { return 0; }

    var crossings = 0;
    let curveBase = offset + 1u;

    for (var i = 0u; i < curveCount; i++) {
        let curveOffset = curveBase + i * 3u;
        let p0 = unpackPoint(coverageFontBuffer[curveOffset]);
        let p1 = unpackPoint(coverageFontBuffer[curveOffset + 1u]);
        let p2 = unpackPoint(coverageFontBuffer[curveOffset + 2u]);

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

// Sample coverage glyph using multi-sample anti-aliasing (4x4 grid)
// Returns coverage value [0,1] where 1 = fully inside glyph
fn sampleCoverageGlyph(glyphIndex: u32, localUV: vec2<f32>, pixelSize: vec2<f32>) -> f32 {
    let codepoint = glyphIndex;
    let offset = coverageFontOffsetTable[codepoint];
    if (offset == 0xFFFFFFFFu) {
        return 0.0;  // Glyph not present
    }

    let header = coverageFontBuffer[offset];
    let curveCount = (header >> 16u) & 0xFFFFu;
    if (curveCount == 0u) {
        return 0.0;  // Empty glyph
    }

    // 4x4 supersampling for coverage calculation
    var coverage = 0.0;
    let samples = 4;
    let sampleWeight = 1.0 / f32(samples * samples);

    for (var sy = 0; sy < samples; sy++) {
        for (var sx = 0; sx < samples; sx++) {
            // Jittered sample position within pixel
            let sampleOffset = vec2<f32>(
                (f32(sx) + 0.5) / f32(samples) - 0.5,
                (f32(sy) + 0.5) / f32(samples) - 0.5
            );
            let samplePos = localUV + sampleOffset * pixelSize;

            // Check if sample is inside glyph using winding number
            let winding = coverageWindingNumber(glyphIndex, samplePos);
            if (winding != 0) {
                coverage += sampleWeight;
            }
        }
    }

    return coverage;
}

// Render coverage glyph
// Returns: (color, hasGlyph)
fn renderCoverageGlyph(
    glyphIndex: u32,
    localPx: vec2<f32>,
    cellSize: vec2<f32>,
    bgColor: vec3<f32>,
    fgColor: vec3<f32>
) -> vec4<f32> {
    let localUV = vec2<f32>(
        localPx.x / cellSize.x,
        1.0 - localPx.y / cellSize.y
    );

    // Pixel size in UV space
    let pixelSize = vec2<f32>(1.0 / cellSize.x, 1.0 / cellSize.y);

    // Get coverage value (0-1)
    let coverage = sampleCoverageGlyph(glyphIndex, localUV, pixelSize);

    let color = mix(bgColor, fgColor, coverage);
    let hasGlyph = select(0.0, 1.0, coverage > 0.01);
    return vec4<f32>(color, hasGlyph);
}
