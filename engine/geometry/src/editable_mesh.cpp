#include "fumar/geometry/editable_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>

namespace fumar::geometry {

const Edge* EdgeTable::find(EdgeKey key) const {
    const auto it = lookup.find(key);
    return it == lookup.end() ? nullptr : &edges[it->second];
}

EdgeTable EditableMesh::buildEdges() const {
    EdgeTable table;

    const auto touch = [&table](EdgeKey key) -> Edge& {
        const auto [it, inserted] = table.lookup.try_emplace(key, static_cast<u32>(table.edges.size()));
        if (inserted) {
            table.edges.push_back(Edge{.key = key, .faces = {}});
        }
        return table.edges[it->second];
    };

    for (u32 f = 0; f < faces.size(); ++f) {
        const Face& face = faces[f];
        for (usize i = 0; i < face.size(); ++i) {
            touch(EdgeKey{face.verts[i], face.verts[(i + 1) % face.size()]}).faces.push_back(f);
        }
    }

    // After the faces, so a loose edge that happens to coincide with a face
    // edge is not listed twice - it simply is that edge.
    for (const EdgeKey key : looseEdges) {
        touch(key);
    }

    return table;
}

Vec3 faceNormal(const EditableMesh& mesh, const Face& face) {
    Vec3 normal{0.0f, 0.0f, 0.0f};
    for (usize i = 0; i < face.size(); ++i) {
        const Vec3 current = mesh.positions[face.verts[i]];
        const Vec3 next = mesh.positions[face.verts[(i + 1) % face.size()]];
        normal.x += (current.y - next.y) * (current.z + next.z);
        normal.y += (current.z - next.z) * (current.x + next.x);
        normal.z += (current.x - next.x) * (current.y + next.y);
    }

    const f32 len = length(normal);
    return len > 1e-12f ? normal / len : Vec3{0.0f, 0.0f, 0.0f};
}

Vec3 faceCentroid(const EditableMesh& mesh, const Face& face) {
    Vec3 sum{0.0f, 0.0f, 0.0f};
    for (const u32 v : face.verts) {
        sum += mesh.positions[v];
    }
    return face.size() > 0 ? sum / static_cast<f32>(face.size()) : sum;
}

TriangleMesh EditableMesh::triangulate() const {
    TriangleMesh out;

    // A face whose corners all lie in a line has no direction it faces. Up is
    // as good a guess as any and keeps NaNs out of the lighting.
    std::vector<Vec3> normals(faces.size());
    for (usize f = 0; f < faces.size(); ++f) {
        normals[f] = faceNormal(*this, faces[f]);
        if (lengthSquared(normals[f]) == 0.0f) {
            normals[f] = Vec3{0.0f, 1.0f, 0.0f};
        }
    }

    // Which faces touch each vertex, so a corner can look at its neighbours:
    // vertex v's faces are facesAt[firstFace[v] .. firstFace[v + 1]). One flat
    // array rather than a vector per vertex - this runs on every frame of a
    // drag, and a vector per vertex is an allocation per vertex per frame.
    const bool smooth = smoothAngleDegrees > 0.0f;
    std::vector<u32> firstFace;
    std::vector<u32> facesAt;
    if (smooth) {
        firstFace.assign(positions.size() + 1, 0);
        for (const Face& face : faces) {
            for (const u32 v : face.verts) {
                ++firstFace[v + 1];
            }
        }
        for (usize v = 0; v < positions.size(); ++v) {
            firstFace[v + 1] += firstFace[v];
        }
        facesAt.resize(firstFace.back());
        std::vector<u32> cursor(firstFace.begin(), firstFace.end() - 1);
        for (u32 f = 0; f < faces.size(); ++f) {
            for (const u32 v : faces[f].verts) {
                facesAt[cursor[v]++] = f;
            }
        }
    }
    const f32 smoothCos = std::cos(radians(smoothAngleDegrees));

    // Which way U runs across each face, from the first triangle of its fan,
    // and which way round V runs from there (the handedness). Averaged per
    // corner below the same way the normals are, so a smooth surface gets a
    // smooth tangent too. Not MikkTSpace: this runs on every frame of a drag,
    // and the meshes made here are textured with tiling maps, not with maps
    // baked against one exact tangent frame.
    //
    // xyz is the direction, w the handedness: +1 or -1, or 0 for a face whose
    // UVs give no direction at all. Packed into one Vec4 so the walk over a
    // corner's neighbours below reads sixteen bytes a face, not twenty.
    std::vector<Vec4> faceTangents(faces.size(), Vec4{0.0f, 0.0f, 0.0f, 0.0f});
    for (usize f = 0; f < faces.size(); ++f) {
        const Face& face = faces[f];
        if (face.size() < 3 || face.uvs.size() < 3) {
            continue;
        }
        const Vec3 e1 = positions[face.verts[1]] - positions[face.verts[0]];
        const Vec3 e2 = positions[face.verts[2]] - positions[face.verts[0]];
        const Vec2 d1 = face.uvs[1] - face.uvs[0];
        const Vec2 d2 = face.uvs[2] - face.uvs[0];
        const f32 det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-12f) {
            // No UV area: nothing says which way U runs.
            continue;
        }
        const Vec3 tangent = (e1 * d2.y - e2 * d1.y) / det;
        const Vec3 bitangent = (e2 * d1.x - e1 * d2.x) / det;
        if (lengthSquared(tangent) < 1e-24f) {
            continue;
        }
        // The handedness that makes cross(n, t) * w point UP the image, where
        // a normal map's green points. `bitangent` runs towards larger V, and V
        // runs DOWN the image, as in glTF - so up is the opposite of it.
        const Vec3 direction = normalize(tangent);
        faceTangents[f] = Vec4{direction.x, direction.y, direction.z,
                               dot(cross(normals[f], tangent), bitangent) < 0.0f ? 1.0f : -1.0f};
    }

