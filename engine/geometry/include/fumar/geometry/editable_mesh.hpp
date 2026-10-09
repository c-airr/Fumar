#pragma once

#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"

#include <array>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fumar::geometry {

/// One polygon: the corners, in order, and a texture coordinate for each.
///
/// Corners go counter-clockwise seen from the side the face faces, the glTF
/// convention the renderer already follows. A face can have any number of
/// corners from three up - a cube's side is one face with four, not two
/// triangles - because that is the unit a modeller thinks in. Extrude a face,
/// inset a face, cut a loop through faces: none of those mean anything to a
/// triangle soup.
///
/// UVs live per CORNER rather than per vertex, because one vertex at the corner
/// of a cube belongs to three faces that each want a different coordinate there.
struct Face {
    std::vector<u32> verts;
    std::vector<Vec2> uvs;

    usize size() const { return verts.size(); }
};

/// An edge named by its two vertices, smaller index first.
///
/// Edges are identified by their endpoints rather than by an index into a list
/// because every topology change rebuilds that list, and an index would point
/// at a different edge afterwards. The pair survives as long as the vertices
/// do - which is exactly what a selection needs.
struct EdgeKey {
    u32 a = 0;
    u32 b = 0;

    EdgeKey() = default;
    EdgeKey(u32 first, u32 second) : a(first < second ? first : second), b(first < second ? second : first) {}

    friend bool operator==(const EdgeKey&, const EdgeKey&) = default;
    friend auto operator<=>(const EdgeKey&, const EdgeKey&) = default;
};

struct EdgeKeyHash {
    usize operator()(const EdgeKey& key) const {
        return std::hash<u64>{}((static_cast<u64>(key.a) << 32) | key.b);
    }
};

/// An edge and the faces on either side of it.
struct Edge {
    EdgeKey key;

    /// Usually one or two. One is an open border, two is the interior of a
    /// surface, more than two is a non-manifold fin that most operations
    /// refuse to touch. Zero is a loose edge with no face at all.
    std::vector<u32> faces;
};

/// Every edge of a mesh, derived from its faces and loose edges.
///
/// Derived rather than stored, and rebuilt after each change. That is the
/// central simplification of this module: a half-edge structure (Blender's
/// BMesh, OpenMesh) keeps adjacency up to date incrementally and answers "what
/// is next to this" in constant time, at the cost of every operation having to
/// maintain a web of pointers correctly. Rebuilding the table is linear in the
/// size of the mesh - microseconds for the few thousand faces anyone models by
/// hand - and leaves every operation free to just rewrite the face list.
///
/// When meshes get large enough for that to show up, this is the class to
/// replace, and nothing outside this module sees the difference.
struct EdgeTable {
    std::vector<Edge> edges;
    std::unordered_map<EdgeKey, u32, EdgeKeyHash> lookup;

    /// Index into `edges`, or nullptr if no such edge exists.
    const Edge* find(EdgeKey key) const;
};

/// What the GPU needs: triangles with one normal per corner.
///
/// Kept separate from the renderer's own vertex struct so this module never
/// learns what Vulkan is. The renderer copies these into its Vertex layout.
struct TriangleMesh {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    std::vector<u32> indices;

    /// How many corners each source face had, in order. A face of n corners
    /// became n - 2 consecutive triangles fanned from its first corner, so this
    /// list is enough to put the faces back together - which is how a mesh
    /// saved to glTF comes back as quads instead of triangles.
    std::vector<u32> polygonSizes;
};

/// A polygon mesh that can be edited.
///
/// Plain data on purpose: positions, faces, and the edges that belong to no
/// face. Every operation in operations.hpp takes one of these by reference and
/// rewrites it.
struct EditableMesh {
    std::vector<Vec3> positions;
    std::vector<Face> faces;

    /// Edges with no face on either side. What extruding a single vertex
    /// produces, and what is left of a face whose other edges were deleted.
    /// Without them a mesh could only ever be a surface.
    std::vector<EdgeKey> looseEdges;

    /// Faces meeting at less than this angle are shaded as one smooth surface;
    /// sharper than this and the edge between them stays crisp. Zero shades
    /// every face flat.
    ///
    /// One number instead of a smooth/flat switch because real objects need
    /// both at once: a cylinder wants its side smooth and its rim sharp, and a
    /// sphere and a cube want opposite answers from the same mesh code. Thirty
    /// degrees is what Blender settled on for "smooth by angle" - every face of
    /// a 24-segment sphere is within 15 degrees of its neighbour, every corner
    /// of a box is 90.
    f32 smoothAngleDegrees = 30.0f;

    usize vertexCount() const { return positions.size(); }
    usize faceCount() const { return faces.size(); }

    EdgeTable buildEdges() const;

    /// Corners per face, fanned into triangles, with a normal per corner.
    ///
    /// Each corner's normal averages the faces around its vertex that lie
    /// within smoothAngleDegrees of its own face - see there. Every corner is
    /// still a vertex of its own, so a crisp edge and a smooth one cost the
    /// same and can sit side by side on one mesh.
    TriangleMesh triangulate() const;

    /// Drops vertices nothing refers to and renumbers the rest.
    ///
    /// Every operation that removes faces leaves vertices behind; this is the
    /// single place that tidies them up, so an operation can delete freely and
    /// call this once at the end.
    ///
    /// Returns where each old vertex went, or kRemoved for those that are gone
    /// - so an operation can still find the vertex it meant to return.
    std::vector<u32> removeUnusedVertices();

    static constexpr u32 kRemoved = 0xFFFFFFFFu;

    /// Centre of the bounding box of every vertex. Zero for an empty mesh.
    Vec3 centre() const;
};

/// The face's normal, by Newell's method.
///
/// Sums a cross product per edge rather than taking one cross product of two
/// edges, so a face that is slightly bent, or whose first three corners happen
/// to be nearly in line, still gets the normal it visibly has. Zero for a
/// degenerate face.
Vec3 faceNormal(const EditableMesh& mesh, const Face& face);

/// Average of the face's corners.
Vec3 faceCentroid(const EditableMesh& mesh, const Face& face);

/// Rebuilds a polygon mesh from triangles, joining what can be joined.
///
/// Vertices at the same position are welded into one - a triangle list
/// duplicates a vertex for every face it touches, and an editable mesh whose
/// faces do not share corners would fall apart the first time one was moved.
///
/// With `polygonSizes` (what triangulate() wrote, or what a glTF file saved by
/// fumar carries) the original faces come back exactly. Without it - a file
/// from anywhere else - pairs of triangles that share their longest edge and
/// lie in one plane are joined into quads, which turns the usual exporter
/// output for a box back into six faces instead of twelve.
EditableMesh fromTriangles(std::span<const Vec3> positions, std::span<const Vec2> uvs,
                           std::span<const u32> indices, std::span<const u32> polygonSizes = {});

/// Everything wrong with a mesh, one line per problem; empty when it is sound.
///
/// Checks what the operations are supposed to guarantee: indices in range,
/// at least three distinct corners per face, a UV per corner, no edge shared by
/// more than two faces, neighbouring faces wound the same way, no vertex that
/// nothing uses. The tests run this after every operation, because a broken
/// topology does not crash - it renders, mostly, and breaks the NEXT operation.
std::vector<std::string> validate(const EditableMesh& mesh);

} // namespace fumar::geometry
