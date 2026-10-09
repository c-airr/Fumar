#include "fumar/geometry/primitives.hpp"

#include "fumar/core/assert.hpp"

#include <algorithm>
#include <cmath>

namespace fumar::geometry {

namespace {

/// Adds a face, reversing it if it turns out to face the origin.
///
/// For a closed convex shape centred on the origin, "outward" and "away from
/// the origin" are the same thing - so instead of reasoning out the winding of
/// every ring of a sphere by hand, each face is checked and turned round if it
/// came out backwards. The cube and cylinder are written out explicitly
/// because their order is already established by the renderer's versions.
void addOutwardFace(EditableMesh& mesh, Face face) {
    if (dot(faceNormal(mesh, face), faceCentroid(mesh, face)) < 0.0f) {
        std::reverse(face.verts.begin(), face.verts.end());
        std::reverse(face.uvs.begin(), face.uvs.end());
    }
    mesh.faces.push_back(std::move(face));
}

} // namespace

EditableMesh makeCube() {
    EditableMesh mesh;

    // Corner i has its x, y and z sign in bits 0, 1 and 2.
    for (u32 i = 0; i < 8; ++i) {
        mesh.positions.push_back(Vec3{(i & 1u) ? 0.5f : -0.5f, (i & 2u) ? 0.5f : -0.5f, (i & 4u) ? 0.5f : -0.5f});
    }

    // Each face starts at its bottom-left corner seen from outside and goes
    // counter-clockwise - the same order, and the same UVs, as the renderer's
    // cube, so turning one into the other changes nothing on screen.
    const std::array<std::array<u32, 4>, 6> faces{{
        {4, 5, 7, 6}, // +Z
        {1, 0, 2, 3}, // -Z
        {5, 1, 3, 7}, // +X
        {0, 4, 6, 2}, // -X
        {6, 7, 3, 2}, // +Y
        {0, 1, 5, 4}, // -Y
    }};

    for (const auto& corners : faces) {
        mesh.faces.push_back(Face{
            .verts = {corners.begin(), corners.end()},
            .uvs = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}},
        });
    }

    return mesh;
}

EditableMesh makePlane(f32 halfSize, f32 uvTiling) {
    EditableMesh mesh;
    mesh.positions = {
        {-halfSize, 0.0f, halfSize},
        {halfSize, 0.0f, halfSize},
        {halfSize, 0.0f, -halfSize},
        {-halfSize, 0.0f, -halfSize},
    };
    mesh.faces.push_back(Face{
        .verts = {0, 1, 2, 3},
        .uvs = {{0.0f, uvTiling}, {uvTiling, uvTiling}, {uvTiling, 0.0f}, {0.0f, 0.0f}},
    });
    return mesh;
}

