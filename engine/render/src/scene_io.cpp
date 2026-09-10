#include "fumar/render/scene_io.hpp"

#include "fumar/core/log.hpp"
#include "fumar/platform/paths.hpp"
#include "fumar/render/renderer.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <unordered_map>
#include <vector>

namespace fumar {
namespace {

using Json = nlohmann::json;

/// Bumped whenever the layout changes in a way older files cannot express.
/// A file without it, or with a newer one, is refused rather than
/// misinterpreted.
constexpr u32 kSceneVersion = 1;

Json toJson(const Vec3& v) {
    return Json::array({v.x, v.y, v.z});
}

Json toJson(const Vec4& v) {
    return Json::array({v.x, v.y, v.z, v.w});
}

Json toJson(const Quat& q) {
    return Json::array({q.x, q.y, q.z, q.w});
}

Vec3 vec3From(const Json& json, const Vec3& fallback = {}) {
    if (!json.is_array() || json.size() != 3) {
        return fallback;
    }
    return Vec3{json[0].get<f32>(), json[1].get<f32>(), json[2].get<f32>()};
}

Vec4 vec4From(const Json& json, const Vec4& fallback = {}) {
    if (!json.is_array() || json.size() != 4) {
        return fallback;
    }
    return Vec4{json[0].get<f32>(), json[1].get<f32>(), json[2].get<f32>(), json[3].get<f32>()};
}

Quat quatFrom(const Json& json) {
    if (!json.is_array() || json.size() != 4) {
        return Quat{};
    }
    return Quat{json[0].get<f32>(), json[1].get<f32>(), json[2].get<f32>(), json[3].get<f32>()};
}

/// Rewrites an asset path relative to the executable.
///
/// Absolute paths would pin a scene to the machine it was saved on - copy the
/// project anywhere else, or just move the build directory, and every model in
/// it stops loading. Relative to the executable is what survives, because that
/// is where the assets sit.
///
/// Forward slashes on the way out, so a scene saved on Windows opens on Linux.
std::string toPortablePath(const std::string& absolute) {
    std::error_code ec;
    const auto relative = std::filesystem::relative(absolute, executableDirectory(), ec);

    // relative() gives up across drive letters, and a path that genuinely lives
    // elsewhere is better stored verbatim than mangled.
    if (ec || relative.empty()) {
        return absolute;
    }

    std::string text = relative.generic_string();
    return text;
}

std::filesystem::path fromPortablePath(const std::string& stored) {
    const std::filesystem::path path(stored);
    if (path.is_absolute()) {
        return path;
    }
    return executableDirectory() / path;
}

} // namespace

bool saveScene(const Renderer& renderer, const std::filesystem::path& path) {
    const Scene& scene = renderer.scene();
    const ResourceRegistry& resources = renderer.resources();

    Json root;
    root["fumar_scene"] = kSceneVersion;

    // --- camera -------------------------------------------------------------
    // Saved with the scene because where you were looking is part of what you
    // were working on.
    const Camera& camera = renderer.camera();
    root["camera"] = {
        {"position", toJson(camera.position)},
        {"yaw", camera.yaw},
        {"pitch", camera.pitch},
        {"fov", camera.fovYDegrees},
    };

    // --- environment --------------------------------------------------------
    // Angles and colours, exactly as the editor shows them. Storing the derived
    // sun VECTOR instead would save two lines here and cost the ability to
    // reopen the file and keep dragging the sliders.
    const Environment& env = renderer.environment();
    root["environment"] = {
        {"sun_elevation", env.sunElevationDegrees},
        {"sun_azimuth", env.sunAzimuthDegrees},
        {"sun_color", toJson(env.sunColor)},
        {"sun_intensity", env.sunIntensity},
        {"sun_radius", env.sunAngularRadiusDegrees},
        {"sky_zenith", toJson(env.skyZenithColor)},
        {"sky_horizon", toJson(env.skyHorizonColor)},
        {"ground", toJson(env.groundColor)},
        {"sky_intensity", env.skyIntensity},
        {"exposure", env.exposure},
        {"shadow_strength", env.shadowStrength},
        {"occlusion_strength", env.occlusionStrength},
        {"occlusion_radius", env.occlusionRadius},
        {"reflection_strength", env.reflectionStrength},
        {"reflection_roughness_limit", env.reflectionRoughnessLimit},
    };

    // --- meshes -------------------------------------------------------------
    Json meshes = Json::array();
    for (u32 i = 0; i < static_cast<u32>(resources.meshCount()); ++i) {
        const MeshSource& source = resources.meshSource(MeshHandle{i});

        Json entry;
        if (source.imported()) {
            entry["file"] = toPortablePath(source.file);
            entry["primitive"] = source.primitive;
        } else {
            entry["shape"] = source.shape;
            entry["parameters"] = toJson(source.parameters);
        }
        meshes.push_back(std::move(entry));
    }
    root["meshes"] = std::move(meshes);

    // --- materials ----------------------------------------------------------
    Json materials = Json::array();
    for (u32 i = 0; i < static_cast<u32>(resources.materialCount()); ++i) {
        const Material& material = resources.material(MaterialHandle{i});

        Json entry;
        entry["name"] = material.name;
        entry["color"] = toJson(material.baseColorFactor);
        entry["metallic"] = material.metallic;
        entry["roughness"] = material.roughness;

        if (!material.sourceFile.empty()) {
            entry["file"] = toPortablePath(material.sourceFile);
            entry["index"] = material.sourceIndex;
        } else if (!material.baseColorPath.empty()) {
            entry["texture"] = toPortablePath(material.baseColorPath);
        }
        materials.push_back(std::move(entry));
    }
    root["materials"] = std::move(materials);

    // --- nodes --------------------------------------------------------------
    // Depth-first, so parents precede children, and each node stores its parent
    // as an INDEX into this list rather than as a NodeId: ids are assigned at
    // runtime and would collide or dangle on load. One pass is enough to
    // resolve them because a parent is always written first.
    std::unordered_map<NodeId, int> indexOf;
    Json nodes = Json::array();

    scene.traverse([&](NodeId id, u32) {
        const Node& node = scene.node(id);
        const int index = static_cast<int>(nodes.size());
        indexOf[id] = index;

        Json entry;
        entry["name"] = node.name;
        entry["position"] = toJson(node.transform.position);
        entry["rotation"] = toJson(node.transform.rotation);
        entry["scale"] = toJson(node.transform.scale);

        // Omitted when they carry no information, which keeps a hand-written or
        // hand-edited scene file readable.
        if (!node.visible) {
            entry["visible"] = false;
        }
        if (!node.script.empty()) {
            entry["script"] = node.script;
        }
        if (node.mesh.valid()) {
            entry["mesh"] = node.mesh.index;
        }
        if (node.material.valid()) {
            entry["material"] = node.material.index;
        }

        // A nested object rather than flat keys, so a node that is not a light
        // carries nothing about lighting at all.
        if (node.light.has_value()) {
            const Light& light = *node.light;
            entry["light"] = {
                {"type", light.type == LightType::Spot ? "spot" : "point"},
                {"color", toJson(light.color)},
                {"intensity", light.intensity},
                {"range", light.range},
                {"inner", light.innerConeDegrees},
                {"outer", light.outerConeDegrees},
                {"source_radius", light.sourceRadius},
                {"shadows", light.castsShadows},
            };
        }

        const auto parent = indexOf.find(node.parent);
        entry["parent"] = parent != indexOf.end() ? parent->second : -1;

        nodes.push_back(std::move(entry));
    });
    root["nodes"] = std::move(nodes);

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream file(path);
    if (!file) {
        FUMAR_ERROR("cannot write scene '{}'", path.string());
        return false;
    }

    // Indented: the point of JSON here is that a person can read it.
    file << root.dump(2) << '\n';

    FUMAR_INFO("saved '{}': {} nodes, {} meshes, {} materials", path.filename().string(),
               root["nodes"].size(), root["meshes"].size(), root["materials"].size());
    return true;
}

bool loadScene(Renderer& renderer, const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        FUMAR_ERROR("cannot open scene '{}'", path.string());
        return false;
    }

