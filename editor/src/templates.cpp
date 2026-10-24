#include "templates.hpp"

#include "materials_panel.hpp"

#include "fumar/core/log.hpp"
#include "fumar/core/math.hpp"
#include "fumar/platform/paths.hpp"
#include "fumar/render/environment.hpp"
#include "fumar/render/material_file.hpp"
#include "fumar/render/renderer.hpp"
#include "fumar/scene/scene.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

namespace fumar {
namespace {

/// The walled courtyard the editor opens with: grass, brick, a wooden deck, a
/// polished pillar, a lamp and the helmet.
void buildStarter(Renderer& renderer) {
    Scene& scene = renderer.scene();
    const std::filesystem::path textures = executableDirectory() / "assets" / "textures";

    // A small walled courtyard, because an open plain is the worst possible
    // scene for showing what this renderer does. Shadows need something to fall
    // across, bounced light needs a surface to bounce off, and a reflection of
    // an empty sky is a reflection of nothing. Three walls give all three.
    //
    // Base colours are LINEAR reflectances, not the numbers a colour picker
    // shows. Something that looks mid-grey on screen reflects about 20% of the
    // light hitting it, not 50. Where a texture supplies the colour the factor
    // stays near white and only trims the brightness.
    //
    // The stock library's full materials when it has been fetched (see
    // tools/get-materials), and the plain colour textures that ship on main
    // when it has not - so a fresh clone opens the same courtyard, only
    // flatter.
    //
    // A stock material is taken as a DESCRIPTION, made into the scene's own
    // material with the scene's tiling - not loaded as the shared .fmat. The
    // tiling belongs to these walls and this ground, not to every scene that
    // uses grass, and a scene keeps its own materials when it is saved.
    const std::filesystem::path stock = stockMaterialsDirectory();
    const auto pick = [&](const char* stockName, const char* name, Vec4 colour, const std::filesystem::path& texture,
                          Vec2 tiling) {
        const std::filesystem::path fmat = stock / stockName / (std::string(stockName) + ".fmat");
        std::error_code ec;
        if (std::filesystem::exists(fmat, ec)) {
            if (std::optional<MaterialDesc> desc = readMaterialFile(fmat)) {
                desc->uvScale = tiling;
                return renderer.createMaterial(
                    *desc, MaterialOrigin{.kind = MaterialOrigin::Kind::Builtin, .file = {}, .index = 0});
            }
        }
        const MaterialHandle plain = renderer.createMaterial(name, colour, texture);
        renderer.resources().material(plain).uvScale = tiling;
        return plain;
    };
    const MaterialHandle grassMaterial =
        pick("Grass", "Grass", Vec4{0.85f, 0.9f, 0.85f, 1.0f}, textures / "grass.jpg", Vec2{14.0f, 14.0f});
    const MaterialHandle brickMaterial =
        pick("Bricks", "Brick", Vec4{0.85f, 0.85f, 0.85f, 1.0f}, textures / "brick.jpg", Vec2{6.0f, 2.0f});
    const MaterialHandle woodMaterial =
        pick("WoodFloor", "Wood", Vec4{0.9f, 0.9f, 0.9f, 1.0f}, textures / "wood.jpg", Vec2{3.0f, 3.0f});
    const MaterialHandle stoneMaterial =
        renderer.createMaterial("Stone", Vec4{0.34f, 0.335f, 0.32f, 1.0f});
    const MaterialHandle metalMaterial =
        renderer.createMaterial("Polished metal", Vec4{0.55f, 0.56f, 0.58f, 1.0f});

    ResourceRegistry& resources = renderer.resources();

    // Tiling is what keeps a texture at a believable size. A node's scale does
    // not touch texture coordinates, so a cube stretched into a ten-metre wall
    // would otherwise carry one enormous brick.
    //
    // A stock material's roughness comes from its map, which the factor
    // scales - so only the plain fallbacks get a roughness set here.
    const auto roughen = [&](MaterialHandle handle, f32 roughness) {
        Material& material = resources.material(handle);
        if (material.roughnessPath.empty() && material.packedMetallicRoughnessPath.empty()) {
            material.roughness = roughness;
        }
    };
    roughen(grassMaterial, 0.95f);
    roughen(brickMaterial, 0.85f);
    roughen(woodMaterial, 0.55f);

    resources.material(stoneMaterial).roughness = 0.75f;

    // Polished metal, so the courtyard shows a ray traced reflection without
    // anyone having to build one: the grass, the brick and the sky all appear
    // in it. Turn its roughness up in Details and watch the reflection dissolve
    // into the sky gradient - that is the whole difference between the two
    // methods, in one slider.
    resources.material(metalMaterial).roughness = 0.12f;
    resources.material(metalMaterial).metallic = 1.0f;

    const MeshHandle groundMesh = renderer.createPlaneMesh(16.0f);
    const MeshHandle cubeMesh = renderer.createCubeMesh();
    const MeshHandle cylinderMesh = renderer.createCylinderMesh(0.9f, 3.0f, 40);

    // --- ground -------------------------------------------------------------
    const NodeId ground = scene.createNode("Ground");
    scene.node(ground).mesh = groundMesh;
    scene.node(ground).material = grassMaterial;

    // --- walls --------------------------------------------------------------
    // Boxes rather than flat quads, so they have a thickness for the sun to
    // find an edge of and for bounced light to come off the inside face of.
    const NodeId walls = scene.createNode("Walls");

    struct WallPlacement {
        const char* name;
        Vec3 position;
        Vec3 scale;
    };

    // Open towards the camera, so the courtyard reads as a place you are
    // standing in rather than a box you are looking into.
    const std::array<WallPlacement, 3> wallPlacements{{
        {"Wall back", Vec3{0.0f, 1.75f, -7.0f}, Vec3{14.0f, 3.5f, 0.5f}},
        {"Wall left", Vec3{-7.0f, 1.75f, 0.0f}, Vec3{0.5f, 3.5f, 14.0f}},
        {"Wall right", Vec3{7.0f, 1.75f, 0.0f}, Vec3{0.5f, 3.5f, 14.0f}},
    }};

    for (const WallPlacement& wall : wallPlacements) {
        const NodeId id = scene.createNode(wall.name, walls);
        scene.node(id).mesh = cubeMesh;
        scene.node(id).material = brickMaterial;
        scene.node(id).transform.position = wall.position;
        scene.node(id).transform.scale = wall.scale;
    }

    // --- a wooden deck ------------------------------------------------------
    const NodeId deck = scene.createNode("Deck");
    scene.node(deck).mesh = cubeMesh;
    scene.node(deck).material = woodMaterial;
    scene.node(deck).transform.position = Vec3{-3.4f, 0.15f, 2.6f};
    scene.node(deck).transform.scale = Vec3{5.0f, 0.3f, 4.0f};

    // --- the pillar ---------------------------------------------------------
    const NodeId pillar = scene.createNode("Pillar");
    scene.node(pillar).mesh = cylinderMesh;
    scene.node(pillar).material = metalMaterial;
    scene.node(pillar).transform.position = Vec3{0.0f, 1.5f, -1.0f};

    // --- some blocks --------------------------------------------------------
    const NodeId blocks = scene.createNode("Blocks");

    struct BlockPlacement {
        Vec3 position;
        Vec3 scale;
        f32 yawDegrees;
    };

    const std::array<BlockPlacement, 4> blockPlacements{{
        {Vec3{3.4f, 0.5f, 1.4f}, Vec3{1.0f, 1.0f, 1.0f}, 18.0f},
        {Vec3{4.6f, 0.3f, -2.4f}, Vec3{0.6f, 0.6f, 0.6f}, -35.0f},
        {Vec3{-4.8f, 0.55f, -3.6f}, Vec3{1.1f, 1.1f, 1.1f}, 8.0f},
        {Vec3{1.6f, 0.25f, 3.8f}, Vec3{1.6f, 0.5f, 1.0f}, 52.0f},
    }};

    char blockName[32];
    int blockIndex = 0;
    for (const BlockPlacement& block : blockPlacements) {
        std::snprintf(blockName, sizeof(blockName), "Block %c",
                      static_cast<char>('A' + blockIndex++));

        const NodeId id = scene.createNode(blockName, blocks);
        scene.node(id).mesh = cubeMesh;
        scene.node(id).material = stoneMaterial;
        scene.node(id).transform.position = block.position;
        scene.node(id).transform.scale = block.scale;
        scene.node(id).transform.rotation =
            fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, radians(block.yawDegrees));
    }

