#version 450

// A floor grid on the plane y = 0, for the Modeler.
//
// Drawn as one fullscreen triangle after the geometry, not as a mesh of lines.
// Each pixel works out where its view ray meets the floor, and draws a line if
// that point lies close to a whole number of metres. That makes the grid
// infinite for free, and lets the lines stay one pixel wide at any distance:
// "close" is measured in screen-space derivatives, so a line a hundred metres
// away is as thin as one at your feet instead of shrinking to nothing or
// turning into moire.
//
// It writes the depth of the floor point it found, and is tested against the
// depth buffer, so the object being modelled hides the grid where it stands in
// front of it - which is the entire difference between a grid you can read
// depth from and a pattern painted over the screen.

#include "frame.glsl"

layout(location = 0) in vec2 vUV;

layout(location = 0) out vec4 outColor;

/// How strongly a pixel is on a line of the given spacing: 1 on the line,
/// falling to 0 one pixel away.
float lineStrength(vec2 coordinate, float spacing) {
    const vec2 scaled = coordinate / spacing;
    const vec2 width = fwidth(scaled);
    const vec2 distanceToLine = abs(fract(scaled - 0.5) - 0.5) / max(width, vec2(1e-6));
    return 1.0 - min(min(distanceToLine.x, distanceToLine.y), 1.0);
}

void main() {
    // The view ray through this pixel, as the segment between the near and far
    // planes in world space.
    const vec2 ndc = vUV * 2.0 - 1.0;
    vec4 nearPoint = frame.invViewProjection * vec4(ndc, 0.0, 1.0);
    vec4 farPoint = frame.invViewProjection * vec4(ndc, 1.0, 1.0);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;

    const vec3 ray = farPoint.xyz - nearPoint.xyz;
    if (abs(ray.y) < 1e-6) {
        discard; // looking exactly along the floor
    }

    // Where along that segment it crosses y = 0. Outside 0..1 means behind the
    // camera or past the far plane - looking up at the sky, in other words.
    const float t = -nearPoint.y / ray.y;
    if (t <= 0.0 || t >= 1.0) {
        discard;
    }
    const vec3 floorPoint = nearPoint.xyz + ray * t;

    const vec4 clip = frame.projection * frame.view * vec4(floorPoint, 1.0);
    gl_FragDepth = clip.z / clip.w;

    // Metres, and every tenth line stronger, so scale can be read at a glance.
    const float minor = lineStrength(floorPoint.xz, 1.0);
    const float major = lineStrength(floorPoint.xz, 10.0);

    // The two axes through the origin in the colours every 3D tool uses: red
    // along X, and blue along Z, which is the depth axis in a Y-up world.
    const float alongX = 1.0 - min(abs(floorPoint.z) / max(fwidth(floorPoint.z), 1e-6), 1.0);
    const float alongZ = 1.0 - min(abs(floorPoint.x) / max(fwidth(floorPoint.x), 1e-6), 1.0);

    vec3 colour = vec3(0.02);
    float alpha = max(minor * 0.28, major * 0.5);
    if (alongX > 0.0) {
        colour = mix(colour, vec3(0.55, 0.06, 0.05), alongX);
        alpha = max(alpha, alongX * 0.9);
    }
    if (alongZ > 0.0) {
        colour = mix(colour, vec3(0.05, 0.12, 0.6), alongZ);
        alpha = max(alpha, alongZ * 0.9);
    }

    // Fades out with distance, where the lines crowd together into a grey
    // smear anyway, and so the grid does not end at a hard edge.
    const float distance = length(floorPoint.xz - frame.cameraPosition.xz);
    alpha *= 1.0 - smoothstep(15.0, 60.0, distance);

    if (alpha <= 0.002) {
        discard;
    }
    outColor = vec4(colour, alpha);
}
