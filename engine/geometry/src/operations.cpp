#include "fumar/geometry/operations.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <unordered_map>

namespace fumar::geometry {

namespace {

constexpr Vec2 average(Vec2 a, Vec2 b) { return Vec2{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f}; }

constexpr Vec2 mix(Vec2 a, Vec2 b, f32 t) { return Vec2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

u32 addVertex(EditableMesh& mesh, Vec3 position) {
    mesh.positions.push_back(position);
    return static_cast<u32>(mesh.positions.size() - 1);
}

/// Splits every edge in `midpoints` wherever it still appears in a face, by
/// slotting the midpoint in between its two ends.
///
/// The step that keeps a surface closed after an operation has cut some of its
/// edges. A face beside a subdivided one shares the edge that was split; left
/// alone it would still run straight from end to end while its neighbour
/// bends at the middle, and the two would no longer meet - a T-junction, which
/// renders as a crack. Giving the neighbour the midpoint as an extra corner
/// keeps it the same shape and closes the gap.
void insertMidpoints(EditableMesh& mesh, const std::unordered_map<EdgeKey, u32, EdgeKeyHash>& midpoints) {
    if (midpoints.empty()) {
        return;
    }

    for (Face& face : mesh.faces) {
        Face split;
        for (usize i = 0; i < face.size(); ++i) {
            const usize next = (i + 1) % face.size();
            split.verts.push_back(face.verts[i]);
            split.uvs.push_back(face.uvs[i]);

            const auto it = midpoints.find(EdgeKey{face.verts[i], face.verts[next]});
            if (it != midpoints.end()) {
                split.verts.push_back(it->second);
                split.uvs.push_back(average(face.uvs[i], face.uvs[next]));
            }
        }
        face = std::move(split);
    }

    // A loose edge that was cut becomes two loose edges.
    std::vector<EdgeKey> loose;
    for (const EdgeKey key : mesh.looseEdges) {
        const auto it = midpoints.find(key);
        if (it == midpoints.end()) {
            loose.push_back(key);
        } else {
            loose.emplace_back(key.a, it->second);
            loose.emplace_back(it->second, key.b);
        }
    }
    mesh.looseEdges = std::move(loose);
}

/// Every directed edge of every face, as (from, to).
std::set<std::pair<u32, u32>> directedEdges(const EditableMesh& mesh) {
    std::set<std::pair<u32, u32>> result;
    for (const Face& face : mesh.faces) {
        for (usize i = 0; i < face.size(); ++i) {
            result.emplace(face.verts[i], face.verts[(i + 1) % face.size()]);
        }
    }
    return result;
}

/// Turns a new face round if it disagrees with the faces it touches.
///
/// Two faces that share an edge must list it in opposite directions, or one of
/// them faces inward. A new face that shares any edge with the mesh therefore
/// has exactly one correct winding, and this picks it. A face that shares no
/// edge has no neighbour to agree with, so it is turned to face away from the
/// middle of the mesh - right for anything closing a hole in a solid.
void orientAgainstNeighbours(const EditableMesh& mesh, Face& face) {
    const auto existing = directedEdges(mesh);

    i32 vote = 0;
    for (usize i = 0; i < face.size(); ++i) {
        const u32 from = face.verts[i];
        const u32 to = face.verts[(i + 1) % face.size()];
        if (existing.contains({from, to})) {
            --vote; // same direction as a neighbour: this face is backwards
        } else if (existing.contains({to, from})) {
            ++vote;
        }
    }

    bool reverse = vote < 0;
    if (vote == 0) {
        reverse = dot(faceNormal(mesh, face), faceCentroid(mesh, face) - mesh.centre()) < 0.0f;
    }

    if (reverse) {
        std::reverse(face.verts.begin(), face.verts.end());
        std::reverse(face.uvs.begin(), face.uvs.end());
    }
}

/// UVs for a face that came from nowhere, by flattening it onto its own plane
/// and fitting the result into 0..1.
void projectUvs(const EditableMesh& mesh, Face& face) {
    const Vec3 normal = faceNormal(mesh, face);
    const Vec3 helper = std::abs(normal.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 tangent = normalize(cross(helper, normal));
    const Vec3 bitangent = cross(normal, tangent);

    face.uvs.clear();
    Vec2 low{1e30f, 1e30f};
    Vec2 high{-1e30f, -1e30f};
    for (const u32 v : face.verts) {
        const Vec2 uv{dot(mesh.positions[v], tangent), dot(mesh.positions[v], bitangent)};
        low = Vec2{std::min(low.x, uv.x), std::min(low.y, uv.y)};
        high = Vec2{std::max(high.x, uv.x), std::max(high.y, uv.y)};
        face.uvs.push_back(uv);
    }

    const f32 span = std::max({high.x - low.x, high.y - low.y, 1e-6f});
    for (Vec2& uv : face.uvs) {
        uv = (uv - low) / span;
    }
}

/// The faces in a selection that actually exist, in ascending order.
std::vector<u32> validFaces(const EditableMesh& mesh, const Selection& selection) {
    std::vector<u32> result;
    for (const u32 f : selection.faces) {
        if (f < mesh.faces.size()) {
            result.push_back(f);
        }
    }
    return result;
}

/// Puts selected vertices into a closed loop by following the edges between
/// them. Empty when they do not form exactly one loop.
std::vector<u32> chainIntoLoop(const std::set<EdgeKey>& edges) {
    std::map<u32, std::vector<u32>> neighbours;
    for (const EdgeKey key : edges) {
        neighbours[key.a].push_back(key.b);
        neighbours[key.b].push_back(key.a);
    }
    if (neighbours.size() < 3) {
        return {};
    }
    for (const auto& [vertex, adjacent] : neighbours) {
        if (adjacent.size() != 2) {
            return {};
        }
    }

    std::vector<u32> loop{neighbours.begin()->first};
    u32 previous = loop.front();
    u32 current = neighbours[previous][0];
    while (current != loop.front()) {
        loop.push_back(current);
        const std::vector<u32>& adjacent = neighbours[current];
        const u32 next = adjacent[0] == previous ? adjacent[1] : adjacent[0];
        previous = current;
        current = next;
        if (loop.size() > neighbours.size()) {
            return {};
        }
    }

    // Two separate loops would each pass the degree test; only one that
    // visits every vertex is a single outline.
    return loop.size() == neighbours.size() ? loop : std::vector<u32>{};
}

} // namespace

std::set<u32> coveredVertices(const EditableMesh& mesh, const Selection& selection, SelectMode mode) {
    std::set<u32> result;
    const usize count = mesh.positions.size();

    switch (mode) {
    case SelectMode::Vertex:
        for (const u32 v : selection.vertices) {
            if (v < count) {
                result.insert(v);
            }
        }
        break;
    case SelectMode::Edge:
        for (const EdgeKey key : selection.edges) {
            if (key.a < count && key.b < count) {
                result.insert(key.a);
                result.insert(key.b);
            }
        }
        break;
    case SelectMode::Face:
        for (const u32 f : validFaces(mesh, selection)) {
            result.insert(mesh.faces[f].verts.begin(), mesh.faces[f].verts.end());
        }
        break;
    }
    return result;
}

Selection selectAll(const EditableMesh& mesh, SelectMode mode) {
    Selection selection;
    switch (mode) {
    case SelectMode::Vertex:
        for (u32 v = 0; v < mesh.positions.size(); ++v) {
            selection.vertices.insert(v);
        }
        break;
    case SelectMode::Edge:
        for (const Edge& edge : mesh.buildEdges().edges) {
            selection.edges.insert(edge.key);
        }
        break;
    case SelectMode::Face:
        for (u32 f = 0; f < mesh.faces.size(); ++f) {
            selection.faces.insert(f);
        }
        break;
    }
    return selection;
}

// --- extrude -----------------------------------------------------------------

Selection extrude(EditableMesh& mesh, const Selection& selection, SelectMode mode) {
    Selection result;

    if (mode == SelectMode::Vertex) {
        // Each vertex grows a loose edge to a copy of itself.
        for (const u32 v : selection.vertices) {
            if (v >= mesh.positions.size()) {
                continue;
            }
            const u32 copy = addVertex(mesh, mesh.positions[v]);
            mesh.looseEdges.emplace_back(v, copy);
            result.vertices.insert(copy);
        }
        return result;
    }

    if (mode == SelectMode::Edge) {
        const EdgeTable table = mesh.buildEdges();
        std::map<u32, u32> copies;
        const auto copyOf = [&](u32 v) {
            const auto [it, inserted] = copies.try_emplace(v, 0u);
            if (inserted) {
                it->second = addVertex(mesh, mesh.positions[v]);
            }
            return it->second;
        };

        std::vector<Face> added;
        std::set<EdgeKey> consumedLoose;
        for (const EdgeKey key : selection.edges) {
            const Edge* edge = table.find(key);
            if (edge == nullptr) {
                continue;
            }

            // Find which way the edge runs in the face beside it. The new quad
            // has to run the OTHER way along it, or the two faces would wind
            // against each other and one of them would face inward.
            u32 from = key.a;
            u32 to = key.b;
            if (!edge->faces.empty()) {
                const Face& face = mesh.faces[edge->faces[0]];
                for (usize i = 0; i < face.size(); ++i) {
                    if (face.verts[i] == key.a && face.verts[(i + 1) % face.size()] == key.b) {
                        from = key.b;
                        to = key.a;
                        break;
                    }
                }
            } else {
                consumedLoose.insert(key);
            }

            const u32 fromCopy = copyOf(from);
            const u32 toCopy = copyOf(to);
            added.push_back(Face{
                .verts = {from, to, toCopy, fromCopy},
                .uvs = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}},
            });
            result.edges.insert(EdgeKey{fromCopy, toCopy});
        }

        std::erase_if(mesh.looseEdges, [&](EdgeKey key) { return consumedLoose.contains(key); });
        mesh.faces.insert(mesh.faces.end(), added.begin(), added.end());
        return result;
    }

