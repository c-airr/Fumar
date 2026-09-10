// Everything that is constant for one frame: the camera, and the environment
// that lights it.
//
// Shared by every shader that needs any of it, because a uniform block has to
// be declared IDENTICALLY in each stage and each pipeline that binds the same
// set. Two copies that drift apart is not a compile error - the shader simply
// reads the wrong bytes, which shows up as a light coming from nowhere.
//
// glslc resolves a quoted #include relative to the including file, and the
// build passes -MD so touching this file recompiles everything that includes
// it (see cmake/FumarShaders.cmake).

#ifndef FUMAR_FRAME_GLSL
#define FUMAR_FRAME_GLSL

// Set 0 is bound once per frame. Keeping it separate from the per-material set
// means switching material does not disturb it.
layout(set = 0, binding = 0) uniform FrameData {
    mat4 view;
    mat4 projection;

    /// inverse(projection * view). Turns a point in clip space back into a
    /// world-space direction, which is how the sky finds out where a pixel is
    /// looking without any geometry to interpolate from.
    mat4 invViewProjection;

    vec4 cameraPosition;

    /// Direction TOWARDS the sun, normalised. Pointing at the light rather
    /// than along it is the convention that makes dot(normal, sun) the
    /// brightness directly, with no sign to remember.
    vec4 sunDirection;

    vec4 sunColor;
    vec4 skyZenithColor;
    vec4 skyHorizonColor;
    vec4 groundColor;

    float sunIntensity;

    /// Half-angle of the sun's disc in radians. The real sun is about 0.0046
    /// (a quarter of a degree); larger values give a bigger, softer sun, and
    /// once shadows are ray traced this is also what makes their edges soft.
    float sunAngularRadius;

    float skyIntensity;

    /// Multiplies the whole image before tone mapping - the camera's shutter,
    /// not a property of the scene.
    float exposure;

    /// How far ray traced shadows are taken. 0 disables them entirely, 1 is
    /// physically what the rays report. Anything in between lifts the shadows,
    /// which is a lie, but a useful one while placing objects.
    float shadowStrength;

    float occlusionStrength;

    /// How far the occlusion rays reach, in world units. Short: this is about
    /// contact and creases, not distant geometry.
    float occlusionRadius;

    /// Padding, so the block ends on a 16-byte boundary the way std140 expects.
    /// Named rather than left implicit because the C++ struct has to carry it
    /// too, and an unnamed gap is a gap somebody eventually fills wrongly.
    float _padding;
} frame;

/// World-space direction the given normalised device coordinate looks along.
///
/// The far plane is z = 1 in Vulkan (unlike OpenGL, where the range is -1..1),
/// so unprojecting a point at z = 1 gives somewhere along the view ray, and
/// subtracting the camera gives its direction.
vec3 worldRayDirection(vec2 ndc) {
    vec4 farPoint = frame.invViewProjection * vec4(ndc, 1.0, 1.0);
    return normalize(farPoint.xyz / farPoint.w - frame.cameraPosition.xyz);
}

#endif // FUMAR_FRAME_GLSL
