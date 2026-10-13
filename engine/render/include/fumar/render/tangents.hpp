#pragma once

#include "fumar/core/types.hpp"
#include "fumar/render/mesh.hpp"

#include <span>

namespace fumar {

/// Gives every vertex an index references the MikkTSpace tangent frame.
///
/// For imported meshes that come without tangents of their own. MikkTSpace is
/// what Blender and most bakers use, and a normal map only reads back right
/// against the frame it was baked against - so this is the frame a map made
/// for the mesh elsewhere expects.
///
/// Positions, normals and UVs must already be filled in; `indices` is a
/// triangle list. Writes `tangent` (w = handedness, +1 or -1) and nothing else.
void computeTangents(std::span<Vertex> vertices, std::span<const u32> indices);

} // namespace fumar