    // --- faces, as a region -------------------------------------------------
    const std::vector<u32> region = validFaces(mesh, selection);
    if (region.empty()) {
        return result;
    }
    const std::set<u32> inRegion(region.begin(), region.end());

    // How many of the region's own faces use each edge. One means the edge is
    // on the outline of the region, where a wall has to be built; two means it
    // is inside, between two faces that move together.
    std::unordered_map<EdgeKey, u32, EdgeKeyHash> regionUses;
    for (const u32 f : region) {
        const Face& face = mesh.faces[f];
        for (usize i = 0; i < face.size(); ++i) {
            ++regionUses[EdgeKey{face.verts[i], face.verts[(i + 1) % face.size()]}];
        }
    }

    // A vertex needs a copy if anything that stays behind still uses it, or if
    // it is on the outline, where the wall needs one end on the old vertex and
    // one on the new. A vertex used by nothing but the region - the middle of
    // an extruded patch - simply moves with it.
    std::vector<bool> needsCopy(mesh.positions.size(), false);
    for (u32 f = 0; f < mesh.faces.size(); ++f) {
        if (!inRegion.contains(f)) {
            for (const u32 v : mesh.faces[f].verts) {
                needsCopy[v] = true;
            }
        }
    }
    for (const EdgeKey key : mesh.looseEdges) {
        needsCopy[key.a] = true;
        needsCopy[key.b] = true;
    }
    for (const auto& [key, uses] : regionUses) {
        if (uses == 1) {
            needsCopy[key.a] = true;
            needsCopy[key.b] = true;
        }
    }

