#pragma once

#include "fumar/geometry/editable_mesh.hpp"

#include <set>

namespace fumar::geometry {

/// What a click picks: a corner, an edge, or a whole face.
enum class SelectMode : u8 {
    Vertex,
    Edge,
    Face,
};

/// The elements an operation acts on.
///
/// Only the set matching the mode in use is meaningful; the others are left
/// empty. Ordered sets rather than vectors so that "is this selected" is a
/// lookup and the result of an operation does not depend on the order things
/// were clicked in.
struct Selection {
    std::set<u32> vertices;
    std::set<EdgeKey> edges;
    std::set<u32> faces;

    bool empty() const { return vertices.empty() && edges.empty() && faces.empty(); }
    void clear() {
        vertices.clear();
        edges.clear();
        faces.clear();
    }
};

/// Every vertex the selection touches, whichever mode it was made in.
///
/// This is what a transform moves: dragging a face moves its corners, dragging
/// an edge moves its two ends.
std::set<u32> coveredVertices(const EditableMesh& mesh, const Selection& selection, SelectMode mode);

/// Selects everything in the given mode.
Selection selectAll(const EditableMesh& mesh, SelectMode mode);

/// Pulls new geometry out of the selection, still in place.
///
/// Faces are extruded as a REGION: where two selected faces meet, no wall is
/// built between them, so extruding the top of a subdivided box raises one
/// block rather than a cluster of separate columns. Edges grow a quad each;
/// vertices grow a loose edge each.
///
/// Nothing moves - the new geometry sits exactly on top of the old, selected,
/// so that the very next drag of the gizmo is what gives it depth. That is how
/// every modeller does it, and it is why the result is returned rather than
/// applied with a distance.
Selection extrude(EditableMesh& mesh, const Selection& selection, SelectMode mode);

/// Shrinks each selected face inside itself and rings it with quads.
///
/// `thickness` is the distance from each old edge to the new one, measured in
/// the plane of the face. Corners are pushed out along their bisector by just
/// enough that every edge ends up exactly that far in, so a sharp corner gets a
/// longer push than a square one. Returns the inner faces.
Selection inset(EditableMesh& mesh, const Selection& selection, f32 thickness);

/// Splits a ring of quads down the middle, starting from one edge.
///
/// From the starting edge, walks across each quad to its opposite edge and
/// into the next quad, in both directions, until it reaches a border, a face
/// that is not a quad, or comes back round to where it started. Every edge
/// crossed is split at its midpoint and every quad crossed is cut in two.
/// Returns the new edges, ready to be slid.
///
/// Empty when the edge has no quad beside it - there is no loop to cut.
Selection loopCut(EditableMesh& mesh, EdgeKey start);

/// Removes the selection and whatever can no longer exist without it.
///
/// Deleting faces keeps their edges where other faces still use them. Deleting
/// an edge or a vertex takes every face that used it, and the face's remaining
/// edges stay behind as loose edges - so deleting one corner of a lone quad
/// leaves the two edges opposite it, which is what Blender does.
void deleteSelection(EditableMesh& mesh, const Selection& selection, SelectMode mode);

/// Collapses every selected vertex into one, at their average position.
///
/// Faces that lose a corner shrink; faces left with fewer than three corners
/// disappear. Returns the surviving vertex.
Selection mergeAtCentre(EditableMesh& mesh, const Selection& selection, SelectMode mode);

/// Builds a face from the selection.
///
/// Two edges become a quad between them; a closed chain of edges, or three or
/// more vertices, becomes one polygon. The winding is chosen to agree with any
/// face the new one shares an edge with, and otherwise to face away from the
/// middle of the mesh. Returns the new face, or an empty selection when the
/// selection does not describe one.
Selection fill(EditableMesh& mesh, const Selection& selection, SelectMode mode);

/// Cuts each selected face into quads through the midpoints of its edges.
///
/// A quad becomes four, a triangle three, an n-gon n. Neighbouring faces that
/// share a split edge get the midpoint as an extra corner, so the surface stays
/// closed rather than growing T-junctions. Returns the new faces.
Selection subdivide(EditableMesh& mesh, const Selection& selection);

/// Reverses the winding of each selected face, which turns it inside out.
void flipNormals(EditableMesh& mesh, const Selection& selection);

} // namespace fumar::geometry
