#pragma once

#include <filesystem>
#include <string>

namespace fumar {

class Mesh;

/// Writes one mesh to a binary glTF file (.glb): positions, normals, the first
/// UV set and 32-bit indices, no material.
///
/// glTF because it is the one format every tool reads - the file opens in
/// Blender as readily as in fumar - and binary because the text variant keeps
/// its vertex data in a second file or as base64, both worse for something
/// that is only ever read by a program.
///
/// The faces the triangles were cut from go into the primitive's
/// extras.fumarPolygons, which glTF allows any application to add and every
/// other one ignores. That is what brings a mesh back into the Modeler as
/// quads and n-gons rather than as triangles.
///
/// Returns false, having logged why, if the mesh is empty or the file cannot
/// be written.
bool writeGlb(const std::filesystem::path& path, const Mesh& mesh, const std::string& name);

} // namespace fumar
