/* Blender shader — composites layer textures with alpha-over blending and
 * applies an optional non-intrusive visual zoom (Ctrl+Scroll).
 *
 * The zoom is a UV transform only: no cols/rows change, no reflow. It runs
 * after all layers have rendered, so font/SDF output is sampled bilinearly.
 * For structural (cell-size) zoom see terminal.c — that resizes the grid.
 */

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

/* Full-screen quad vertex shader */
@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> VertexOutput {
    /* Two triangles forming a full-screen quad */
    var positions = array<vec2<f32>, 6>(
        vec2(-1.0, -1.0),
        vec2( 1.0, -1.0),
        vec2( 1.0,  1.0),
        vec2(-1.0, -1.0),
        vec2( 1.0,  1.0),
        vec2(-1.0,  1.0)
    );

    var uvs = array<vec2<f32>, 6>(
        vec2(0.0, 1.0),
        vec2(1.0, 1.0),
        vec2(1.0, 0.0),
        vec2(0.0, 1.0),
        vec2(1.0, 0.0),
        vec2(0.0, 0.0)
    );

    var output: VertexOutput;
    output.position = vec4(positions[vertex_index], 0.0, 1.0);
    output.uv = uvs[vertex_index];
    return output;
}

/* Layer textures - we blend up to 4 layers */
@group(0) @binding(0) var layer0: texture_2d<f32>;
@group(0) @binding(1) var layer1: texture_2d<f32>;
@group(0) @binding(2) var layer2: texture_2d<f32>;
@group(0) @binding(3) var layer3: texture_2d<f32>;
@group(0) @binding(4) var layer_sampler: sampler;

/* Layout must match render-target-texture.c (32 bytes total, 16-byte aligned). */
struct BlendUniforms {
    layer_count: u32,
    target_w: u32,           // target width in pixels
    target_h: u32,           // target height in pixels
    _pad: u32,
    visual_zoom_scale: f32,  // 1.0 = off, >1.0 = zoomed in
    visual_zoom_off_x: f32,  // pan offset in source pixels
    visual_zoom_off_y: f32,
    _pad2: f32,
};

@group(0) @binding(5) var<uniform> uniforms: BlendUniforms;

/* Alpha-over compositing (premultiplied alpha) */
fn alpha_over(dst: vec4<f32>, src: vec4<f32>) -> vec4<f32> {
    return vec4(
        dst.rgb * (1.0 - src.a) + src.rgb,
        dst.a * (1.0 - src.a) + src.a
    );
}

/* Map the quad UV through the visual-zoom transform before sampling.
 * With scale=1 and zero offsets this is the identity. */
fn zoomed_uv(uv: vec2<f32>) -> vec2<f32> {
    let scale = uniforms.visual_zoom_scale;
    if (scale <= 1.0) {
        return uv;
    }
    let size = vec2<f32>(f32(uniforms.target_w), f32(uniforms.target_h));
    if (size.x <= 0.0 || size.y <= 0.0) {
        return uv;
    }
    // Zoom around the viewport center, then apply pan.
    let centered = uv - vec2<f32>(0.5, 0.5);
    let off_uv = vec2<f32>(uniforms.visual_zoom_off_x / size.x,
                           uniforms.visual_zoom_off_y / size.y);
    return vec2<f32>(0.5, 0.5) + centered / scale + off_uv;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let uv = zoomed_uv(input.uv);

    /* Start with layer 0 (bottom layer, typically text) */
    var result = textureSample(layer0, layer_sampler, uv);

    /* Blend additional layers if present */
    if (uniforms.layer_count > 1u) {
        let layer1_color = textureSample(layer1, layer_sampler, uv);
        result = alpha_over(result, layer1_color);
    }

    if (uniforms.layer_count > 2u) {
        let layer2_color = textureSample(layer2, layer_sampler, uv);
        result = alpha_over(result, layer2_color);
    }

    if (uniforms.layer_count > 3u) {
        let layer3_color = textureSample(layer3, layer_sampler, uv);
        result = alpha_over(result, layer3_color);
    }

    return result;
}