    usize corners = 0;
    usize triangles = 0;
    for (const Face& face : faces) {
        if (face.size() >= 3) {
            corners += face.size();
            triangles += face.size() - 2;
        }
    }
    out.positions.reserve(corners);
    out.normals.reserve(corners);
    out.uvs.reserve(corners);
    out.tangents.reserve(corners);
    out.indices.reserve(triangles * 3);
    out.polygonSizes.reserve(faces.size());

    for (usize f = 0; f < faces.size(); ++f) {
        const Face& face = faces[f];
        if (face.size() < 3) {
            continue;
        }

        // Every corner gets its own vertex, so each one can carry its own
        // normal - which is what lets a sharp edge stay sharp next to a smooth
        // one. Sharing vertices between faces would force one normal per
        // position and round over every corner the modeller made crisp.
        const u32 base = static_cast<u32>(out.positions.size());
        for (usize i = 0; i < face.size(); ++i) {
            // One walk over the faces round this corner gathers both the normal
            // and the tangent. The tangent takes the same neighbours as the
            // normal, but only those whose UVs run the same way round: across a
            // mirrored seam the two sides' tangents point apart, and their sum
            // is nothing useful.
            const Vec4 own = faceTangents[f];
            Vec3 normal = normals[f];
            Vec3 tangent{own.x, own.y, own.z};
            if (smooth) {
                Vec3 normalSum{0.0f, 0.0f, 0.0f};
                Vec3 tangentSum{0.0f, 0.0f, 0.0f};
                const u32 v = face.verts[i];
                for (u32 k = firstFace[v]; k < firstFace[v + 1]; ++k) {
                    const u32 neighbour = facesAt[k];
                    if (dot(normals[neighbour], normals[f]) < smoothCos) {
                        continue;
                    }
                    normalSum += normals[neighbour];
                    const Vec4 other = faceTangents[neighbour];
                    // Equal handedness, and so also both usable: an unusable
                    // face's w is 0, and this corner's is checked below.
                    if (other.w == own.w) {
                        tangentSum += Vec3{other.x, other.y, other.z};
                    }
                }
                if (lengthSquared(normalSum) > 1e-12f) {
                    normal = normalize(normalSum);
                }
                if (own.w != 0.0f) {
                    tangent = tangentSum;
                }
            }

            // Square to the corner's own normal (Gram-Schmidt): a tangent that
            // leans out of the surface tilts every normal-map texel with it.
            tangent -= normal * dot(normal, tangent);
            Vec4 corner{0.0f, 0.0f, 0.0f, own.w};
            if (own.w != 0.0f && lengthSquared(tangent) > 1e-12f) {
                const Vec3 t = normalize(tangent);
                corner = Vec4{t.x, t.y, t.z, own.w};
            } else {
                // Any direction in the surface beats a NaN in the lighting.
                const Vec3 t = normalize(
                    cross(normal, std::abs(normal.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f}));
                corner = Vec4{t.x, t.y, t.z, 1.0f};
            }

            out.positions.push_back(positions[face.verts[i]]);
            out.normals.push_back(normal);
            out.tangents.push_back(corner);
            out.uvs.push_back(i < face.uvs.size() ? face.uvs[i] : Vec2{0.0f, 0.0f});
        }

        // A fan from the first corner. Correct for every convex polygon, which
        // is everything the operations here produce; a concave face drawn by
        // hand can fold over itself, and the fix for that is ear clipping.
        for (u32 i = 1; i + 1 < face.size(); ++i) {
            out.indices.insert(out.indices.end(), {base, base + i, base + i + 1});
        }

        out.polygonSizes.push_back(static_cast<u32>(face.size()));
    }