    std::map<u32, u32> copies;
    for (const u32 f : region) {
        for (const u32 v : mesh.faces[f].verts) {
            if (needsCopy[v] && !copies.contains(v)) {
                copies[v] = addVertex(mesh, mesh.positions[v]);
            }
        }
    }
    const auto moved = [&copies](u32 v) {
        const auto it = copies.find(v);
        return it == copies.end() ? v : it->second;
    };

    // The walls, one per outline edge. Collected before the region's faces are
    // rewritten, because the outline is described in terms of the old corners.
    //
    // An outline edge runs a -> b in its region face. The wall is a, b, b', a':
    // along the edge the same way, then up to the copies. Its normal comes out
    // as edge x (copy - original), which points away from the region - outward.
    std::vector<Face> walls;
    for (const u32 f : region) {
        const Face& face = mesh.faces[f];
        for (usize i = 0; i < face.size(); ++i) {
            const u32 a = face.verts[i];
            const u32 b = face.verts[(i + 1) % face.size()];
            if (regionUses[EdgeKey{a, b}] == 1) {
                walls.push_back(Face{
                    .verts = {a, b, moved(b), moved(a)},
                    .uvs = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}},
                });
            }
        }
    }

    for (const u32 f : region) {
        for (u32& v : mesh.faces[f].verts) {
            v = moved(v);
        }
        result.faces.insert(f);
    }

    mesh.faces.insert(mesh.faces.end(), walls.begin(), walls.end());
    return result;
}

