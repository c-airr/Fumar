#include "fumar/render/gltf_loader.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/core/log.hpp"
#include "fumar/render/texture.hpp"
#include "fumar/rhi/descriptor.hpp"
#include "fumar/rhi/device.hpp"
#include "fumar/rhi/upload_context.hpp"

#include <cgltf.h>

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace fumar {
namespace {

/// cgltf_data is malloc'd by the parser and freed by cgltf_free, so it needs a
/// custom deleter rather than the default one.
struct GltfDeleter {
    void operator()(cgltf_data* data) const {
        if (data != nullptr) {
            cgltf_free(data);
        }
    }
};

using GltfData = std::unique_ptr<cgltf_data, GltfDeleter>;

/// Copies a glTF matrix into ours.
///
/// A straight memcpy works because both sides are column-major 4x4 float
/// matrices - glTF specifies that layout, and fumar's Mat4 uses it to match
/// GLSL. If either changed, this would silently transpose everything.
Mat4 toMat4(const cgltf_float source[16]) {
    Mat4 result{};
    std::memcpy(&result.columns[0].x, source, sizeof(f32) * 16);
    return result;
}

/// Reads a node's local transform.
///
/// glTF nodes carry either an explicit matrix or a translation/rotation/scale
/// triple. The triple is preferred where present: it is what the file author
/// actually specified, and decomposing a matrix back into one loses precision.
Transform readTransform(const cgltf_node& node) {
    if (node.has_matrix) {
        return Transform::fromMatrix(toMat4(node.matrix));
    }

    Transform transform;
    if (node.has_translation) {
        transform.position = Vec3{node.translation[0], node.translation[1], node.translation[2]};
    }
    if (node.has_rotation) {
        // glTF stores quaternions as (x, y, z, w), the same order fumar uses.
        transform.rotation = Quat{node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]};
    }
    if (node.has_scale) {
        transform.scale = Vec3{node.scale[0], node.scale[1], node.scale[2]};
    }
    return transform;
}

/// Reads a vec2 or vec3 attribute one element at a time.
///
/// cgltf_accessor_read_float handles the parts of glTF that make raw pointer
/// access wrong: interleaved buffers with a stride, normalised integer types
/// that need scaling, and sparse accessors that override individual elements.
/// Casting the buffer to a float pointer works right until a file uses one of
/// those, which is why it is not done here.
template <usize N>
bool readAttribute(const cgltf_accessor* accessor, std::vector<std::array<f32, N>>& out) {
    if (accessor == nullptr) {
        return false;
    }

    out.resize(accessor->count);
    for (usize i = 0; i < accessor->count; ++i) {
        if (cgltf_accessor_read_float(accessor, i, out[i].data(), N) == 0) {
            return false;
        }
    }
    return true;
}

/// Computes normals for a mesh that has none.
///
/// The cross product of two triangle edges gives that face's normal; adding it
/// into each of its vertices and normalising at the end produces an
/// area-weighted average, since the unnormalised cross product's length is
/// twice the triangle area.
void generateNormals(std::vector<Vertex>& vertices, const std::vector<u32>& indices) {
    for (Vertex& vertex : vertices) {
        vertex.normal = Vec3{};
    }

    for (usize i = 0; i + 2 < indices.size(); i += 3) {
        Vertex& a = vertices[indices[i + 0]];
        Vertex& b = vertices[indices[i + 1]];
        Vertex& c = vertices[indices[i + 2]];

        const Vec3 faceNormal = cross(b.position - a.position, c.position - a.position);
        a.normal += faceNormal;
        b.normal += faceNormal;
        c.normal += faceNormal;
    }

    for (Vertex& vertex : vertices) {
        vertex.normal = normalize(vertex.normal);
    }
}

/// Resolves a glTF image to pixels, whether it sits in a separate file or in
/// the .glb binary chunk.
rhi::Image loadImage(const cgltf_image& image, const std::filesystem::path& baseDirectory,
                     rhi::Device& device, rhi::UploadContext& upload) {
    if (image.buffer_view != nullptr) {
        const cgltf_buffer_view& view = *image.buffer_view;
        const auto* base = static_cast<const u8*>(view.buffer->data);
        if (base == nullptr) {
            FUMAR_WARN("embedded texture has no buffer data");
            return {};
        }
        return loadTextureFromMemory(device, upload, base + view.offset, view.size);
    }

    if (image.uri == nullptr) {
        return {};
    }

    // URIs are percent-encoded, so a path containing a space arrives as "%20"
    // and would not open. cgltf decodes in place, hence the copy.
    std::string uri = image.uri;
    cgltf_decode_uri(uri.data());
    uri.resize(std::strlen(uri.c_str()));

    return loadTextureFromFile(device, upload, baseDirectory / uri);
}

/// Builds one Mesh from a glTF primitive, or an empty one if it cannot.
Mesh buildMesh(const cgltf_primitive& primitive, rhi::Device& device, rhi::UploadContext& upload) {
    std::vector<std::array<f32, 3>> positions;
    std::vector<std::array<f32, 3>> normals;
    std::vector<std::array<f32, 2>> uvs;

    for (usize i = 0; i < primitive.attributes_count; ++i) {
        const cgltf_attribute& attribute = primitive.attributes[i];
        switch (attribute.type) {
        case cgltf_attribute_type_position:
            readAttribute<3>(attribute.data, positions);
            break;
        case cgltf_attribute_type_normal:
            readAttribute<3>(attribute.data, normals);
            break;
        case cgltf_attribute_type_texcoord:
            // Only the first UV set; glTF allows several, for lightmaps.
            if (attribute.index == 0) {
                readAttribute<2>(attribute.data, uvs);
            }
            break;
        default:
            break;
        }
    }

    if (positions.empty()) {
        return {};
    }

    std::vector<Vertex> vertices(positions.size());
    for (usize v = 0; v < positions.size(); ++v) {
        vertices[v].position = Vec3{positions[v][0], positions[v][1], positions[v][2]};
        if (v < normals.size()) {
            vertices[v].normal = Vec3{normals[v][0], normals[v][1], normals[v][2]};
        }
        if (v < uvs.size()) {
            vertices[v].uv = Vec2{uvs[v][0], uvs[v][1]};
        }
    }

    std::vector<u32> indices;
    if (primitive.indices != nullptr) {
        indices.resize(primitive.indices->count);
        for (usize k = 0; k < indices.size(); ++k) {
            // Reads through whatever integer width the file used - 8, 16 or 32
            // bit - and widens it to ours.
            indices[k] = static_cast<u32>(cgltf_accessor_read_index(primitive.indices, k));
        }
    } else {
        // An unindexed primitive is just its vertices in order.
        indices.resize(vertices.size());
        for (u32 k = 0; k < static_cast<u32>(indices.size()); ++k) {
            indices[k] = k;
        }
    }

    if (normals.empty()) {
        FUMAR_DEBUG("primitive has no normals, generating them");
        generateNormals(vertices, indices);
    }

    return Mesh(device, upload, vertices, indices);
}

} // namespace