    return out;
}

std::vector<u32> EditableMesh::removeUnusedVertices() {
    std::vector<bool> used(positions.size(), false);
    for (const Face& face : faces) {
        for (const u32 v : face.verts) {
            used[v] = true;
        }
    }
    for (const EdgeKey key : looseEdges) {
        used[key.a] = true;
        used[key.b] = true;
    }

    std::vector<u32> remap(positions.size(), kRemoved);
    std::vector<Vec3> kept;
    kept.reserve(positions.size());
    for (u32 v = 0; v < positions.size(); ++v) {
        if (used[v]) {
            remap[v] = static_cast<u32>(kept.size());
            kept.push_back(positions[v]);
        }
    }

    if (kept.size() == positions.size()) {
        return remap;
    }

    positions = std::move(kept);
    for (Face& face : faces) {
        for (u32& v : face.verts) {
            v = remap[v];
        }
    }
    for (EdgeKey& key : looseEdges) {
        key = EdgeKey{remap[key.a], remap[key.b]};
    }
    return remap;
}

Vec3 EditableMesh::centre() const {
    if (positions.empty()) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }
    Vec3 low = positions[0];
    Vec3 high = positions[0];
    for (const Vec3 p : positions) {
        low = min(low, p);
        high = max(high, p);
    }
    return (low + high) * 0.5f;
}

namespace {

/// A position snapped to a grid fine enough to be invisible and coarse enough
/// to absorb the rounding an exporter introduces. Two vertices that land in the
/// same cell are the same vertex.
struct WeldKey {
    i64 x;
    i64 y;
    i64 z;

    friend auto operator<=>(const WeldKey&, const WeldKey&) = default;
};

WeldKey weldKey(Vec3 p) {
    constexpr f64 kCell = 1e5; // a hundredth of a millimetre at one unit per metre
    return WeldKey{std::llround(static_cast<f64>(p.x) * kCell), std::llround(static_cast<f64>(p.y) * kCell),
                   std::llround(static_cast<f64>(p.z) * kCell)};
}

Vec3 triangleNormal(Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 n = cross(b - a, c - a);
    const f32 len = length(n);
    return len > 1e-12f ? n / len : Vec3{0.0f, 0.0f, 0.0f};
}

} // namespace

