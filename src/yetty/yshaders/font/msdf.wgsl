// =============================================================================
// MSDF Font Rendering
//
// Multi-channel Signed Distance Field text rendering.
// Requires: msdfFontTexture, msdfFontSampler, msdfFontBuffer (GlyphMetadata)
// =============================================================================

// Bindings (binding numbers set by ShaderManager based on registration order)
// MSDF_FONT_BINDINGS_PLACEHOLDER

// Render MSDF glyph
// Returns: (color, hasGlyph)
fn renderMsdfGlyph(
    glyphIndex: u32,
    localPx: vec2<f32>,
    cellSize: vec2<f32>,
    scale: f32,
    pixelRange: f32,
    bgColor: vec3<f32>,
    fgColor: vec3<f32>
) -> vec4<f32> {
    let glyph = msdfFontBuffer[glyphIndex];

    // Calculate glyph position within cell
    let scaledGlyphSize = glyph.size * scale;
    let scaledBearing = glyph.bearing * scale;

    // Baseline at 80% of cell height
    let baseline = cellSize.y * 0.8;
    let glyphTop = baseline - scaledBearing.y;
    let glyphLeft = scaledBearing.x;

    // Glyph bounds in cell pixel space
    let glyphMinPx = vec2<f32>(glyphLeft, glyphTop);
    let glyphMaxPx = vec2<f32>(glyphLeft + scaledGlyphSize.x, glyphTop + scaledGlyphSize.y);

    // Check if inside glyph bounds
    if (localPx.x >= glyphMinPx.x && localPx.x < glyphMaxPx.x &&
        localPx.y >= glyphMinPx.y && localPx.y < glyphMaxPx.y) {
        // Calculate UV for MSDF sampling
        let glyphLocalPos = (localPx - glyphMinPx) / scaledGlyphSize;
        let uv = mix(glyph.uvMin, glyph.uvMax, glyphLocalPos);

        // Sample MSDF texture
        let msdf = textureSampleLevel(msdfFontTexture, msdfFontSampler, uv, 0.0);

        // Calculate signed distance
        let sd = median(msdf.r, msdf.g, msdf.b);

        // Apply anti-aliased edge
        let screenPxRange = pixelRange * scale;
        let alpha = clamp((sd - 0.5) * screenPxRange + 0.5, 0.0, 1.0);

        // Blend foreground over background, encode hasGlyph in alpha
        let color = mix(bgColor, fgColor, alpha);
        let hasGlyph = select(0.0, 1.0, alpha > 0.01);
        return vec4<f32>(color, hasGlyph);
    }

    // Outside glyph bounds
    return vec4<f32>(bgColor, 0.0);
}