// --- inset -------------------------------------------------------------------

Selection inset(EditableMesh& mesh, const Selection& selection, f32 thickness) {
    Selection result;

    for (const u32 f : validFaces(mesh, selection)) {
        const Face original = mesh.faces[f];
        const usize n = original.size();
        const Vec3 normal = faceNormal(mesh, original);
        if (lengthSquared(normal) == 0.0f) {
            continue;
        }

        const Vec3 centroid = faceCentroid(mesh, original);
        Vec2 centroidUv{0.0f, 0.0f};
        for (const Vec2 uv : original.uvs) {
            centroidUv += uv;
        }
        centroidUv = centroidUv / static_cast<f32>(n);

        Face inner;
        for (usize i = 0; i < n; ++i) {
            const Vec3 previous = mesh.positions[original.verts[(i + n - 1) % n]];
            const Vec3 corner = mesh.positions[original.verts[i]];
            const Vec3 next = mesh.positions[original.verts[(i + 1) % n]];

            // For a counter-clockwise face, normal x edge points to the left of
            // the edge - into the face. Each corner moves along the average of
            // its two edges' inward directions, and further for a sharper
            // corner: dividing by the cosine of half the angle between them is
            // what puts both edges exactly `thickness` in.
            const Vec3 inwardA = cross(normal, normalize(corner - previous));
            const Vec3 inwardB = cross(normal, normalize(next - corner));
            Vec3 bisector = inwardA + inwardB;
            bisector = lengthSquared(bisector) > 1e-12f ? normalize(bisector) : inwardA;

            const f32 cosHalf = std::max(dot(bisector, inwardA), 0.2f);
            const f32 reach = length(centroid - corner);

            // Never past the middle: an inset thicker than the face is wide
            // would turn the inner face inside out.
            const f32 distance = std::min(thickness / cosHalf, reach * 0.95f);

            inner.verts.push_back(addVertex(mesh, corner + bisector * distance));
            inner.uvs.push_back(mix(original.uvs[i], centroidUv, reach > 1e-6f ? distance / reach : 0.0f));
        }

        // The ring, one quad per original edge: old edge, then back along the
        // new one. Same direction as the face along the outside, so it agrees
        // with whatever neighbour shares that edge.
        for (usize i = 0; i < n; ++i) {
            const usize next = (i + 1) % n;
            mesh.faces.push_back(Face{
                .verts = {original.verts[i], original.verts[next], inner.verts[next], inner.verts[i]},
                .uvs = {original.uvs[i], original.uvs[next], inner.uvs[next], inner.uvs[i]},
            });
        }

        mesh.faces[f] = std::move(inner);
        result.faces.insert(f);
    }

    return result;
}

// --- loop cut ----------------------------------------------------------------