EditableMesh fromTriangles(std::span<const Vec3> positions, std::span<const Vec2> uvs,
                           std::span<const u32> indices, std::span<const u32> polygonSizes) {
    EditableMesh mesh;

    // --- weld ---------------------------------------------------------------
    std::map<WeldKey, u32> welded;
    std::vector<u32> remap(positions.size());
    for (u32 i = 0; i < positions.size(); ++i) {
        const auto [it, inserted] = welded.try_emplace(weldKey(positions[i]), static_cast<u32>(mesh.positions.size()));
        if (inserted) {
            mesh.positions.push_back(positions[i]);
        }
        remap[i] = it->second;
    }

    const auto uvOf = [&uvs](u32 index) { return index < uvs.size() ? uvs[index] : Vec2{0.0f, 0.0f}; };

    // Indices come from files, and a file can say anything. One past the end
    // of the positions would read outside remap below.
    const auto inRange = [&positions](u32 index) { return index < positions.size(); };
    const bool indicesInRange = std::all_of(indices.begin(), indices.end(), inRange);

    // --- the exact way back -------------------------------------------------
    // Summed in 64 bits: a corrupt size near 2^32 would wrap a 32-bit product
    // round to a small number that happens to match the index count, and the
    // loop below would then walk far past the end of the indices.
    usize expectedIndices = 0;
    for (const u32 size : polygonSizes) {
        expectedIndices += size >= 3 ? (static_cast<usize>(size) - 2) * 3 : 0;
    }

    if (!polygonSizes.empty() && indicesInRange && expectedIndices == indices.size()) {
        usize cursor = 0;
        for (const u32 size : polygonSizes) {
            if (size < 3) {
                continue;
            }
            // Fanned from the first corner, so the first triangle holds the
            // first three corners and each one after it adds its last corner.
            Face face;
            const u32 first = indices[cursor];
            face.verts = {remap[first], remap[indices[cursor + 1]], remap[indices[cursor + 2]]};
            face.uvs = {uvOf(first), uvOf(indices[cursor + 1]), uvOf(indices[cursor + 2])};
            for (usize t = 1; t < size - 2; ++t) {
                const u32 corner = indices[cursor + t * 3 + 2];
                face.verts.push_back(remap[corner]);
                face.uvs.push_back(uvOf(corner));
            }
            cursor += (static_cast<usize>(size) - 2) * 3;
            mesh.faces.push_back(std::move(face));
        }
        return mesh;
    }

    // --- triangles, then pairs joined into quads ----------------------------
    struct Triangle {
        std::array<u32, 3> verts;
        std::array<Vec2, 3> uvs;
        Vec3 normal;
        bool joined = false;
    };

    std::vector<Triangle> triangles;
    for (usize i = 0; i + 2 < indices.size(); i += 3) {
        const std::array<u32, 3> source{indices[i], indices[i + 1], indices[i + 2]};
        if (!indicesInRange && !(inRange(source[0]) && inRange(source[1]) && inRange(source[2]))) {
            continue;
        }
        const std::array<u32, 3> verts{remap[source[0]], remap[source[1]], remap[source[2]]};

        // Two corners welded together: the triangle has no area and would only
        // ever be a sliver nobody can select.
        if (verts[0] == verts[1] || verts[1] == verts[2] || verts[0] == verts[2]) {
            continue;
        }
        triangles.push_back(Triangle{
            .verts = verts,
            .uvs = {uvOf(source[0]), uvOf(source[1]), uvOf(source[2])},
            .normal = triangleNormal(mesh.positions[verts[0]], mesh.positions[verts[1]], mesh.positions[verts[2]]),
        });
    }

    // Which triangles share each edge. Only edges with exactly two are
    // candidates for joining.
    std::unordered_map<EdgeKey, std::vector<u32>, EdgeKeyHash> edgeTriangles;
    for (u32 t = 0; t < triangles.size(); ++t) {
        for (u32 i = 0; i < 3; ++i) {
            edgeTriangles[EdgeKey{triangles[t].verts[i], triangles[t].verts[(i + 1) % 3]}].push_back(t);
        }
    }

    // Longest shared edge first. An exporter splitting a quad cuts along a
    // diagonal, and a diagonal is longer than any side - so the longest edges
    // are the ones most likely to be cuts that should be undone.
    std::vector<std::pair<f32, EdgeKey>> candidates;
    for (const auto& [key, owners] : edgeTriangles) {
        if (owners.size() == 2) {
            candidates.emplace_back(lengthSquared(mesh.positions[key.a] - mesh.positions[key.b]), key);
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& l, const auto& r) { return l.first > r.first || (l.first == r.first && l.second < r.second); });

    for (const auto& [ignored, key] : candidates) {
        const std::vector<u32>& owners = edgeTriangles[key];
        Triangle& first = triangles[owners[0]];
        Triangle& second = triangles[owners[1]];
        if (first.joined || second.joined || dot(first.normal, second.normal) < 0.9999f) {
            continue;
        }

        // Rotate the first triangle so the shared edge is its last edge
        // (corner 2 -> corner 0), and find the second triangle's corner that is
        // not on the edge. The quad is then the first's three corners plus
        // that one, slotted in between corner 2 and corner 0.
        u32 rotate = 0;
        while (EdgeKey{first.verts[(rotate + 2) % 3], first.verts[rotate]} != key) {
            ++rotate;
        }
        u32 apex = 0;
        while (second.verts[apex] == key.a || second.verts[apex] == key.b) {
            ++apex;
        }

        Face face;
        for (u32 i = 0; i < 3; ++i) {
            face.verts.push_back(first.verts[(rotate + i) % 3]);
            face.uvs.push_back(first.uvs[(rotate + i) % 3]);
        }
        face.verts.push_back(second.verts[apex]);
        face.uvs.push_back(second.uvs[apex]);

        // Joining two triangles that meet at a reflex angle makes a concave
        // quad, which then triangulates wrongly. Every corner has to turn the
        // same way as the face.
        bool convex = true;
        const Vec3 normal = first.normal;
        for (usize i = 0; i < 4 && convex; ++i) {
            const Vec3 p0 = mesh.positions[face.verts[i]];
            const Vec3 p1 = mesh.positions[face.verts[(i + 1) % 4]];
            const Vec3 p2 = mesh.positions[face.verts[(i + 2) % 4]];
            convex = dot(cross(p1 - p0, p2 - p1), normal) > 1e-9f;
        }
        if (!convex) {
            continue;
        }

        first.joined = true;
        second.joined = true;
        mesh.faces.push_back(std::move(face));
    }

    for (const Triangle& triangle : triangles) {
        if (!triangle.joined) {
            mesh.faces.push_back(Face{
                .verts = {triangle.verts[0], triangle.verts[1], triangle.verts[2]},
                .uvs = {triangle.uvs[0], triangle.uvs[1], triangle.uvs[2]},
            });
        }
    }

    mesh.removeUnusedVertices();
    return mesh;
}

