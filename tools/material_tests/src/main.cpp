// Tests for the parts of the material system that run without a GPU.
//
// Same shape as geometry_tests: each check prints PASS or FAIL with what it
// expected, and the exit code says whether any failed.

#include "fumar/geometry/primitives.hpp"
#include "fumar/render/mesh.hpp"
#include "fumar/render/tangents.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace fumar;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    std::printf(condition ? "  PASS  %s\n" : "  FAIL  %s\n", what.c_str());
    if (!condition) {
        ++failures;
    }
}

void section(const char* name) { std::printf("\n%s\n", name); }

bool near(f32 a, f32 b, f32 tolerance) { return std::abs(a - b) <= tolerance; }

// --- tangents ------------------------------------------------------------------

/// A unit quad on the ground, facing up, with U along +X and V along +Z.
///
/// The tangent follows U, so it is +X. The handedness w is whatever turns
/// cross(normal, tangent) into the direction V runs: cross(+Y, +X) is -Z, and V
/// runs along +Z, so w is -1. A frame with w the wrong way round lights every
/// normal-map bump from the opposite side, and looks plausible doing it.
void mikkTangentsOnQuad() {
    std::vector<Vertex> vertices{
        Vertex{.position = {0, 0, 0}, .normal = {0, 1, 0}, .uv = {0, 0}, .tangent = {}},
        Vertex{.position = {1, 0, 0}, .normal = {0, 1, 0}, .uv = {1, 0}, .tangent = {}},
        Vertex{.position = {1, 0, 1}, .normal = {0, 1, 0}, .uv = {1, 1}, .tangent = {}},
        Vertex{.position = {0, 0, 1}, .normal = {0, 1, 0}, .uv = {0, 1}, .tangent = {}},
    };
    // Counter-clockwise seen from above, so the faces face +Y like the normals.
    const std::vector<u32> indices{0, 2, 1, 0, 3, 2};

    computeTangents(vertices, indices);

    bool alongU = true;
    for (const Vertex& v : vertices) {
        alongU = alongU && near(v.tangent.x, 1.0f, 1e-4f) && near(v.tangent.y, 0.0f, 1e-4f) &&
                 near(v.tangent.z, 0.0f, 1e-4f) && v.tangent.w == -1.0f;
    }
    check(alongU, "quad tangents are (1, 0, 0) with w = -1");
}

/// MikkTSpace over a whole sphere: every tangent usable by a normal map.
void mikkTangentsOrthonormal() {
    const geometry::TriangleMesh triangles = geometry::makeUvSphere(1.0f, 16, 8).triangulate();

    std::vector<Vertex> vertices(triangles.positions.size());
    for (usize i = 0; i < vertices.size(); ++i) {
        vertices[i] = Vertex{triangles.positions[i], triangles.normals[i], triangles.uvs[i], Vec4{}};
    }
    computeTangents(vertices, triangles.indices);

    bool sound = !vertices.empty();
    for (const Vertex& v : vertices) {
        const Vec3 t{v.tangent.x, v.tangent.y, v.tangent.z};
        sound = sound && std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z) &&
                near(length(t), 1.0f, 1e-3f) && std::abs(dot(t, v.normal)) <= 1e-3f &&
                (v.tangent.w == 1.0f || v.tangent.w == -1.0f);
    }
    check(sound, "sphere tangents are unit, orthogonal to the normal, and signed");
}

} // namespace

int main() {
    section("tangents");
    mikkTangentsOnQuad();
    mikkTangentsOrthonormal();

    std::printf(failures == 0 ? "\nall good\n" : "\n%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
