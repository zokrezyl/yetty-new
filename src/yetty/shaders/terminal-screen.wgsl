// Terminal screen shader - raster font rendering only
// Simplified version of gpu-screen.wgsl for TerminalScreen

// Grid uniforms (managed by renderer, group 0)
// Must match GridUniforms in terminal-screen.cpp (272 bytes)
struct GridUniforms {
    projection: mat4x4<f32>,   // 64 bytes
    screenSize: vec2<f32>,     // 8 bytes
    cellSize: vec2<f32>,       // 8 bytes
    gridSize: vec2<f32>,       // 8 bytes (cols, rows)
    pixelRange: f32,           // 4 bytes
    scale: f32,                // 4 bytes
    cursorPos: vec2<f32>,      // 8 bytes (col, row)
    cursorVisible: f32,        // 4 bytes
    cursorShape: f32,          // 4 bytes (1=block, 2=underline, 3=bar)
    viewportOrigin: vec2<f32>, // 8 bytes - viewport origin in framebuffer
    cursorBlink: f32,          // 4 bytes (0=no blink, 1=blink)
    hasSelection: f32,         // 4 bytes (0=no, 1=yes)
    selStart: vec2<f32>,       // 8 bytes (col, row)
    selEnd: vec2<f32>,         // 8 bytes (col, row)
    preEffectIndex: u32,       // 4 bytes (unused)
    postEffectIndex: u32,      // 4 bytes (unused)
    preEffectP0: f32,          // 4 bytes (unused)
    preEffectP1: f32,          // 4 bytes
    preEffectP2: f32,          // 4 bytes
    preEffectP3: f32,          // 4 bytes
    preEffectP4: f32,          // 4 bytes
    preEffectP5: f32,          // 4 bytes
    postEffectP0: f32,         // 4 bytes (unused)
    postEffectP1: f32,         // 4 bytes
    postEffectP2: f32,         // 4 bytes
    postEffectP3: f32,         // 4 bytes
    postEffectP4: f32,         // 4 bytes
    postEffectP5: f32,         // 4 bytes
    defaultFg: u32,            // 4 bytes - packed RGBA
    defaultBg: u32,            // 4 bytes - packed RGB
    spaceGlyph: u32,           // 4 bytes (unused)
    effectIndex: u32,          // 4 bytes (unused)
    effectP0: f32,             // 4 bytes
    effectP1: f32,             // 4 bytes
    effectP2: f32,             // 4 bytes
    effectP3: f32,             // 4 bytes
    effectP4: f32,             // 4 bytes
    effectP5: f32,             // 4 bytes
    visualZoomScale: f32,      // 4 bytes (unused)
    visualZoomOffsetX: f32,    // 4 bytes
    visualZoomOffsetY: f32,    // 4 bytes
    ypaintScrollingSlot: u32,  // 4 bytes (unused)
    ypaintOverlaySlot: u32,    // 4 bytes (unused)
    _pad0: f32,                // 4 bytes
    _pad1: f32,                // 4 bytes
    _pad2: f32,                // 4 bytes
};  // Total: 272 bytes

// Raster glyph UV (8 bytes per glyph)
struct RasterGlyphUV {
    uv: vec2<f32>,
};

// Cell structure (12 bytes) - matches C++ TextCell
struct Cell {
    glyph: u32,    // 4 bytes
    fg: u32,       // 4 bytes - packed: R | (G << 8) | (B << 16) | (A << 24)
    bg: u32,       // 4 bytes - packed: R | (G << 8) | (B << 16) | (style << 24)
};

// Bindings (group 0)
@group(0) @binding(0) var<uniform> grid: GridUniforms;
@group(0) @binding(1) var<storage, read> cellBuffer: array<Cell>;
@group(0) @binding(2) var rasterTexture: texture_2d<f32>;
@group(0) @binding(3) var rasterSampler: sampler;
@group(0) @binding(4) var<storage, read> rasterMetadata: array<RasterGlyphUV>;

// Vertex output
struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

// Fullscreen quad vertex shader
@vertex
fn vs_main(@builtin(vertex_index) vertexIndex: u32) -> VertexOutput {
    // Fullscreen triangle (vertices 0,1,2 cover the screen)
    var positions = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0),
        vec2<f32>(3.0, -1.0),
        vec2<f32>(-1.0, 3.0)
    );
    var uvs = array<vec2<f32>, 3>(
        vec2<f32>(0.0, 1.0),
        vec2<f32>(2.0, 1.0),
        vec2<f32>(0.0, -1.0)
    );

    var output: VertexOutput;
    output.position = vec4<f32>(positions[vertexIndex], 0.0, 1.0);
    output.uv = uvs[vertexIndex];
    return output;
}

// Unpack color from u32
fn unpackColor(packed: u32) -> vec4<f32> {
    let r = f32(packed & 0xFFu) / 255.0;
    let g = f32((packed >> 8u) & 0xFFu) / 255.0;
    let b = f32((packed >> 16u) & 0xFFu) / 255.0;
    let a = f32((packed >> 24u) & 0xFFu) / 255.0;
    return vec4<f32>(r, g, b, a);
}

// Fragment shader
@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    // Convert UV to pixel coordinates
    let pixelPos = input.uv * grid.screenSize;

    // Calculate cell position
    let cellPos = floor(pixelPos / grid.cellSize);
    let col = i32(cellPos.x);
    let row = i32(cellPos.y);

    // Bounds check
    let cols = i32(grid.gridSize.x);
    let rows = i32(grid.gridSize.y);
    if (col < 0 || col >= cols || row < 0 || row >= rows) {
        // Outside grid - return default background
        return unpackColor(grid.defaultBg);
    }

    // Get cell
    let cellIndex = row * cols + col;
    let cell = cellBuffer[cellIndex];

    // Unpack colors
    let fgColor = unpackColor(cell.fg);
    let bgColor = unpackColor(cell.bg);

    // Position within cell (0 to cellSize)
    let localPx = pixelPos - cellPos * grid.cellSize;

    // Default to background
    var finalColor = bgColor.rgb;

    // Render glyph if present
    let glyphIndex = cell.glyph;
    if (glyphIndex > 0u) {
        let glyphUV = rasterMetadata[glyphIndex].uv;

        // Skip empty glyphs (marked with negative UV)
        if (glyphUV.x >= 0.0) {
            // Compute UV within the glyph cell
            let glyphSizeUV = grid.cellSize / 1024.0;
            let localUV = localPx / grid.cellSize;
            let sampleUV = glyphUV + localUV * glyphSizeUV;

            // Sample grayscale texture (R channel)
            let alpha = textureSampleLevel(rasterTexture, rasterSampler, sampleUV, 0.0).r;

            finalColor = mix(bgColor.rgb, fgColor.rgb, alpha);
        }
    }

    // Cursor rendering
    if (grid.cursorVisible > 0.5) {
        let cursorCol = i32(grid.cursorPos.x);
        let cursorRow = i32(grid.cursorPos.y);

        if (col == cursorCol && row == cursorRow) {
            let cursorShape = i32(grid.cursorShape);

            if (cursorShape == 1) {
                // Block cursor - invert colors
                finalColor = vec3<f32>(1.0) - finalColor;
            } else if (cursorShape == 2) {
                // Underline cursor
                if (localPx.y > grid.cellSize.y - 2.0) {
                    finalColor = fgColor.rgb;
                }
            } else if (cursorShape == 3) {
                // Bar cursor
                if (localPx.x < 2.0) {
                    finalColor = fgColor.rgb;
                }
            }
        }
    }

    return vec4<f32>(finalColor, 1.0);
}