std::vector<std::string> validate(const EditableMesh& mesh) {
    std::vector<std::string> problems;
    const usize vertexCount = mesh.positions.size();
    std::vector<bool> used(vertexCount, false);

    for (usize f = 0; f < mesh.faces.size(); ++f) {
        const Face& face = mesh.faces[f];
        if (face.size() < 3) {
            problems.push_back(std::format("face {} has {} corners", f, face.size()));
        }
        if (face.uvs.size() != face.verts.size()) {
            problems.push_back(std::format("face {} has {} corners but {} uvs", f, face.size(), face.uvs.size()));
        }
        for (usize i = 0; i < face.size(); ++i) {
            const u32 v = face.verts[i];
            if (v >= vertexCount) {
                problems.push_back(std::format("face {} refers to vertex {} of {}", f, v, vertexCount));
                continue;
            }
            used[v] = true;
            for (usize j = i + 1; j < face.size(); ++j) {
                if (face.verts[j] == v) {
                    problems.push_back(std::format("face {} uses vertex {} twice", f, v));
                }
            }
        }
    }

    for (const EdgeKey key : mesh.looseEdges) {
        if (key.a >= vertexCount || key.b >= vertexCount || key.a == key.b) {
            problems.push_back(std::format("loose edge {}-{} is invalid", key.a, key.b));
            continue;
        }
        used[key.a] = true;
        used[key.b] = true;
    }

    if (!problems.empty()) {
        // Everything below indexes by vertex, and would only crash on what was
        // already reported.
        return problems;
    }

    // Each directed edge (from -> to, in the order a face lists it) may appear
    // once. Twice means two neighbouring faces are wound the same way, which
    // is a face pointing inward - it shades black and breaks every operation
    // that walks from one face to the next.
    std::map<std::pair<u32, u32>, usize> directed;
    for (usize f = 0; f < mesh.faces.size(); ++f) {
        const Face& face = mesh.faces[f];
        for (usize i = 0; i < face.size(); ++i) {
            const auto key = std::make_pair(face.verts[i], face.verts[(i + 1) % face.size()]);
            const auto [it, inserted] = directed.try_emplace(key, f);
            if (!inserted) {
                problems.push_back(std::format("faces {} and {} are wound the same way across edge {}-{}", it->second,
                                               f, key.first, key.second));
            }
        }
    }

    for (const Edge& edge : mesh.buildEdges().edges) {
        if (edge.faces.size() > 2) {
            problems.push_back(std::format("edge {}-{} is shared by {} faces", edge.key.a, edge.key.b, edge.faces.size()));
        }
    }

    for (usize v = 0; v < vertexCount; ++v) {
        if (!used[v]) {
            problems.push_back(std::format("vertex {} is not used by anything", v));
        }
    }

    return problems;
}

} // namespace fumar::geometry
