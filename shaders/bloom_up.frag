#version 450

// One step back up the bloom chain: read a small level, add it into the level
// above at twice the size.
//
// The addition is done by the blend state rather than in the shader, because
// the destination is also the source of the previous step - a shader cannot
// read the image it is writing. The pipeline is built with additive blending
// and the attachment loaded rather than cleared, so what the downsample pass
// left there survives and this is added on top.
//
// Summing every level is what gives the glow its shape: the small levels reach
// far and contribute a wide, faint halo, the large ones stay tight and bright.
// A single blur at one radius cannot do both.

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant) uniform PushConstants {
    /// One texel of the SOURCE, in UV.
    vec2 texelSize;

    /// How far the tent reaches, in source texels. Above 1 the levels overlap
    /// more, which trades a little definition for a smoother falloff.
    float radius;
} push;

layout(location = 0) in vec2 vUV;

layout(location = 0) out vec4 outColor;

void main() {
    const vec2 r = push.texelSize * push.radius;

    // A 3x3 tent - 1,2,1 / 2,4,2 / 1,2,1 over sixteen. Cheap, and the right
    // shape for this: the chain is already smooth, so what the upsample has to
    // avoid is reintroducing the square edges of the smaller level, not blur
    // further.
    vec3 sum = texture(source, vUV + vec2(-r.x, r.y)).rgb;
    sum += texture(source, vUV + vec2(0.0, r.y)).rgb * 2.0;
    sum += texture(source, vUV + vec2(r.x, r.y)).rgb;

    sum += texture(source, vUV + vec2(-r.x, 0.0)).rgb * 2.0;
    sum += texture(source, vUV).rgb * 4.0;
    sum += texture(source, vUV + vec2(r.x, 0.0)).rgb * 2.0;

    sum += texture(source, vUV + vec2(-r.x, -r.y)).rgb;
    sum += texture(source, vUV + vec2(0.0, -r.y)).rgb * 2.0;
    sum += texture(source, vUV + vec2(r.x, -r.y)).rgb;

    outColor = vec4(sum / 16.0, 1.0);
}
