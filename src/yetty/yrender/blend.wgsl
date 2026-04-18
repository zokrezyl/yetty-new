/* Blender shader - composites layer textures with alpha-over blending */

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

struct BlendUniforms {
    layer_count: u32,
    _pad0: u32,
    _pad1: u32,
    _pad2: u32,
};

@group(0) @binding(5) var<uniform> uniforms: BlendUniforms;

/* Alpha-over compositing (premultiplied alpha) */
fn alpha_over(dst: vec4<f32>, src: vec4<f32>) -> vec4<f32> {
    return vec4(
        dst.rgb * (1.0 - src.a) + src.rgb,
        dst.a * (1.0 - src.a) + src.a
    );
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    /* Start with layer 0 (bottom layer, typically text) */
    var result = textureSample(layer0, layer_sampler, input.uv);

    /* Blend additional layers if present */
    if (uniforms.layer_count > 1u) {
        let layer1_color = textureSample(layer1, layer_sampler, input.uv);
        result = alpha_over(result, layer1_color);
    }

    if (uniforms.layer_count > 2u) {
        let layer2_color = textureSample(layer2, layer_sampler, input.uv);
        result = alpha_over(result, layer2_color);
    }

    if (uniforms.layer_count > 3u) {
        let layer3_color = textureSample(layer3, layer_sampler, input.uv);
        result = alpha_over(result, layer3_color);
    }

    return result;
}
