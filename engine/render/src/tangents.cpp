#include "fumar/render/tangents.hpp"

#include <mikktspace.h>

namespace fumar {
namespace {

/// What the MikkTSpace callbacks are handed: the mesh, as a triangle list.
struct MikkMesh {
    std::span<Vertex> vertices;
    std::span<const u32> indices;
};

MikkMesh& meshOf(const SMikkTSpaceContext* context) {
    return *static_cast<MikkMesh*>(context->m_pUserData);
}

const Vertex& corner(const SMikkTSpaceContext* context, int face, int vert) {
    const MikkMesh& mesh = meshOf(context);
    return mesh.vertices[mesh.indices[static_cast<usize>(face) * 3 + static_cast<usize>(vert)]];
}

int faceCount(const SMikkTSpaceContext* context) {
    return static_cast<int>(meshOf(context).indices.size() / 3);
}

int verticesOfFace(const SMikkTSpaceContext*, int) {
    return 3;
}

void position(const SMikkTSpaceContext* context, float out[], int face, int vert) {
    const Vec3 p = corner(context, face, vert).position;
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
}

void normal(const SMikkTSpaceContext* context, float out[], int face, int vert) {
    const Vec3 n = corner(context, face, vert).normal;
    out[0] = n.x;
    out[1] = n.y;
    out[2] = n.z;
}

void texCoord(const SMikkTSpaceContext* context, float out[], int face, int vert) {
    const Vec2 uv = corner(context, face, vert).uv;
    out[0] = uv.x;
    out[1] = uv.y;
}

/// MikkTSpace reports a tangent per corner of each face. A vertex shared by
/// several faces is written once per face; with a mesh whose vertices were
/// split where the frame differs - which glTF requires - every write agrees.
void setTangent(const SMikkTSpaceContext* context, const float tangent[], float sign, int face, int vert) {
    MikkMesh& mesh = meshOf(context);
    Vertex& v = mesh.vertices[mesh.indices[static_cast<usize>(face) * 3 + static_cast<usize>(vert)]];
    v.tangent = Vec4{tangent[0], tangent[1], tangent[2], sign < 0.0f ? -1.0f : 1.0f};
}

} // namespace

void computeTangents(std::span<Vertex> vertices, std::span<const u32> indices) {
    if (vertices.empty() || indices.size() < 3) {
        return;
    }

    MikkMesh mesh{vertices, indices};

    SMikkTSpaceInterface callbacks{};
    callbacks.m_getNumFaces = faceCount;
    callbacks.m_getNumVerticesOfFace = verticesOfFace;
    callbacks.m_getPosition = position;
    callbacks.m_getNormal = normal;
    callbacks.m_getTexCoord = texCoord;
    callbacks.m_setTSpaceBasic = setTangent;

    SMikkTSpaceContext context{};
    context.m_pInterface = &callbacks;
    context.m_pUserData = &mesh;
    genTangSpaceDefault(&context);
}

} // namespace fumar