    Json root;
    try {
        // Parsing before touching the renderer, so a malformed file costs
        // nothing: the scene on screen is still whatever it was.
        file >> root;
    } catch (const Json::parse_error& error) {
        FUMAR_ERROR("scene '{}' is not valid JSON: {}", path.string(), error.what());
        return false;
    }

    const u32 version = root.value("fumar_scene", 0u);
    if (version != kSceneVersion) {
        FUMAR_ERROR("scene '{}' has version {}, expected {}", path.string(), version, kSceneVersion);
        return false;
    }

    renderer.resetScene();

    Scene& scene = renderer.scene();

    // --- resources referenced from files ------------------------------------
    // Every glTF named by a mesh or material is loaded once, into a node that is
    // deleted straight afterwards. The nodes are not what we want - the meshes,
    // materials and textures it registered along the way are, and those stay in
    // the registry.
    std::vector<std::string> files;
    const auto noteFile = [&files](const Json& entry) {
        if (!entry.contains("file")) {
            return;
        }
        const auto name = entry["file"].get<std::string>();
        if (std::find(files.begin(), files.end(), name) == files.end()) {
            files.push_back(name);
        }
    };

    for (const Json& entry : root.value("meshes", Json::array())) {
        noteFile(entry);
    }
    for (const Json& entry : root.value("materials", Json::array())) {
        noteFile(entry);
    }