EditableMesh makeCylinder(f32 radius, f32 height, u32 segments) {
    FUMAR_ASSERT(segments >= 3);

    EditableMesh mesh;
    const f32 halfHeight = height * 0.5f;

    // Bottom ring at even indices, top ring at odd ones. Unlike the renderer's
    // cylinder there is no duplicated seam vertex: the seam is a UV problem,
    // and UVs live on face corners here, so the last quad simply asks for U = 1
    // at a vertex the first quad sees at U = 0.
    for (u32 i = 0; i < segments; ++i) {
        const f32 angle = static_cast<f32>(i) / static_cast<f32>(segments) * kTwoPi;
        const f32 x = std::cos(angle) * radius;
        const f32 z = std::sin(angle) * radius;
        mesh.positions.push_back(Vec3{x, -halfHeight, z});
        mesh.positions.push_back(Vec3{x, halfHeight, z});
    }

    const auto bottom = [segments](u32 i) { return (i % segments) * 2; };
    const auto top = [segments](u32 i) { return (i % segments) * 2 + 1; };

    for (u32 i = 0; i < segments; ++i) {
        const f32 u0 = static_cast<f32>(i) / static_cast<f32>(segments);
        const f32 u1 = static_cast<f32>(i + 1) / static_cast<f32>(segments);

        // Increasing angle runs clockwise seen from above, so this order -
        // bottom, top, next top, next bottom - is the one that faces out.
        mesh.faces.push_back(Face{
            .verts = {bottom(i), top(i), top(i + 1), bottom(i + 1)},
            .uvs = {{u0, 1.0f}, {u0, 0.0f}, {u1, 0.0f}, {u1, 1.0f}},
        });
    }

    // Each cap is ONE polygon. The top one walks the ring backwards, which is
    // counter-clockwise from above.
    Face topCap;
    Face bottomCap;
    for (u32 i = 0; i < segments; ++i) {
        const u32 forward = i;
        const u32 backward = segments - 1 - i;

        const auto capUv = [segments](u32 index) {
            const f32 angle = static_cast<f32>(index) / static_cast<f32>(segments) * kTwoPi;
            return Vec2{std::cos(angle) * 0.5f + 0.5f, std::sin(angle) * 0.5f + 0.5f};
        };

        topCap.verts.push_back(top(backward));
        topCap.uvs.push_back(capUv(backward));
        bottomCap.verts.push_back(bottom(forward));
        bottomCap.uvs.push_back(capUv(forward));
    }
    mesh.faces.push_back(std::move(topCap));
    mesh.faces.push_back(std::move(bottomCap));

    return mesh;
}

EditableMesh makeUvSphere(f32 radius, u32 segments, u32 rings) {
    FUMAR_ASSERT(segments >= 3 && rings >= 2);

    EditableMesh mesh;

    // Vertex 0 is the north pole, the last one the south pole, and in between
    // rings - 1 bands of `segments` vertices each, from north to south.
    mesh.positions.push_back(Vec3{0.0f, radius, 0.0f});
    for (u32 ring = 1; ring < rings; ++ring) {
        const f32 polar = static_cast<f32>(ring) / static_cast<f32>(rings) * kPi;
        for (u32 s = 0; s < segments; ++s) {
            const f32 azimuth = static_cast<f32>(s) / static_cast<f32>(segments) * kTwoPi;
            mesh.positions.push_back(Vec3{radius * std::sin(polar) * std::cos(azimuth), radius * std::cos(polar),
                                          radius * std::sin(polar) * std::sin(azimuth)});
        }
    }
    const u32 south = static_cast<u32>(mesh.positions.size());
    mesh.positions.push_back(Vec3{0.0f, -radius, 0.0f});

    const auto band = [segments](u32 ring, u32 s) { return 1 + (ring - 1) * segments + (s % segments); };
    const auto uv = [segments, rings](u32 s, u32 ring) {
        return Vec2{static_cast<f32>(s) / static_cast<f32>(segments), static_cast<f32>(ring) / static_cast<f32>(rings)};
    };

    for (u32 s = 0; s < segments; ++s) {
        // Triangles round the poles: a quad there would have two corners on
        // the same point.
        addOutwardFace(mesh, Face{
                                 .verts = {0, band(1, s), band(1, s + 1)},
                                 .uvs = {uv(s, 0), uv(s, 1), uv(s + 1, 1)},
                             });
        addOutwardFace(mesh, Face{
                                 .verts = {south, band(rings - 1, s + 1), band(rings - 1, s)},
                                 .uvs = {uv(s, rings), uv(s + 1, rings - 1), uv(s, rings - 1)},
                             });

        for (u32 ring = 1; ring + 1 < rings; ++ring) {
            addOutwardFace(mesh, Face{
                                     .verts = {band(ring, s), band(ring + 1, s), band(ring + 1, s + 1), band(ring, s + 1)},
                                     .uvs = {uv(s, ring), uv(s, ring + 1), uv(s + 1, ring + 1), uv(s + 1, ring)},
                                 });
        }
    }

    return mesh;
}

} // namespace fumar::geometry
