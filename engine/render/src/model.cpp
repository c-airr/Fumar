#include "fumar/render/model.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/core/log.hpp"
#include "fumar/render/texture.hpp"
#include "fumar/rhi/descriptor.hpp"
#include "fumar/rhi/device.hpp"
#include "fumar/rhi/upload_context.hpp"

#include <cgltf.h>

#include <cstring>
#include <memory>
#include <unordered_map>

namespace fumar {
namespace {

/// cgltf_data is malloc'd by the parser and freed by cgltf_free, so it gets a
/// custom deleter rather than a plain unique_ptr.
struct GltfDeleter {
    void operator()(cgltf_data* data) const {
        if (data != nullptr) {
            cgltf_free(data);
        }
    }
};

using GltfData = std::unique_ptr<cgltf_data, GltfDeleter>;

/// Copies a node's world transform into our Mat4.
///
/// A straight memcpy works because both sides are column-major 4x4 float
/// matrices - glTF specifies that layout, and fumar's Mat4 uses it to match
/// GLSL. If either changed, this would silently transpose the scene.
Mat4 toMat4(const cgltf_float source[16]) {
    Mat4 result{};
    std::memcpy(&result.columns[0].x, source, sizeof(f32) * 16);
    return result;
}

/// Reads a vec2 or vec3 attribute into a vector, one element at a time.
///
/// cgltf_accessor_read_float handles the parts of glTF that make raw pointer
/// access wrong: interleaved buffers with a stride, normalised integer types
/// that need scaling, and sparse accessors that override individual elements.
/// Casting the buffer to a float pointer works right up until a file uses one
/// of those, which is why it is not done here.
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

/// Computes flat normals when a mesh has none.
///
/// The cross product of two triangle edges gives that face's normal; adding it
/// into each of its vertices and normalising at the end produces the
/// area-weighted average, which is a reasonable smooth normal.
void generateNormals(std::vector<Vertex>& vertices, const std::vector<u32>& indices) {
    for (Vertex& vertex : vertices) {
        vertex.normal = Vec3{};
    }

    for (usize i = 0; i + 2 < indices.size(); i += 3) {
        Vertex& a = vertices[indices[i + 0]];
        Vertex& b = vertices[indices[i + 1]];
        Vertex& c = vertices[indices[i + 2]];

        // Not normalised: the magnitude is twice the triangle area, so larger
        // faces contribute proportionally more to the shared vertex.
        const Vec3 faceNormal = cross(b.position - a.position, c.position - a.position);
        a.normal += faceNormal;
        b.normal += faceNormal;
        c.normal += faceNormal;
    }

    for (Vertex& vertex : vertices) {
        vertex.normal = normalize(vertex.normal);
    }
}

/// Resolves a glTF image to pixels, whether it lives in a separate file or
/// inside the .glb binary chunk.
rhi::Image loadGltfImage(const cgltf_image& image, const std::filesystem::path& baseDirectory,
                         rhi::Device& device, rhi::UploadContext& upload) {
    if (image.buffer_view != nullptr) {
        // Embedded: the bytes are already in memory, still in their original
        // PNG or JPEG encoding.
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

    // URIs are percent-encoded, so a path with a space arrives as "%20" and
    // would not open. cgltf decodes in place, hence the copy.
    std::string uri = image.uri;
    cgltf_decode_uri(uri.data());
    uri.resize(std::strlen(uri.c_str()));

    return loadTextureFromFile(device, upload, baseDirectory / uri);
}

} // namespace

std::optional<Model> Model::loadGltf(const std::filesystem::path& path, const ModelLoadContext& context) {
    const std::string pathString = path.string();

    cgltf_options options{};
    cgltf_data* raw = nullptr;

    if (cgltf_parse_file(&options, pathString.c_str(), &raw) != cgltf_result_success) {
        FUMAR_ERROR("could not parse glTF '{}'", pathString);
        return std::nullopt;
    }
    GltfData data(raw);

    // Parsing only reads the JSON. The actual vertex data sits in external .bin
    // files or the .glb binary chunk, and this is what pulls it in.
    if (cgltf_load_buffers(&options, data.get(), pathString.c_str()) != cgltf_result_success) {
        FUMAR_ERROR("could not load buffers for '{}'", pathString);
        return std::nullopt;
    }

    if (cgltf_validate(data.get()) != cgltf_result_success) {
        FUMAR_ERROR("glTF '{}' failed validation", pathString);
        return std::nullopt;
    }

    Model model;
    const std::filesystem::path baseDirectory = path.parent_path();

    // --- textures -----------------------------------------------------------
    model.m_textures.reserve(data->images_count);
    for (usize i = 0; i < data->images_count; ++i) {
        model.m_textures.push_back(loadGltfImage(data->images[i], baseDirectory, context.device,
                                                 context.upload));
    }

    // --- materials ----------------------------------------------------------
    // One descriptor set per material, built once here rather than per frame.
    rhi::DescriptorWriter writer;
    std::vector<vk::DescriptorSet> materialSets(data->materials_count);

    for (usize i = 0; i < data->materials_count; ++i) {
        const cgltf_material& material = data->materials[i];

        vk::ImageView view = context.fallbackTexture;
        if (material.has_pbr_metallic_roughness) {
            const cgltf_texture* texture = material.pbr_metallic_roughness.base_color_texture.texture;
            if (texture != nullptr && texture->image != nullptr) {
                const usize index = static_cast<usize>(texture->image - data->images);
                if (index < model.m_textures.size() && model.m_textures[index].valid()) {
                    view = model.m_textures[index].view();
                }
            }
        }

        materialSets[i] = context.descriptorPool.allocate(context.materialSetLayout);
        writer.image(materialSets[i], 0, view, context.sampler);
    }

    // A set for primitives with no material at all, which glTF permits.
    const vk::DescriptorSet defaultSet = context.descriptorPool.allocate(context.materialSetLayout);
    writer.image(defaultSet, 0, context.fallbackTexture, context.sampler);

    writer.submit(context.device.handle());

    // --- geometry -----------------------------------------------------------
    for (usize nodeIndex = 0; nodeIndex < data->nodes_count; ++nodeIndex) {
        const cgltf_node& node = data->nodes[nodeIndex];
        if (node.mesh == nullptr) {
            continue;
        }

        // Walks up through every parent, so a node nested three levels deep
        // lands in the right place without us tracking the hierarchy.
        cgltf_float worldMatrix[16];
        cgltf_node_transform_world(&node, worldMatrix);
        const Mat4 transform = toMat4(worldMatrix);

        for (usize primitiveIndex = 0; primitiveIndex < node.mesh->primitives_count; ++primitiveIndex) {
            const cgltf_primitive& primitive = node.mesh->primitives[primitiveIndex];

            // Only triangles. glTF also allows points, lines and strips, which
            // would each need their own pipeline topology.
            if (primitive.type != cgltf_primitive_type_triangles) {
                FUMAR_DEBUG("skipping a non-triangle primitive in '{}'", pathString);
                continue;
            }

            std::vector<std::array<f32, 3>> positions;
            std::vector<std::array<f32, 3>> normals;
            std::vector<std::array<f32, 2>> uvs;

            for (usize a = 0; a < primitive.attributes_count; ++a) {
                const cgltf_attribute& attribute = primitive.attributes[a];
                switch (attribute.type) {
                case cgltf_attribute_type_position:
                    readAttribute<3>(attribute.data, positions);
                    break;
                case cgltf_attribute_type_normal:
                    readAttribute<3>(attribute.data, normals);
                    break;
                case cgltf_attribute_type_texcoord:
                    // Only the first UV set; glTF allows several for lightmaps.
                    if (attribute.index == 0) {
                        readAttribute<2>(attribute.data, uvs);
                    }
                    break;
                default:
                    break;
                }
            }

            if (positions.empty()) {
                continue;
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
                    // Reads through whatever integer width the file used -
                    // 8, 16 or 32 bit - and widens it to ours.
                    indices[k] = static_cast<u32>(cgltf_accessor_read_index(primitive.indices, k));
                }
            } else {
                // An unindexed primitive is just vertices in order.
                indices.resize(vertices.size());
                for (u32 k = 0; k < static_cast<u32>(indices.size()); ++k) {
                    indices[k] = k;
                }
            }

            if (normals.empty()) {
                FUMAR_DEBUG("primitive has no normals, generating them");
                generateNormals(vertices, indices);
            }

            vk::DescriptorSet materialSet = defaultSet;
            if (primitive.material != nullptr) {
                const usize index = static_cast<usize>(primitive.material - data->materials);
                if (index < materialSets.size()) {
                    materialSet = materialSets[index];
                }
            }

            model.m_primitives.push_back(Primitive{
                .mesh = Mesh(context.device, context.upload, vertices, indices),
                .materialSet = materialSet,
                .transform = transform,
            });
        }
    }

    if (model.m_primitives.empty()) {
        FUMAR_WARN("glTF '{}' contained no drawable geometry", pathString);
        return std::nullopt;
    }

    FUMAR_INFO("loaded '{}': {} primitives, {} textures", path.filename().string(),
               model.m_primitives.size(), model.m_textures.size());
    return model;
}

void Model::draw(vk::CommandBuffer cmd, vk::PipelineLayout layout, const Mat4& rootTransform) const {
    struct ObjectPushConstants {
        Mat4 model;
    };

    for (const Primitive& primitive : m_primitives) {
        // Rebinding set 1 per primitive is the naive approach. Sorting draws by
        // material first would cut these binds down considerably - worth doing
        // once there is a scene big enough for it to matter.
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 1, primitive.materialSet, {});

        const ObjectPushConstants push{.model = rootTransform * primitive.transform};
        cmd.pushConstants<ObjectPushConstants>(layout, vk::ShaderStageFlagBits::eVertex, 0, push);

        primitive.mesh.draw(cmd);
    }
}

} // namespace fumar