    for (const std::string& name : files) {
        const NodeId scratch = renderer.loadModel(fromPortablePath(name));
        if (scratch != kInvalidNode) {
            scene.destroyNode(scratch);
        } else {
            FUMAR_WARN("scene references '{}', which could not be loaded", name);
        }
    }

    // --- meshes -------------------------------------------------------------
    // Rebuilt in file order, so index N in the file is index N in the registry
    // and node references need no translation.
    const ResourceRegistry& registry = renderer.resources();
    std::vector<MeshHandle> meshes;

    for (const Json& entry : root.value("meshes", Json::array())) {
        if (entry.contains("file")) {
            // Already registered by the load above; find it by what it says it
            // came from.
            // Compared in resolved form: the registry records the path the
            // loader was actually given, which is absolute.
            const auto wantedFile = fromPortablePath(entry["file"].get<std::string>()).string();
            const auto wantedPrimitive = entry.value("primitive", 0u);

            MeshHandle found;
            for (u32 i = 0; i < static_cast<u32>(registry.meshCount()); ++i) {
                const MeshSource& source = registry.meshSource(MeshHandle{i});
                if (source.file == wantedFile && source.primitive == wantedPrimitive) {
                    found = MeshHandle{i};
                    break;
                }
            }
            meshes.push_back(found);
            continue;
        }

        const auto shape = entry.value("shape", std::string{});
        const Vec4 p = vec4From(entry.value("parameters", Json::array()));

        if (shape == "cube") {
            meshes.push_back(renderer.createCubeMesh());
        } else if (shape == "plane") {
            meshes.push_back(renderer.createPlaneMesh(p.x, p.y));
        } else if (shape == "cylinder") {
            meshes.push_back(renderer.createCylinderMesh(p.x, p.y, static_cast<u32>(p.z)));
        } else {
            FUMAR_WARN("unknown mesh shape '{}' in scene", shape);
            meshes.push_back(MeshHandle{});
        }
    }

    // --- materials ----------------------------------------------------------
    std::vector<MaterialHandle> materials;
    for (const Json& entry : root.value("materials", Json::array())) {
        if (entry.contains("file")) {
            const auto wantedFile = fromPortablePath(entry["file"].get<std::string>()).string();
            const auto wantedIndex = entry.value("index", 0u);

            MaterialHandle found;
            for (u32 i = 0; i < static_cast<u32>(registry.materialCount()); ++i) {
                const Material& material = registry.material(MaterialHandle{i});
                if (material.sourceFile == wantedFile && material.sourceIndex == wantedIndex) {
                    found = MaterialHandle{i};
                    break;
                }
            }
            materials.push_back(found);
            continue;
        }

        const MaterialHandle created = renderer.createMaterial(
            entry.value("name", std::string{"material"}),
            vec4From(entry.value("color", Json::array()), Vec4{1.0f, 1.0f, 1.0f, 1.0f}),
            entry.contains("texture") ? fromPortablePath(entry["texture"].get<std::string>())
                                      : std::filesystem::path{});

        Material& material = renderer.resources().material(created);
        material.metallic = entry.value("metallic", material.metallic);
        material.roughness = entry.value("roughness", material.roughness);

        materials.push_back(created);
    }

    // --- nodes --------------------------------------------------------------
    std::vector<NodeId> created;
    created.reserve(root.value("nodes", Json::array()).size());

