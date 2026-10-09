#pragma once

#include "fumar/geometry/editable_mesh.hpp"

namespace fumar::geometry {

// The shapes the editor can add, built as polygons from the start.
//
// The renderer has its own versions of the first three that produce triangles
// directly. These describe the SAME shapes - same size, same placement - so
// that opening an existing cube for editing changes nothing about how it looks,
// only that it now has six faces to work with instead of twelve triangles.

/// A cube one unit on a side, centred on the origin. Eight vertices, six quads.
EditableMesh makeCube();

/// A single quad of the given half extent, lying in the XZ plane, facing +Y.
EditableMesh makePlane(f32 halfSize, f32 uvTiling = 1.0f);

/// A cylinder centred on the origin with its axis along Y: one quad per segment
/// around the side, and each cap a single polygon rather than a fan of
/// triangles - so the cap can be inset or extruded as one face.
EditableMesh makeCylinder(f32 radius, f32 height, u32 segments = 32);

/// A UV sphere: quads everywhere except the rings touching the poles, which
/// are triangles meeting at a single vertex.
EditableMesh makeUvSphere(f32 radius, u32 segments = 24, u32 rings = 12);

} // namespace fumar::geometry
