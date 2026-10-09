#include "fumar/geometry/raycast.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace fumar::geometry {

namespace {

constexpr u32 kLeafSize = 4;

Vec3 centroid(Vec3 a, Vec3 b, Vec3 c) { return (a + b + c) / 3.0f; }

/// Möller-Trumbore. The distance along the ray, or a negative number for a
/// miss. Either side of the triangle counts: the Modeler picks a face from
/// behind as readily as from the front, and a back face hides what is behind
/// it as well as a front one does.
f32 intersect(Vec3 origin, Vec3 direction, Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 edgeA = b - a;
    const Vec3 edgeB = c - a;
    const Vec3 p = cross(direction, edgeB);
    const f32 determinant = dot(edgeA, p);
    if (std::abs(determinant) < 1e-12f) {
        return -1.0f; // parallel to the triangle
    }

    const f32 inverse = 1.0f / determinant;
    const Vec3 s = origin - a;
    const f32 u = dot(s, p) * inverse;
    if (u < 0.0f || u > 1.0f) {
        return -1.0f;
    }

    const Vec3 q = cross(s, edgeA);
    const f32 v = dot(direction, q) * inverse;
    if (v < 0.0f || u + v > 1.0f) {
        return -1.0f;
    }

    return dot(edgeB, q) * inverse;
}

/// Whether the ray passes through the box before `limit`. Slabs: the ray
/// enters and leaves each pair of parallel planes, and is inside the box only
/// where all three intervals overlap.
bool hitsBox(Vec3 low, Vec3 high, Vec3 origin, Vec3 inverseDirection, f32 limit) {
    f32 tNear = 0.0f;
    f32 tFar = limit;
    for (usize axis = 0; axis < 3; ++axis) {
        f32 t0 = (low[axis] - origin[axis]) * inverseDirection[axis];
        f32 t1 = (high[axis] - origin[axis]) * inverseDirection[axis];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tNear = std::max(tNear, t0);
        tFar = std::min(tFar, t1);
        if (tNear > tFar) {
            return false;
        }
    }
    return true;
}

} // namespace

RayCaster::RayCaster(const EditableMesh& mesh) {
    for (u32 f = 0; f < mesh.faces.size(); ++f) {
        const Face& face = mesh.faces[f];
        for (usize i = 1; i + 1 < face.size(); ++i) {
            m_triangles.push_back(Triangle{
                .a = mesh.positions[face.verts[0]],
                .b = mesh.positions[face.verts[i]],
                .c = mesh.positions[face.verts[i + 1]],
                .face = f,
            });
        }
    }

    if (m_triangles.empty()) {
        return;
    }

    // A binary tree with leaves of up to four has fewer than n / 2 interior
    // nodes and as many leaves; reserving keeps the build from reallocating.
    m_nodes.reserve(m_triangles.size());
    m_nodes.push_back(Node{});
    build(0, 0, static_cast<u32>(m_triangles.size()));
}

void RayCaster::build(u32 index, u32 first, u32 count) {
    Vec3 low = m_triangles[first].a;
    Vec3 high = low;
    Vec3 centreLow{1e30f, 1e30f, 1e30f};
    Vec3 centreHigh{-1e30f, -1e30f, -1e30f};
    for (u32 i = first; i < first + count; ++i) {
        const Triangle& t = m_triangles[i];
        low = min(low, min(t.a, min(t.b, t.c)));
        high = max(high, max(t.a, max(t.b, t.c)));
        const Vec3 c = centroid(t.a, t.b, t.c);
        centreLow = min(centreLow, c);
        centreHigh = max(centreHigh, c);
    }

    // Indices, not references: the push_back below can move the whole array.
    m_nodes[index].low = low;
    m_nodes[index].high = high;
    m_nodes[index].first = first;
    m_nodes[index].count = count;

    const Vec3 extent = centreHigh - centreLow;
    usize axis = 0;
    if (extent.y > extent.x) {
        axis = 1;
    }
    if (extent.z > extent[axis]) {
        axis = 2;
    }

    // Few enough to test directly, or all piled on one point and impossible to
    // separate by position - either way, a leaf.
    if (count <= kLeafSize || extent[axis] <= 0.0f) {
        return;
    }

    // The median, not the midpoint of the box: it keeps both halves the same
    // size however unevenly the triangles are spread, so the tree stays
    // balanced and no ray walks a long thin branch.
    const u32 middle = first + count / 2;
    std::nth_element(m_triangles.begin() + first, m_triangles.begin() + middle, m_triangles.begin() + first + count,
                     [axis](const Triangle& l, const Triangle& r) {
                         return centroid(l.a, l.b, l.c)[axis] < centroid(r.a, r.b, r.c)[axis];
                     });

    const u32 left = static_cast<u32>(m_nodes.size());
    m_nodes.push_back(Node{});
    m_nodes.push_back(Node{});
    m_nodes[index].first = left;
    m_nodes[index].count = 0;

    build(left, first, middle - first);
    build(left + 1, middle, first + count - middle);
}

template <bool AnyHit>
std::optional<RayCaster::Hit> RayCaster::trace(Vec3 origin, Vec3 direction, f32 tMax) const {
    if (m_nodes.empty()) {
        return std::nullopt;
    }

    // Division by zero is allowed to produce infinity here, and the slab test
    // handles it correctly - except that 0 * infinity is NaN, which is why a
    // zero component is replaced by a huge finite number instead.
    Vec3 inverseDirection;
    for (usize axis = 0; axis < 3; ++axis) {
        inverseDirection[axis] =
            std::abs(direction[axis]) > 1e-20f ? 1.0f / direction[axis] : std::copysign(1e30f, direction[axis]);
    }

    std::optional<Hit> best;
    f32 limit = tMax;

    // A fixed stack: a median split halves the triangles at every level, so
    // the depth is the logarithm of the count - sixty-four levels would hold
    // more triangles than there is memory for.
    std::array<u32, 64> stack{};
    usize depth = 0;
    stack[depth++] = 0;

    while (depth > 0) {
        const Node& node = m_nodes[stack[--depth]];
        if (!hitsBox(node.low, node.high, origin, inverseDirection, limit)) {
            continue;
        }

        if (node.count == 0) {
            stack[depth++] = node.first;
            stack[depth++] = node.first + 1;
            continue;
        }

        for (u32 i = node.first; i < node.first + node.count; ++i) {
            const Triangle& triangle = m_triangles[i];
            const f32 t = intersect(origin, direction, triangle.a, triangle.b, triangle.c);
            if (t > 1e-6f && t < limit) {
                best = Hit{.t = t, .face = triangle.face};
                if constexpr (AnyHit) {
                    return best;
                }
                limit = t;
            }
        }
    }

    return best;
}

std::optional<RayCaster::Hit> RayCaster::closest(Vec3 origin, Vec3 direction, f32 tMax) const {
    return trace<false>(origin, direction, tMax);
}

bool RayCaster::occluded(Vec3 origin, Vec3 direction, f32 tMax) const {
    return trace<true>(origin, direction, tMax).has_value();
}

} // namespace fumar::geometry