Selection loopCut(EditableMesh& mesh, EdgeKey start) {
    Selection result;
    const EdgeTable table = mesh.buildEdges();
    const Edge* startEdge = table.find(start);
    if (startEdge == nullptr) {
        return result;
    }

    // The quads the loop passes through, each with the edge it entered by.
    // Keyed by face so a closed loop that comes back round stops instead of
    // cutting its first quad twice.
    std::map<u32, EdgeKey> crossedFaces;
    std::set<EdgeKey> crossedEdges{start};

    const auto opposite = [&mesh](u32 f, EdgeKey key) {
        const Face& face = mesh.faces[f];
        for (usize i = 0; i < 4; ++i) {
            if (EdgeKey{face.verts[i], face.verts[(i + 1) % 4]} == key) {
                return EdgeKey{face.verts[(i + 2) % 4], face.verts[(i + 3) % 4]};
            }
        }
        return key;
    };

    const auto walk = [&](u32 face, EdgeKey entry) {
        while (true) {
            if (mesh.faces[face].size() != 4 || crossedFaces.contains(face)) {
                return;
            }
            crossedFaces[face] = entry;

            const EdgeKey exit = opposite(face, entry);
            crossedEdges.insert(exit);

            const Edge* exitEdge = table.find(exit);
            if (exitEdge == nullptr || exitEdge->faces.size() != 2) {
                return; // an open border, or a fin the loop cannot continue across
            }
            face = exitEdge->faces[0] == face ? exitEdge->faces[1] : exitEdge->faces[0];
            entry = exit;
        }
    };

    for (const u32 face : startEdge->faces) {
        walk(face, start);
    }

    if (crossedFaces.empty()) {
        return result;
    }

    std::unordered_map<EdgeKey, u32, EdgeKeyHash> midpoints;
    for (const EdgeKey key : crossedEdges) {
        midpoints[key] = addVertex(mesh, (mesh.positions[key.a] + mesh.positions[key.b]) * 0.5f);
    }

    for (const auto& [f, entry] : crossedFaces) {
        const Face quad = mesh.faces[f];

        // Rotate so the entry edge is corners 0 -> 1 and the exit is 2 -> 3.
        usize offset = 0;
        while (EdgeKey{quad.verts[offset], quad.verts[(offset + 1) % 4]} != entry) {
            ++offset;
        }
        const auto corner = [&](usize i) { return quad.verts[(offset + i) % 4]; };
        const auto uv = [&](usize i) { return quad.uvs[(offset + i) % 4]; };

        const u32 entryMid = midpoints[EdgeKey{corner(0), corner(1)}];
        const u32 exitMid = midpoints[EdgeKey{corner(2), corner(3)}];
        const Vec2 entryUv = average(uv(0), uv(1));
        const Vec2 exitUv = average(uv(2), uv(3));

        // Both halves keep the quad's direction round, so they agree with each
        // other and with every neighbour.
        mesh.faces[f] = Face{
            .verts = {corner(0), entryMid, exitMid, corner(3)},
            .uvs = {uv(0), entryUv, exitUv, uv(3)},
        };
        mesh.faces.push_back(Face{
            .verts = {entryMid, corner(1), corner(2), exitMid},
            .uvs = {entryUv, uv(1), uv(2), exitUv},
        });
        result.edges.insert(EdgeKey{entryMid, exitMid});
    }

    // The quads just cut no longer contain any crossed edge. What still does is
    // whatever the loop stopped at - a triangle, an n-gon - and that needs the
    // midpoint too, or it leaves a crack.
    insertMidpoints(mesh, midpoints);
    return result;
}

// --- delete ------------------------------------------------------------------