    for (const Json& entry : root.value("nodes", Json::array())) {
        const int parentIndex = entry.value("parent", -1);

        // Depth-first ordering guarantees the parent was created already, so a
        // single pass is enough.
        const NodeId parent = (parentIndex >= 0 && parentIndex < static_cast<int>(created.size()))
                                  ? created[static_cast<usize>(parentIndex)]
                                  : kRootNode;

        const NodeId id = scene.createNode(entry.value("name", std::string{"node"}), parent);
        created.push_back(id);

        Node& node = scene.node(id);
        node.transform.position = vec3From(entry.value("position", Json::array()));
        node.transform.rotation = quatFrom(entry.value("rotation", Json::array()));
        node.transform.scale = vec3From(entry.value("scale", Json::array()), Vec3{1.0f, 1.0f, 1.0f});
        node.visible = entry.value("visible", true);
        node.script = entry.value("script", std::string{});

        if (entry.contains("light")) {
            const Json& source = entry["light"];
            Light light;
            light.type = source.value("type", std::string{"point"}) == "spot" ? LightType::Spot
                                                                             : LightType::Point;
            light.color = vec3From(source.value("color", Json::array()), light.color);
            light.intensity = source.value("intensity", light.intensity);
            light.range = source.value("range", light.range);
            light.innerConeDegrees = source.value("inner", light.innerConeDegrees);
            light.outerConeDegrees = source.value("outer", light.outerConeDegrees);
            light.sourceRadius = source.value("source_radius", light.sourceRadius);
            light.castsShadows = source.value("shadows", light.castsShadows);
            node.light = light;
        }

        const auto meshIndex = entry.value("mesh", ~0u);
        if (meshIndex < meshes.size()) {
            node.mesh = meshes[meshIndex];
        }

        const auto materialIndex = entry.value("material", ~0u);
        if (materialIndex < materials.size()) {
            node.material = materials[materialIndex];
        }
    }

    // --- environment --------------------------------------------------------
    // Every field falls back to the default, so a scene written before the
    // environment existed loads with a sensible sky rather than a black one.
    if (root.contains("environment")) {
        const Json& source = root["environment"];
        Environment env;
        env.sunElevationDegrees = source.value("sun_elevation", env.sunElevationDegrees);
        env.sunAzimuthDegrees = source.value("sun_azimuth", env.sunAzimuthDegrees);
        env.sunColor = vec3From(source.value("sun_color", Json::array()), env.sunColor);
        env.sunIntensity = source.value("sun_intensity", env.sunIntensity);
        env.sunAngularRadiusDegrees = source.value("sun_radius", env.sunAngularRadiusDegrees);
        env.skyZenithColor = vec3From(source.value("sky_zenith", Json::array()), env.skyZenithColor);
        env.skyHorizonColor = vec3From(source.value("sky_horizon", Json::array()), env.skyHorizonColor);
        env.groundColor = vec3From(source.value("ground", Json::array()), env.groundColor);
        env.skyIntensity = source.value("sky_intensity", env.skyIntensity);
        env.exposure = source.value("exposure", env.exposure);
        env.shadowStrength = source.value("shadow_strength", env.shadowStrength);
        env.occlusionStrength = source.value("occlusion_strength", env.occlusionStrength);
        env.occlusionRadius = source.value("occlusion_radius", env.occlusionRadius);
        env.reflectionStrength = source.value("reflection_strength", env.reflectionStrength);
        env.reflectionRoughnessLimit =
            source.value("reflection_roughness_limit", env.reflectionRoughnessLimit);
        renderer.environment() = env;
    }

    // --- camera -------------------------------------------------------------
    if (root.contains("camera")) {
        const Json& camera = root["camera"];
        renderer.camera().position = vec3From(camera.value("position", Json::array()),
                                              Vec3{0.0f, 3.0f, 9.0f});
        renderer.camera().yaw = camera.value("yaw", -90.0f);
        renderer.camera().pitch = camera.value("pitch", 0.0f);
        renderer.camera().fovYDegrees = camera.value("fov", 60.0f);
    }

    FUMAR_INFO("loaded '{}': {} nodes, {} meshes, {} materials", path.filename().string(),
               created.size(), meshes.size(), materials.size());
    return true;
}

} // namespace fumar