    // One block bobs, so Play visibly does two different things at once.
    if (scene.isAlive(blocks) && !scene.node(blocks).children.empty()) {
        scene.node(scene.node(blocks).children.front()).script = "bob";
    }

    // --- a lamp -------------------------------------------------------------
    // Warm, low, and inside the courtyard where the sun does not reach. A light
    // that only brightens what is already lit teaches nothing.
    const NodeId lamp = scene.createNode("Lamp");
    scene.node(lamp).transform.position = Vec3{-3.4f, 1.6f, 2.6f};
    scene.node(lamp).light = Light{
        .color = Vec3{1.0f, 0.62f, 0.30f},
        .intensity = 55.0f,
        .range = 9.0f,
    };

    const std::filesystem::path modelPath = executableDirectory() / "assets" / "DamagedHelmet.glb";
    if (std::filesystem::exists(modelPath)) {
        const NodeId model = renderer.loadModel(modelPath);
        if (model != kInvalidNode) {
            scene.node(model).transform.position = Vec3{0.0f, 4.2f, -1.0f};

            // A C++ component, while a block above carries a Lua script. Both
            // run in the same frame off the same context, which is the whole
            // claim the dual scripting model makes - press Play and watch one
            // object driven from each side.
            scene.node(model).component = "Spinner";
        }
    }