void deleteSelection(EditableMesh& mesh, const Selection& selection, SelectMode mode) {
    std::vector<bool> removeFace(mesh.faces.size(), false);

    const auto touchesSelectedVertex = [&](EdgeKey key) {
        return selection.vertices.contains(key.a) || selection.vertices.contains(key.b);
    };

    for (u32 f = 0; f < mesh.faces.size(); ++f) {
        const Face& face = mesh.faces[f];
        switch (mode) {
        case SelectMode::Face:
            removeFace[f] = selection.faces.contains(f);
            break;
        case SelectMode::Edge:
            for (usize i = 0; i < face.size() && !removeFace[f]; ++i) {
                removeFace[f] = selection.edges.contains(EdgeKey{face.verts[i], face.verts[(i + 1) % face.size()]});
            }
            break;
        case SelectMode::Vertex:
            for (const u32 v : face.verts) {
                removeFace[f] = removeFace[f] || selection.vertices.contains(v);
            }
            break;
        }
    }

    // Edges that belonged to a removed face, as candidates for staying behind
    // as loose edges.
    std::set<EdgeKey> orphanedEdges;
    std::vector<Face> kept;
    for (u32 f = 0; f < mesh.faces.size(); ++f) {
        const Face& face = mesh.faces[f];
        if (removeFace[f]) {
            for (usize i = 0; i < face.size(); ++i) {
                orphanedEdges.insert(EdgeKey{face.verts[i], face.verts[(i + 1) % face.size()]});
            }
        } else {
            kept.push_back(face);
        }
    }
    mesh.faces = std::move(kept);

    std::erase_if(mesh.looseEdges, [&](EdgeKey key) {
        return (mode == SelectMode::Edge && selection.edges.contains(key)) ||
               (mode == SelectMode::Vertex && touchesSelectedVertex(key));
    });

    // Deleting faces takes their edges with them. Deleting an edge or a vertex
    // takes only that, plus the faces that cannot exist without it; their
    // other edges stay, as loose edges, unless something still uses them.
    if (mode != SelectMode::Face) {
        std::set<EdgeKey> stillUsed(mesh.looseEdges.begin(), mesh.looseEdges.end());
        for (const Face& face : mesh.faces) {
            for (usize i = 0; i < face.size(); ++i) {
                stillUsed.insert(EdgeKey{face.verts[i], face.verts[(i + 1) % face.size()]});
            }
        }
        for (const EdgeKey key : orphanedEdges) {
            const bool deleted = (mode == SelectMode::Edge && selection.edges.contains(key)) ||
                                 (mode == SelectMode::Vertex && touchesSelectedVertex(key));
            if (!deleted && !stillUsed.contains(key)) {
                mesh.looseEdges.push_back(key);
            }
        }
    }

    // Whatever is left with nothing to belong to - including the selected
    // vertices themselves, which by now nothing refers to.
    mesh.removeUnusedVertices();
}

// --- merge -------------------------------------------------------------------

Selection mergeAtCentre(EditableMesh& mesh, const Selection& selection, SelectMode mode) {
    Selection result;
    const std::set<u32> merging = coveredVertices(mesh, selection, mode);
    if (merging.size() < 2) {
        result.vertices = merging;
        return result;
    }

    Vec3 centre{0.0f, 0.0f, 0.0f};
    for (const u32 v : merging) {
        centre += mesh.positions[v];
    }
    const u32 survivor = *merging.begin();
    mesh.positions[survivor] = centre / static_cast<f32>(merging.size());

    const auto target = [&](u32 v) { return merging.contains(v) ? survivor : v; };

    std::vector<Face> kept;
    std::set<std::vector<u32>> seen;
    for (const Face& face : mesh.faces) {
        Face collapsed;
        for (usize i = 0; i < face.size(); ++i) {
            const u32 v = target(face.verts[i]);
            // Two corners that are now the same vertex in a row are one corner.
            if (collapsed.verts.empty() || collapsed.verts.back() != v) {
                collapsed.verts.push_back(v);
                collapsed.uvs.push_back(face.uvs[i]);
            }
        }
        if (collapsed.verts.size() > 1 && collapsed.verts.front() == collapsed.verts.back()) {
            collapsed.verts.pop_back();
            collapsed.uvs.pop_back();
        }

        // Fewer than three corners is not a face. A corner that still appears
        // twice, not next to itself, means opposite corners were merged and the
        // face folded into a bow tie - neither half of that is worth keeping.
        std::vector<u32> sorted = collapsed.verts;
        std::sort(sorted.begin(), sorted.end());
        const bool repeats = std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end();
        if (collapsed.verts.size() < 3 || repeats) {
            continue;
        }

        // Two faces collapsed onto the same corners - the two sides of a strip
        // that was merged flat - would be the same face twice.
        if (!seen.insert(sorted).second) {
            continue;
        }
        kept.push_back(std::move(collapsed));
    }
    mesh.faces = std::move(kept);

    std::set<EdgeKey> faceEdges;
    for (const Face& face : mesh.faces) {
        for (usize i = 0; i < face.size(); ++i) {
            faceEdges.insert(EdgeKey{face.verts[i], face.verts[(i + 1) % face.size()]});
        }
    }
    std::set<EdgeKey> loose;
    for (const EdgeKey key : mesh.looseEdges) {
        const EdgeKey merged{target(key.a), target(key.b)};
        if (merged.a != merged.b && !faceEdges.contains(merged)) {
            loose.insert(merged);
        }
    }
    mesh.looseEdges.assign(loose.begin(), loose.end());

    const std::vector<u32> remap = mesh.removeUnusedVertices();
    if (remap[survivor] != EditableMesh::kRemoved) {
        result.vertices.insert(remap[survivor]);
    }
    return result;
}