NodeId loadGltfIntoScene(const std::filesystem::path& path, const GltfLoadContext& context, Scene& scene,
                         ResourceRegistry& resources, NodeId parent) {
    const std::string pathString = path.string();

    cgltf_options options{};
    cgltf_data* raw = nullptr;

    if (cgltf_parse_file(&options, pathString.c_str(), &raw) != cgltf_result_success) {
        FUMAR_ERROR("could not parse glTF '{}'", pathString);
        return kInvalidNode;
    }
    GltfData data(raw);

    // Parsing only reads the JSON. The vertex data lives in external .bin files
    // or the .glb binary chunk, and this is what pulls it in.
    if (cgltf_load_buffers(&options, data.get(), pathString.c_str()) != cgltf_result_success) {
        FUMAR_ERROR("could not load buffers for '{}'", pathString);
        return kInvalidNode;
    }

    if (cgltf_validate(data.get()) != cgltf_result_success) {
        FUMAR_ERROR("glTF '{}' failed validation", pathString);
        return kInvalidNode;
    }

    const std::filesystem::path baseDirectory = path.parent_path();

    // --- textures -----------------------------------------------------------
    std::vector<TextureHandle> textureHandles(data->images_count);
    for (usize i = 0; i < data->images_count; ++i) {
        rhi::Image image = loadImage(data->images[i], baseDirectory, context.device, context.upload);
        textureHandles[i] = image.valid() ? resources.addTexture(std::move(image)) : TextureHandle{};
    }

    // --- materials ----------------------------------------------------------
    // One descriptor set per material, written once here. Per-frame rebinding
    // is cheap; rebuilding the set would not be.
    rhi::DescriptorWriter writer;
    std::vector<MaterialHandle> materialHandles(data->materials_count);

    for (usize i = 0; i < data->materials_count; ++i) {
        const cgltf_material& source = data->materials[i];

        Material material;
        material.name = source.name != nullptr ? source.name : "material";

        vk::ImageView view = context.fallbackTexture;
        if (source.has_pbr_metallic_roughness) {
            const auto& pbr = source.pbr_metallic_roughness;
            material.baseColorFactor = Vec4{pbr.base_color_factor[0], pbr.base_color_factor[1],
                                            pbr.base_color_factor[2], pbr.base_color_factor[3]};

            const cgltf_texture* texture = pbr.base_color_texture.texture;
            if (texture != nullptr && texture->image != nullptr) {
                // Pointer arithmetic against the array base is how cgltf
                // expects you to recover an index - it stores pointers, not ids.
                const usize imageIndex = static_cast<usize>(texture->image - data->images);
                if (imageIndex < textureHandles.size() && textureHandles[imageIndex].valid()) {
                    material.baseColor = textureHandles[imageIndex];
                    view = resources.texture(material.baseColor).view();
                }
            }
        }

        // Recorded so a saved scene can find this material again by reloading
        // the file, rather than trying to store an embedded texture itself.
        material.sourceFile = pathString;
        material.sourceIndex = static_cast<u32>(i);

        material.descriptorSet = context.descriptorPool.allocate(context.materialSetLayout);
        writer.image(material.descriptorSet, 0, view, context.sampler);
        materialHandles[i] = resources.addMaterial(std::move(material));
    }

    writer.submit(context.device.handle());

    // --- nodes --------------------------------------------------------------
    // glTF node indices map one-to-one onto scene nodes, so the parent links
    // can be rebuilt in a second pass without searching.
    std::vector<NodeId> nodeIds(data->nodes_count, kInvalidNode);

    const NodeId modelRoot = scene.createNode(path.stem().string(), parent);

    for (usize i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& source = data->nodes[i];
        const std::string name = source.name != nullptr ? source.name : "node";

        // Parented to the model root for now; the real parent is attached below
        // once every node exists.
        nodeIds[i] = scene.createNode(name, modelRoot);
        scene.node(nodeIds[i]).transform = readTransform(source);
    }

    for (usize i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& source = data->nodes[i];
        if (source.parent == nullptr) {
            continue;
        }
        const usize parentIndex = static_cast<usize>(source.parent - data->nodes);
        if (parentIndex < nodeIds.size()) {
            scene.setParent(nodeIds[i], nodeIds[parentIndex]);
        }
    }

    // --- geometry -----------------------------------------------------------
    usize primitiveCount = 0;
    u32 primitiveCounter = 0;
    for (usize i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& source = data->nodes[i];
        if (source.mesh == nullptr) {
            continue;
        }

        for (usize p = 0; p < source.mesh->primitives_count; ++p) {
            const cgltf_primitive& primitive = source.mesh->primitives[p];

            // Triangles only. glTF also permits points, lines and strips, each
            // of which would need its own pipeline topology.
            if (primitive.type != cgltf_primitive_type_triangles) {
                FUMAR_DEBUG("skipping a non-triangle primitive");
                continue;
            }

            Mesh mesh = buildMesh(primitive, context.device, context.upload);
            if (!mesh.valid()) {
                continue;
            }

            MaterialHandle material = resources.fallbackMaterial();
            if (primitive.material != nullptr) {
                const usize index = static_cast<usize>(primitive.material - data->materials);
                if (index < materialHandles.size()) {
                    material = materialHandles[index];
                }
            }

            // A glTF mesh can hold several primitives, each with its own
            // material, and a scene node draws exactly one. So every primitive
            // past the first becomes a child node with an identity transform.
            NodeId target = nodeIds[i];
            if (p > 0) {
                target = scene.createNode(scene.node(nodeIds[i]).name + "_part", nodeIds[i]);
            }

            // The file and a running index, so a saved scene can find this
            // exact primitive again after a reload.
            scene.node(target).mesh =
                resources.addMesh(std::move(mesh), importedMesh(pathString, primitiveCounter++));
            scene.node(target).material = material;
            ++primitiveCount;
        }
    }

    if (primitiveCount == 0) {
        FUMAR_WARN("glTF '{}' contained no drawable geometry", pathString);
        scene.destroyNode(modelRoot);
        return kInvalidNode;
    }

    FUMAR_INFO("loaded '{}': {} nodes, {} primitives, {} textures, {} materials",
               path.filename().string(), data->nodes_count, primitiveCount, data->images_count,
               data->materials_count);
    return modelRoot;
}

} // namespace fumar
