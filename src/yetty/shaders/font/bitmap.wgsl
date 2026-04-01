// =============================================================================
// Bitmap/Emoji Font Rendering
//
// Pre-rendered RGBA bitmap glyphs (emoji, color fonts).
// Requires: emojiFontTexture, emojiFontSampler, emojiFontBuffer (EmojiGlyphMetadata)
// =============================================================================

// Bindings (binding numbers set by ShaderManager based on registration order)
// BITMAP_FONT_BINDINGS_PLACEHOLDER

// Render bitmap/emoji glyph
// Returns: (color, hasGlyph)
fn renderBitmapGlyph(
    glyphIndex: u32,
    localPx: vec2<f32>,
    cellSize: vec2<f32>,
    bgColor: vec3<f32>
) -> vec4<f32> {
    let emoji = emojiFontBuffer[glyphIndex];

    // Emoji fills the cell, centered and scaled to fit
    // emoji.size contains the actual glyph size in atlas pixels
    let emojiAspect = emoji.size.x / emoji.size.y;
    let cellAspect = cellSize.x / cellSize.y;

    // Scale emoji to fit within cell while maintaining aspect ratio
    var scaledSize: vec2<f32>;
    if (emojiAspect > cellAspect) {
        // Emoji is wider - fit to width
        scaledSize = vec2<f32>(cellSize.x, cellSize.x / emojiAspect);
    } else {
        // Emoji is taller - fit to height
        scaledSize = vec2<f32>(cellSize.y * emojiAspect, cellSize.y);
    }

    // Center in cell
    let offset = (cellSize - scaledSize) * 0.5;

    // Check if inside emoji bounds
    if (localPx.x >= offset.x && localPx.x < offset.x + scaledSize.x &&
        localPx.y >= offset.y && localPx.y < offset.y + scaledSize.y) {
        // Calculate UV for sampling
        let emojiLocalPos = (localPx - offset) / scaledSize;
        let uv = mix(emoji.uvMin, emoji.uvMax, emojiLocalPos);

        // Sample emoji texture (pre-rendered RGBA)
        let emojiColor = textureSampleLevel(emojiFontTexture, emojiFontSampler, uv, 0.0);

        // Alpha blend emoji over background
        let color = mix(bgColor, emojiColor.rgb, emojiColor.a);
        let hasGlyph = select(0.0, 1.0, emojiColor.a > 0.01);
        return vec4<f32>(color, hasGlyph);
    }

    // Outside emoji bounds
    return vec4<f32>(bgColor, 0.0);
}
