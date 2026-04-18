// =============================================================================
// Raster Font Rendering
//
// Simple texture atlas with grayscale glyphs.
// Requires: rasterFontTexture, rasterFontSampler, rasterFontBuffer (RasterGlyphUV)
// =============================================================================

// Bindings (binding numbers set by ShaderManager based on registration order)
// RASTER_FONT_BINDINGS_PLACEHOLDER

// Render raster glyph
// Returns: (color, hasGlyph)
fn renderRasterGlyph(
    glyphIndex: u32,
    localPx: vec2<f32>,
    cellSize: vec2<f32>,
    bgColor: vec3<f32>,
    fgColor: vec3<f32>
) -> vec4<f32> {
    let glyphUV = rasterFontBuffer[glyphIndex].uv;

    // Skip empty glyphs (marked with negative UV)
    if (glyphUV.x >= 0.0) {
        // Compute UV within the glyph cell
        // glyphSizeUV should be passed as uniform, for now assume square cells
        let glyphSizeUV = vec2<f32>(cellSize.x / 1024.0, cellSize.y / 1024.0);
        let localUV = localPx / cellSize;
        let sampleUV = glyphUV + localUV * glyphSizeUV;

        // Sample grayscale texture (R channel) - use Level to avoid non-uniform control flow issue
        let alpha = textureSampleLevel(rasterFontTexture, rasterFontSampler, sampleUV, 0.0).r;

        let color = mix(bgColor, fgColor, alpha);
        let hasGlyph = select(0.0, 1.0, alpha > 0.01);
        return vec4<f32>(color, hasGlyph);
    }

    // Empty glyph
    return vec4<f32>(bgColor, 0.0);
}