    // The camera, placed to look into the courtyard rather than at the origin.
    renderer.camera().position = Vec3{6.5f, 4.2f, 9.5f};
    renderer.camera().yaw = -125.0f;
    renderer.camera().pitch = -16.0f;
}

/// A stock material when the library has been fetched (tools/get-materials),
/// a plain colour - textured where main ships a texture - when it has not.
///
/// Like the starter's: the stock material is taken as a description and made
/// into the scene's own, with the scene's tiling. stockRoughness scales the
/// stock roughness map, which is how the same asphalt becomes wet asphalt.
MaterialHandle pickMaterial(Renderer& renderer, const char* stockName, const char* name, Vec4 colour,
                            const char* fallbackTexture, Vec2 tiling, f32 roughness, f32 stockRoughness = 1.0f) {
    const std::filesystem::path fmat =
        stockMaterialsDirectory() / stockName / (std::string(stockName) + ".fmat");
    std::error_code ec;
    if (std::filesystem::exists(fmat, ec)) {
        if (std::optional<MaterialDesc> desc = readMaterialFile(fmat)) {
            desc->uvScale = tiling;
            desc->roughnessFactor *= stockRoughness;
            return renderer.createMaterial(*desc,
                                           MaterialOrigin{.kind = MaterialOrigin::Kind::Builtin, .file = {}, .index = 0});
        }
    }
    std::filesystem::path texture;
    if (fallbackTexture != nullptr) {
        texture = executableDirectory() / "assets" / "textures" / fallbackTexture;
    }
    const MaterialHandle plain = renderer.createMaterial(name, colour, texture);
    renderer.resources().material(plain).uvScale = tiling;
    renderer.resources().material(plain).roughness = roughness;
    return plain;
}

/// A flat colour. Colours are linear reflectances, as in the starter.
MaterialHandle plainMaterial(Renderer& renderer, const char* name, Vec3 colour, f32 roughness, f32 metallic = 0.0f) {
    const MaterialHandle handle = renderer.createMaterial(name, Vec4{colour.x, colour.y, colour.z, 1.0f});
    renderer.resources().material(handle).roughness = roughness;
    renderer.resources().material(handle).metallic = metallic;
    return handle;
}

/// A surface that gives off light. Dark underneath, so what shows is the glow.
MaterialHandle glowMaterial(Renderer& renderer, const char* name, Vec3 colour, f32 intensity) {
    const MaterialHandle handle = plainMaterial(renderer, name, Vec3{0.02f, 0.02f, 0.02f}, 0.6f);
    renderer.resources().material(handle).emissiveColor = colour;
    renderer.resources().material(handle).emissiveIntensity = intensity;
    return handle;
}

/// Nothing but a floor under the sky, the way Unreal's Blank starts.
void buildBlank(Renderer& renderer) {
    Scene& scene = renderer.scene();

    const MaterialHandle floorMaterial =
        plainMaterial(renderer, "Floor", Vec3{0.18f, 0.18f, 0.18f}, 0.8f);

    const NodeId floor = scene.createNode("Floor");
    scene.node(floor).mesh = renderer.createPlaneMesh(20.0f);
    scene.node(floor).material = floorMaterial;

    renderer.camera().position = Vec3{6.0f, 3.0f, 8.0f};
    renderer.camera().yaw = -127.0f;
    renderer.camera().pitch = -15.0f;
}

/// A walk rather than a view: a walled yard in daylight, a lamp-lit tunnel out
/// of it, and a hall at the end lit through a hole in the roof.
///
/// Each stage shows one thing the rays do that a rasteriser fakes or cannot do
/// at all, in the order you meet them:
///
/// - the yard: sun shadows, and a puddle reflecting the sky and the walls;
/// - the tunnel: no sun at all, so everything is the lamps, their shadows, and
///   their reflections running down a wet floor - plus glowing strips that light
///   the walls only through bounced light;
/// - the hall: red and green walls bleeding colour onto a white floor, a mirror,
///   and five gold balls from polished to rough, where the reflection fades into
///   the sky approximation past the roughness limit.
///
/// Laid out along -Z: the yard around the origin, the tunnel from z = -8 to -30,
/// the hall from -30 to -42. Everything is a box, so it can be pulled apart and
/// rebuilt in the Modeler.
void buildRayTracing(Renderer& renderer) {
    Scene& scene = renderer.scene();

    Environment& env = renderer.environment();
    // High enough for the sun to come down through the hall's skylight, and a
    // larger disc than the default so its shadows soften with distance - the
    // effect the rays get right and a shadow map does not.
    env.sunElevationDegrees = 64.0f;
    env.sunAzimuthDegrees = 150.0f;
    env.sunAngularRadiusDegrees = 1.2f;

    // --- materials ----------------------------------------------------------
    const MaterialHandle cobbles =
        pickMaterial(renderer, "Rock", "Cobbles", Vec4{0.30f, 0.29f, 0.27f, 1.0f}, nullptr, Vec2{8.0f, 8.0f}, 0.9f);
    const MaterialHandle brick =
        pickMaterial(renderer, "Bricks", "Brick", Vec4{0.85f, 0.85f, 0.85f, 1.0f}, "brick.jpg", Vec2{6.0f, 2.0f}, 0.85f);
    const MaterialHandle wood = pickMaterial(renderer, "WoodFloor", "Wood", Vec4{0.9f, 0.9f, 0.9f, 1.0f}, "wood.jpg",
                                             Vec2{1.0f, 1.0f}, 0.6f);
    const MaterialHandle tiles =
        pickMaterial(renderer, "Tiles", "Tiles", Vec4{0.62f, 0.62f, 0.60f, 1.0f}, nullptr, Vec2{8.0f, 1.0f}, 0.3f);
    const MaterialHandle concrete = pickMaterial(renderer, "Concrete", "Concrete", Vec4{0.32f, 0.32f, 0.31f, 1.0f},
                                                 nullptr, Vec2{4.0f, 4.0f}, 0.85f);
    // The one surface the whole tunnel depends on. Smooth enough that every
    // lamp shows in it, not so smooth it reads as glass.
    const MaterialHandle wetAsphalt = pickMaterial(renderer, "Asphalt", "Wet asphalt", Vec4{0.06f, 0.06f, 0.065f, 1.0f},
                                                   nullptr, Vec2{2.0f, 12.0f}, 0.08f, 0.15f);
    const MaterialHandle plaster = pickMaterial(renderer, "Plaster", "Plaster", Vec4{0.75f, 0.74f, 0.72f, 1.0f},
                                                nullptr, Vec2{3.0f, 2.0f}, 0.9f);

    const MaterialHandle puddle = plainMaterial(renderer, "Puddle", Vec3{0.02f, 0.025f, 0.03f}, 0.02f);
    const MaterialHandle stone = plainMaterial(renderer, "Stone", Vec3{0.34f, 0.335f, 0.32f}, 0.75f);
    const MaterialHandle steel = plainMaterial(renderer, "Brushed steel", Vec3{0.56f, 0.57f, 0.58f}, 0.22f, 1.0f);
    const MaterialHandle mirror = plainMaterial(renderer, "Mirror", Vec3{0.92f, 0.92f, 0.92f}, 0.0f, 1.0f);
    const MaterialHandle red = plainMaterial(renderer, "Red paint", Vec3{0.55f, 0.05f, 0.04f}, 0.8f);
    const MaterialHandle green = plainMaterial(renderer, "Green paint", Vec3{0.06f, 0.42f, 0.08f}, 0.8f);
    const MaterialHandle white = plainMaterial(renderer, "White floor", Vec3{0.7f, 0.7f, 0.68f}, 0.5f);

    const MaterialHandle lampGlow = glowMaterial(renderer, "Lamp glow", Vec3{1.0f, 0.66f, 0.34f}, 8.0f);
    const MaterialHandle stripGlow = glowMaterial(renderer, "Strip glow", Vec3{0.2f, 0.75f, 1.0f}, 4.0f);
    const MaterialHandle signGlow = glowMaterial(renderer, "Sign glow", Vec3{0.25f, 1.0f, 0.35f}, 6.0f);

    // Five golds from mirror-smooth to fully rough. The default reflection
    // limit is 0.35, so the third is where the traced reflection hands over to
    // the sky - move the limit in World settings and watch the hand-over move.
    std::array<MaterialHandle, 5> golds{};
    for (usize i = 0; i < golds.size(); ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Gold %.2f", static_cast<double>(0.2f * static_cast<f32>(i)));
        golds[i] = plainMaterial(renderer, name, Vec3{1.0f, 0.77f, 0.34f}, 0.2f * static_cast<f32>(i), 1.0f);
    }

    const MeshHandle cube = renderer.createCubeMesh();
    const MeshHandle ball = renderer.createSphereMesh(0.5f, 48, 24);

    const auto box = [&](const char* name, NodeId parent, MaterialHandle material, Vec3 position, Vec3 scale) {
        const NodeId id = scene.createNode(name, parent);
        scene.node(id).mesh = cube;
        scene.node(id).material = material;
        scene.node(id).transform.position = position;
        scene.node(id).transform.scale = scale;
        return id;
    };

    const auto lamp = [&](const char* name, NodeId parent, Vec3 position, Vec3 colour, f32 intensity, f32 range) {
        const NodeId id = scene.createNode(name, parent);
        scene.node(id).transform.position = position;
        scene.node(id).light = Light{.color = colour, .intensity = intensity, .range = range};
        return id;
    };

    // --- the yard -----------------------------------------------------------
    // Walls on all four sides: the only way on is the tunnel.
    const NodeId yard = scene.createNode("Yard");

    const NodeId ground = scene.createNode("Ground", yard);
    scene.node(ground).mesh = renderer.createPlaneMesh(8.5f);
    scene.node(ground).material = cobbles;

    box("Wall left", yard, brick, Vec3{-8.0f, 2.0f, 0.0f}, Vec3{0.5f, 4.0f, 16.0f});
    box("Wall right", yard, brick, Vec3{8.0f, 2.0f, 0.0f}, Vec3{0.5f, 4.0f, 16.0f});
    box("Wall front", yard, brick, Vec3{0.0f, 2.0f, 8.0f}, Vec3{16.5f, 4.0f, 0.5f});
    // The back wall in three pieces, around the tunnel's mouth: 4 m wide, 3 high.
    box("Wall back left", yard, brick, Vec3{-5.0f, 2.0f, -8.0f}, Vec3{6.0f, 4.0f, 0.5f});
    box("Wall back right", yard, brick, Vec3{5.0f, 2.0f, -8.0f}, Vec3{6.0f, 4.0f, 0.5f});
    box("Lintel", yard, brick, Vec3{0.0f, 3.5f, -8.0f}, Vec3{4.0f, 1.0f, 0.5f});
    box("Sign", yard, signGlow, Vec3{0.0f, 3.5f, -7.72f}, Vec3{1.4f, 0.3f, 0.04f});

    // Still water: a mirror lying on the ground, and dark enough that what it
    // shows is the sky and the brick rather than its own colour.
    box("Puddle", yard, puddle, Vec3{1.2f, 0.005f, -4.2f}, Vec3{3.2f, 0.01f, 2.2f});

    box("Crate", yard, wood, Vec3{-5.2f, 0.5f, -5.6f}, Vec3{1.0f, 1.0f, 1.0f});
    box("Crate small", yard, wood, Vec3{-4.1f, 0.35f, -6.1f}, Vec3{0.7f, 0.7f, 0.7f});
    box("Crate top", yard, wood, Vec3{-5.1f, 1.3f, -5.5f}, Vec3{0.6f, 0.6f, 0.6f});
    box("Block", yard, stone, Vec3{4.8f, 0.6f, 2.5f}, Vec3{1.2f, 1.2f, 1.2f});
    box("Bench", yard, stone, Vec3{-6.6f, 0.25f, 2.0f}, Vec3{1.0f, 0.5f, 3.5f});

    // --- the tunnel ---------------------------------------------------------
    // 4 m wide and 3 high inside, 21.5 m long, closed above: from here on the
    // sun reaches nothing, so the picture is lamps, their bounces and their
    // reflections.
    const NodeId tunnel = scene.createNode("Tunnel");

    // A centimetre proud of the yard, so the two floors never share a plane.
    box("Floor", tunnel, wetAsphalt, Vec3{0.0f, -0.04f, -19.0f}, Vec3{4.0f, 0.1f, 22.5f});
    box("Side left", tunnel, tiles, Vec3{-2.25f, 1.5f, -19.0f}, Vec3{0.5f, 3.0f, 21.5f});
    box("Side right", tunnel, tiles, Vec3{2.25f, 1.5f, -19.0f}, Vec3{0.5f, 3.0f, 21.5f});
    box("Roof", tunnel, concrete, Vec3{0.0f, 3.25f, -19.0f}, Vec3{5.0f, 0.5f, 21.5f});

    // Steel panels down the right wall at eye height: every lamp shows twice,
    // once on the floor and once beside you.
    for (int i = 0; i < 4; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Panel %d", i + 1);
        box(name, tunnel, steel, Vec3{1.97f, 1.5f, -11.5f - 5.0f * static_cast<f32>(i)}, Vec3{0.06f, 1.2f, 3.6f});
    }

    // Glowing strips along the foot of both walls. They are not lights - no
    // light node, no shadows of their own - so the blue they put on the walls
    // and in the floor arrives only as bounced light.
    box("Strip left", tunnel, stripGlow, Vec3{-1.96f, 0.06f, -19.0f}, Vec3{0.05f, 0.04f, 21.0f});
    box("Strip right", tunnel, stripGlow, Vec3{1.96f, 0.06f, -19.0f}, Vec3{0.05f, 0.04f, 21.0f});

    // Four ceiling lamps, five metres apart. The shade is a box that glows; the
    // light itself hangs just under it, so the shade does not shadow it.
    for (int i = 0; i < 4; ++i) {
        const f32 z = -11.0f - 5.0f * static_cast<f32>(i);
        char name[32];
        std::snprintf(name, sizeof(name), "Lamp %d", i + 1);
        const NodeId fixture = box(name, tunnel, lampGlow, Vec3{0.0f, 2.96f, z}, Vec3{0.7f, 0.08f, 0.35f});
        scene.node(fixture).name = std::string(name) + " shade";
        lamp(name, tunnel, Vec3{0.0f, 2.75f, z}, Vec3{1.0f, 0.66f, 0.34f}, 26.0f, 7.5f);
    }

    // Something for the lamps to throw a shadow across the whole width with.
    box("Barrier", tunnel, stone, Vec3{-0.9f, 0.45f, -18.5f}, Vec3{1.6f, 0.9f, 0.3f});

    // --- the hall -----------------------------------------------------------
    // 12 x 12 m and 5 high, with a 3 x 3 m skylight: the sun comes back here as
    // a single shaft, after the tunnel has had none.
    const NodeId hall = scene.createNode("Hall");

    box("Floor", hall, white, Vec3{0.0f, -0.04f, -36.125f}, Vec3{12.0f, 0.1f, 11.75f});
    box("Wall red", hall, red, Vec3{-6.25f, 2.5f, -36.125f}, Vec3{0.5f, 5.0f, 11.75f});
    box("Wall green", hall, green, Vec3{6.25f, 2.5f, -36.125f}, Vec3{0.5f, 5.0f, 11.75f});
    box("Wall back", hall, plaster, Vec3{0.0f, 2.5f, -42.25f}, Vec3{13.0f, 5.0f, 0.5f});
    box("Wall front left", hall, plaster, Vec3{-4.25f, 2.5f, -30.0f}, Vec3{4.5f, 5.0f, 0.5f});
    box("Wall front right", hall, plaster, Vec3{4.25f, 2.5f, -30.0f}, Vec3{4.5f, 5.0f, 0.5f});
    box("Wall over the door", hall, plaster, Vec3{0.0f, 4.0f, -30.0f}, Vec3{4.0f, 2.0f, 0.5f});

    // The roof in four pieces around the skylight (x -1.5..1.5, z -37.5..-34.5).
    box("Roof front", hall, concrete, Vec3{0.0f, 5.25f, -32.125f}, Vec3{13.0f, 0.5f, 4.75f});
    box("Roof back", hall, concrete, Vec3{0.0f, 5.25f, -40.0f}, Vec3{13.0f, 0.5f, 5.0f});
    box("Roof left", hall, concrete, Vec3{-4.0f, 5.25f, -36.0f}, Vec3{5.0f, 0.5f, 3.0f});
    box("Roof right", hall, concrete, Vec3{4.0f, 5.25f, -36.0f}, Vec3{5.0f, 0.5f, 3.0f});

    // A mirror across the back wall, so the balls are seen from both sides and
    // the tunnel's mouth appears behind them.
    box("Mirror", hall, mirror, Vec3{0.0f, 2.4f, -41.95f}, Vec3{6.0f, 2.8f, 0.08f});

    // The golds on plinths in a row, smoothest on the left.
    for (usize i = 0; i < golds.size(); ++i) {
        const f32 x = -4.0f + 2.0f * static_cast<f32>(i);
        char name[32];
        std::snprintf(name, sizeof(name), "Plinth %zu", i + 1);
        box(name, hall, stone, Vec3{x, 0.4f, -39.0f}, Vec3{0.8f, 0.8f, 0.8f});

        std::snprintf(name, sizeof(name), "Ball %zu", i + 1);
        const NodeId id = scene.createNode(name, hall);
        scene.node(id).mesh = ball;
        scene.node(id).material = golds[i];
        scene.node(id).transform.position = Vec3{x, 1.31f, -39.0f};
    }

    // A spot over the balls and a dim lamp by the door, so the hall reads
    // whatever the sun is doing.
    const NodeId spot = lamp("Spot", hall, Vec3{0.0f, 4.6f, -38.0f}, Vec3{1.0f, 0.95f, 0.88f}, 90.0f, 9.0f);
    scene.node(spot).light->type = LightType::Spot;
    scene.node(spot).light->outerConeDegrees = 48.0f;
    scene.node(spot).light->innerConeDegrees = 30.0f;
    // Nodes look down -Z; a quarter turn about X points this one at the floor.
    scene.node(spot).transform.rotation = fromAxisAngle(Vec3{1.0f, 0.0f, 0.0f}, radians(-90.0f));

    lamp("Door lamp", hall, Vec3{-4.5f, 3.4f, -31.0f}, Vec3{1.0f, 0.7f, 0.42f}, 30.0f, 8.0f);

    // In the yard, looking at the tunnel's mouth.
    renderer.camera().position = Vec3{3.5f, 2.0f, 6.5f};
    renderer.camera().yaw = -104.0f;
    renderer.camera().pitch = -4.0f;
}

constexpr std::array kTemplates{
    SceneTemplate{"blank", "Blank", "A floor under the sky and nothing else. Start from scratch.", &buildBlank},
    SceneTemplate{"starter", "Starter",
                  "A walled courtyard with brick, grass, a wooden deck, a polished pillar, a lamp and the helmet. "
                  "Shows a little of everything.",
                  &buildStarter},
    SceneTemplate{"raytracing", "Ray Tracing",
                  "A walk in three stages: a sunlit yard with a puddle, a lamp-lit tunnel with a wet floor and "
                  "steel panels, and a hall with coloured walls, a mirror and gold balls from polished to rough. "
                  "Each stage shows something the rays do.",
                  &buildRayTracing},
};

} // namespace

std::span<const SceneTemplate> sceneTemplates() { return kTemplates; }

usize findSceneTemplate(std::string_view id) {
    for (usize i = 0; i < kTemplates.size(); ++i) {
        if (id == kTemplates[i].id) {
            return i;
        }
    }
    return kDefaultSceneTemplate;
}

void buildSceneTemplate(Renderer& renderer, usize index) {
    const SceneTemplate& chosen = kTemplates[index < kTemplates.size() ? index : kDefaultSceneTemplate];

    // A template decides the whole look, so it starts from the default sky
    // rather than whatever the previous scene left behind.
    renderer.environment() = Environment{};
    chosen.build(renderer);

    FUMAR_INFO("{} scene: {} nodes, {} meshes, {} materials", chosen.id, renderer.scene().nodeCount(),
               renderer.resources().meshCount(), renderer.resources().materialCount());
}

} // namespace fumar