// --- fill --------------------------------------------------------------------

Selection fill(EditableMesh& mesh, const Selection& selection, SelectMode mode) {
    Selection result;
    std::vector<u32> outline;

    if (mode == SelectMode::Edge) {
        std::set<EdgeKey> edges;
        for (const EdgeKey key : selection.edges) {
            if (key.a < mesh.positions.size() && key.b < mesh.positions.size()) {
                edges.insert(key);
            }
        }

        if (edges.size() == 2) {
            const EdgeKey first = *edges.begin();
            const EdgeKey second = *std::next(edges.begin());
            const std::set<u32> corners{first.a, first.b, second.a, second.b};
            if (corners.size() == 4) {
                // Of the two ways to join the ends, the one whose new sides are
                // shorter is the one that does not cross itself.
                const auto& p = mesh.positions;
                const f32 straight = length(p[first.b] - p[second.a]) + length(p[second.b] - p[first.a]);
                const f32 crossed = length(p[first.b] - p[second.b]) + length(p[second.a] - p[first.a]);
                outline = straight <= crossed ? std::vector<u32>{first.a, first.b, second.a, second.b}
                                              : std::vector<u32>{first.a, first.b, second.b, second.a};
            }
        }
        if (outline.empty()) {
            outline = chainIntoLoop(edges);
        }
    } else if (mode == SelectMode::Vertex) {
        std::vector<u32> chosen;
        for (const u32 v : selection.vertices) {
            if (v < mesh.positions.size()) {
                chosen.push_back(v);
            }
        }
        if (chosen.size() < 3) {
            return result;
        }

        // If the mesh already has edges running round the chosen vertices, that
        // order is the outline the modeller meant.
        std::set<EdgeKey> between;
        for (const Edge& edge : mesh.buildEdges().edges) {
            if (selection.vertices.contains(edge.key.a) && selection.vertices.contains(edge.key.b)) {
                between.insert(edge.key);
            }
        }
        outline = chainIntoLoop(between);
        if (outline.size() != chosen.size()) {
            // Otherwise: sort them by angle around their middle, in the plane
            // they lie in. That plane's normal is the largest cross product of
            // any two of them seen from the middle - the most reliable pair.
            Vec3 middle{0.0f, 0.0f, 0.0f};
            for (const u32 v : chosen) {
                middle += mesh.positions[v];
            }
            middle = middle / static_cast<f32>(chosen.size());

            Vec3 normal{0.0f, 0.0f, 0.0f};
            for (usize i = 0; i < chosen.size(); ++i) {
                for (usize j = i + 1; j < chosen.size(); ++j) {
                    const Vec3 candidate =
                        cross(mesh.positions[chosen[i]] - middle, mesh.positions[chosen[j]] - middle);
                    if (lengthSquared(candidate) > lengthSquared(normal)) {
                        normal = candidate;
                    }
                }
            }
            if (lengthSquared(normal) < 1e-12f) {
                return result; // all in a line
            }
            normal = normalize(normal);
            const Vec3 axisX = normalize(mesh.positions[chosen[0]] - middle);
            const Vec3 axisY = cross(normal, axisX);

            std::sort(chosen.begin(), chosen.end(), [&](u32 l, u32 r) {
                const Vec3 dl = mesh.positions[l] - middle;
                const Vec3 dr = mesh.positions[r] - middle;
                return std::atan2(dot(dl, axisY), dot(dl, axisX)) < std::atan2(dot(dr, axisY), dot(dr, axisX));
            });
            outline = std::move(chosen);
        }
    }

    if (outline.size() < 3) {
        return result;
    }

    Face face;
    face.verts = std::move(outline);
    if (lengthSquared(faceNormal(mesh, face)) == 0.0f) {
        return result;
    }

    // The same corners as a face that already exists is not a new face.
    std::vector<u32> sorted = face.verts;
    std::sort(sorted.begin(), sorted.end());
    for (const Face& existing : mesh.faces) {
        std::vector<u32> other = existing.verts;
        std::sort(other.begin(), other.end());
        if (other == sorted) {
            return result;
        }
    }

    orientAgainstNeighbours(mesh, face);
    projectUvs(mesh, face);

    std::set<EdgeKey> covered;
    for (usize i = 0; i < face.size(); ++i) {
        covered.insert(EdgeKey{face.verts[i], face.verts[(i + 1) % face.size()]});
    }
    std::erase_if(mesh.looseEdges, [&](EdgeKey key) { return covered.contains(key); });

    mesh.faces.push_back(std::move(face));
    result.faces.insert(static_cast<u32>(mesh.faces.size() - 1));
    return result;
}

// --- subdivide ---------------------------------------------------------------

Selection subdivide(EditableMesh& mesh, const Selection& selection) {
    Selection result;
    const std::vector<u32> chosen = validFaces(mesh, selection);
    if (chosen.empty()) {
        return result;
    }

    // One midpoint per edge, shared by both faces that meet there, so the two
    // halves of a split edge are the same vertex on either side.
    std::unordered_map<EdgeKey, u32, EdgeKeyHash> midpoints;
    for (const u32 f : chosen) {
        const Face& face = mesh.faces[f];
        for (usize i = 0; i < face.size(); ++i) {
            const EdgeKey key{face.verts[i], face.verts[(i + 1) % face.size()]};
            if (!midpoints.contains(key)) {
                midpoints[key] = addVertex(mesh, (mesh.positions[key.a] + mesh.positions[key.b]) * 0.5f);
            }
        }
    }

    for (const u32 f : chosen) {
        const Face original = mesh.faces[f];
        const usize n = original.size();

        Vec2 centreUv{0.0f, 0.0f};
        for (const Vec2 uv : original.uvs) {
            centreUv += uv;
        }
        centreUv = centreUv / static_cast<f32>(n);
        const u32 centre = addVertex(mesh, faceCentroid(mesh, original));

        // One quad per corner: the corner, the midpoint after it, the centre,
        // the midpoint before it. Counter-clockwise like the face it replaces.
        for (usize i = 0; i < n; ++i) {
            const usize next = (i + 1) % n;
            const usize previous = (i + n - 1) % n;
            Face quad{
                .verts = {original.verts[i], midpoints[EdgeKey{original.verts[i], original.verts[next]}], centre,
                          midpoints[EdgeKey{original.verts[previous], original.verts[i]}]},
                .uvs = {original.uvs[i], average(original.uvs[i], original.uvs[next]), centreUv,
                        average(original.uvs[previous], original.uvs[i])},
            };

            if (i == 0) {
                mesh.faces[f] = std::move(quad);
                result.faces.insert(f);
            } else {
                mesh.faces.push_back(std::move(quad));
                result.faces.insert(static_cast<u32>(mesh.faces.size() - 1));
            }
        }
    }

    // Neighbours that were not subdivided still run straight across the split
    // edges. They get the midpoints as extra corners.
    insertMidpoints(mesh, midpoints);
    return result;
}

void flipNormals(EditableMesh& mesh, const Selection& selection) {
    for (const u32 f : validFaces(mesh, selection)) {
        std::reverse(mesh.faces[f].verts.begin(), mesh.faces[f].verts.end());
        std::reverse(mesh.faces[f].uvs.begin(), mesh.faces[f].uvs.end());
    }
}

} // namespace fumar::geometry
