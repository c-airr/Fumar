#include "cpu_workload.hpp"

#include "fumar/render/profiler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>

namespace fumar {

namespace {

using Clock = std::chrono::steady_clock;

/// The most triangles a subdivided copy may have. A mesh whose edges are long
/// compared with the vertex spacing is halved fewer times than the spacing
/// asks for rather than growing past this.
constexpr usize kMaxDenseTriangles = 1u << 18;

/// Dropped from the caches after this many frames without being used.
constexpr u64 kEvictAfterFrames = 600;

/// Same numbers as shaders/raytrace.glsl: a ray starts this far off the
/// surface, and a lamp's shadow ray stops just short of the lamp.
constexpr f32 kSurfaceOffset = 0.01f;
constexpr f32 kIndirectClamp = 10.0f;

// --- sampling, as the shaders do it -----------------------------------------
// The CPU reproduces the GPU's sample patterns so the two pictures converge to
// the same thing. See raytrace.glsl for why each of these is shaped as it is.

f32 fract(f32 v) { return v - std::floor(v); }

f32 hash13(Vec3 position) {
    Vec3 p{fract(position.x * 137.13f * 0.1031f), fract(position.y * 137.13f * 0.1031f),
           fract(position.z * 137.13f * 0.1031f)};
    const f32 d = dot(p, Vec3{p.z, p.y, p.x} + Vec3{31.32f, 31.32f, 31.32f});
    p += Vec3{d, d, d};
    return fract((p.x + p.y) * p.z);
}

Vec2 vogelDisc(u32 index, u32 count, f32 rotation) {
    const f32 radius = std::sqrt((static_cast<f32>(index) + 0.5f) / static_cast<f32>(count));
    const f32 theta = static_cast<f32>(index) * 2.39996323f + rotation;
    return Vec2{std::cos(theta), std::sin(theta)} * radius;
}

void orthonormalBasis(Vec3 n, Vec3& t, Vec3& b) {
    t = normalize(std::abs(n.y) < 0.99f ? cross(Vec3{0.0f, 1.0f, 0.0f}, n) : cross(Vec3{1.0f, 0.0f, 0.0f}, n));
    b = cross(n, t);
}

/// Which point of the spiral the n-th sample takes. The spiral is ordered
/// from the centre outwards, so taking its points in order would spend the
/// first frames on the middle of the sun's disc or straight up from the
/// surface - a biased answer until every point has been used. Reversing the
/// bits of the count visits the inside, the outside and everything between
/// in alternation, so the average is about right after any number of samples.
u32 sampleIndex(u32 n) {
    u32 r = 0;
    for (u32 bit = 0; bit < 6; ++bit) {
        r |= ((n >> bit) & 1u) << (5 - bit);
    }
    return r % CpuWorkload::kMaxSamples;
}

Vec3 skyIrradiance(const CpuLightInputs& in, Vec3 normal) {
    const f32 up = normal.y * 0.5f + 0.5f;
    const Vec3 sky = lerp(in.skyHorizon, in.skyZenith, 0.35f);
    return lerp(in.ground, sky, up) * in.skyIntensity;
}

f32 luminance(Vec3 c) { return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f; }

/// Slab test, as in the RayCaster: whether the ray passes through the box
/// before `limit`.
bool hitsBox(Vec3 low, Vec3 high, Vec3 origin, Vec3 direction, f32 limit) {
    f32 tNear = 0.0f;
    f32 tFar = limit;
    for (usize axis = 0; axis < 3; ++axis) {
        const f32 inverse =
            std::abs(direction[axis]) > 1e-20f ? 1.0f / direction[axis] : std::copysign(1e30f, direction[axis]);
        f32 t0 = (low[axis] - origin[axis]) * inverse;
        f32 t1 = (high[axis] - origin[axis]) * inverse;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tNear = std::max(tNear, t0);
        tFar = std::min(tFar, t1);
        if (tNear > tFar) {
            return false;
        }
    }
    return true;
}

/// FNV-1a over raw bytes: whether anything the lighting depends on changed.
struct Hasher {
    u64 value = 1469598103934665603ull;
    void bytes(const void* data, usize size) {
        const auto* p = static_cast<const u8*>(data);
        for (usize i = 0; i < size; ++i) {
            value = (value ^ p[i]) * 1099511628211ull;
        }
    }
    template <typename T>
    void add(const T& v) {
        bytes(&v, sizeof(T));
    }
};

Vertex midpoint(const Vertex& a, const Vertex& b) {
    // (a + b) * 0.5 gives the same bits whichever way round the two are taken,
    // so two triangles that share an edge - even through different vertex
    // indices, as at a hard edge - put the new vertex at exactly the same
    // place. No crack opens along the seam.
    Vertex m;
    m.position = (a.position + b.position) * 0.5f;
    const Vec3 n = a.normal + b.normal;
    m.normal = lengthSquared(n) > 1e-12f ? normalize(n) : a.normal;
    m.uv = (a.uv + b.uv) * 0.5f;
    const Vec3 t = xyz(a.tangent) + xyz(b.tangent);
    const Vec3 tangent = lengthSquared(t) > 1e-12f ? normalize(t) : xyz(a.tangent);
    m.tangent = Vec4{tangent.x, tangent.y, tangent.z, a.tangent.w};
    return m;
}

} // namespace

// --- private types ---------------------------------------------------------------

/// A mesh with every triangle split into four, `levels` times.
struct CpuWorkload::DenseMesh {
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    u64 lastUsed = 0;
};

/// A mesh's triangles in a tree, in its own space, for rays to walk.
struct CpuWorkload::Caster {
    geometry::RayCaster tree;
    std::vector<Vec3> positions;
    std::vector<u32> indices;

    /// The longest triangle edge, in the mesh's own units: how many times its
    /// subdivided copy needs halving depends on it.
    f32 longestEdge = 0.0f;
    u64 lastUsed = 0;
};

/// Running averages, one per vertex of a node's subdivided copy.
struct CpuWorkload::Accumulator {
    struct Sample {
        f32 sun = 1.0f;
        f32 sky = 1.0f;
        f32 occlusion = 1.0f;
        std::array<f32, CpuLightInputs::kMaxLamps> lamps{1.0f, 1.0f, 1.0f, 1.0f};
        Vec3 bounce{};
        u32 count = 0;
    };
    u64 denseKey = 0;
    std::vector<Sample> samples;
    u64 lastUsed = 0;
};

/// One object in the CPU's picture of the scene.
struct CpuWorkload::Instance {
    Mat4 world;
    Mat4 inverseWorld;
    Mat4 normalMatrix;
    Vec3 low;
    Vec3 high;
    const Caster* caster = nullptr;
    Vec3 albedo;
    Vec3 emissive;
};

/// One object being drawn this frame, and where its vertices went.
struct CpuWorkload::Visible {
    Accumulator* accumulator = nullptr;
    usize firstVertex = 0;
    usize vertexCount = 0;
};

// --- the scene, as the rays see it ---------------------------------------------

namespace {

struct SceneHit {
    f32 t = 0.0f;
    Vec3 normal;
    u32 instance = 0;
};

} // namespace

CpuWorkload::CpuWorkload() = default;
CpuWorkload::~CpuWorkload() = default;

void CpuWorkload::clear() {
    m_dense.clear();
    m_casters.clear();
    m_accumulators.clear();
    m_instances.clear();
    m_visible.clear();
    m_signature = 0;
    m_converged = false;
}

const CpuWorkload::DenseMesh& CpuWorkload::denseMesh(const Mesh& mesh, u32 levels) {
    const u64 key = (mesh.serial() << 4) | levels;
    auto found = m_dense.find(key);
    if (found != m_dense.end()) {
        found->second->lastUsed = m_frame;
        return *found->second;
    }

    auto dense = std::make_unique<DenseMesh>();
    dense->vertices.assign(mesh.vertices().begin(), mesh.vertices().end());
    dense->indices.assign(mesh.indices().begin(), mesh.indices().end());

    // Each level splits every edge at its midpoint and every triangle into
    // four. The midpoint of an edge is made once and shared by both triangles
    // beside it, found through a map from the edge's two vertex indices.
    for (u32 level = 0; level < levels; ++level) {
        std::unordered_map<u64, u32> midpoints;
        midpoints.reserve(dense->indices.size());
        const auto split = [&](u32 a, u32 b) {
            const u64 edge = (static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b);
            auto [it, inserted] = midpoints.try_emplace(edge, 0);
            if (inserted) {
                const Vertex m = midpoint(dense->vertices[a], dense->vertices[b]);
                it->second = static_cast<u32>(dense->vertices.size());
                dense->vertices.push_back(m);
            }
            return it->second;
        };

        std::vector<u32> next;
        next.reserve(dense->indices.size() * 4);
        for (usize i = 0; i + 2 < dense->indices.size(); i += 3) {
            const u32 a = dense->indices[i];
            const u32 b = dense->indices[i + 1];
            const u32 c = dense->indices[i + 2];
            const u32 ab = split(a, b);
            const u32 bc = split(b, c);
            const u32 ca = split(c, a);
            // Same winding as the parent, all four.
            next.insert(next.end(), {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca});
        }
        dense->indices = std::move(next);
    }

    dense->lastUsed = m_frame;
    return *m_dense.emplace(key, std::move(dense)).first->second;
}

const CpuWorkload::Caster& CpuWorkload::caster(const Mesh& mesh) {
    auto found = m_casters.find(mesh.serial());
    if (found != m_casters.end()) {
        found->second->lastUsed = m_frame;
        return *found->second;
    }

    // Built from the ORIGINAL triangles, not the subdivided ones: splitting a
    // flat triangle into four changes nothing a ray can hit, and the tree is
    // four times smaller for every level not taken.
    auto caster = std::make_unique<Caster>();
    caster->positions.reserve(mesh.vertices().size());
    for (const Vertex& v : mesh.vertices()) {
        caster->positions.push_back(v.position);
    }
    caster->indices.assign(mesh.indices().begin(), mesh.indices().end());
    caster->tree = geometry::RayCaster(caster->positions, caster->indices);
    f32 longest = 0.0f;
    for (usize i = 0; i + 2 < caster->indices.size(); i += 3) {
        const Vec3 a = caster->positions[caster->indices[i]];
        const Vec3 b = caster->positions[caster->indices[i + 1]];
        const Vec3 c = caster->positions[caster->indices[i + 2]];
        longest = std::max({longest, lengthSquared(b - a), lengthSquared(c - b), lengthSquared(a - c)});
    }
    caster->longestEdge = std::sqrt(longest);
    caster->lastUsed = m_frame;
    return *m_casters.emplace(mesh.serial(), std::move(caster)).first->second;
}

// --- prepare: cull, subdivide, transform ---------------------------------------------

void CpuWorkload::prepare(std::span<const CpuDrawable> drawables, const Mat4& viewProjection,
                          FrameProfiler& profiler) {
    ++m_frame;
    m_instances.clear();
    m_visible.clear();
    m_draws.clear();
    m_vertices.clear();
    m_indices.clear();

    struct Pending {
        const CpuDrawable* drawable;
        const DenseMesh* dense;
        Accumulator* accumulator;
    };
    std::vector<Pending> pending;
    pending.reserve(drawables.size());

    Hasher visibleSet;
    {
        const FrameProfiler::Scope scope = profiler.cpu(ProfileStage::Culling);
        for (const CpuDrawable& drawable : drawables) {
            const Mesh& mesh = *drawable.mesh;
            const Bounds& bounds = mesh.bounds();

            // The box's eight corners in world space and in clip space. In
            // world space they bound the object for the rays; in clip space,
            // the object is off screen when every corner is beyond the same
            // edge of the view.
            Vec3 low{1e30f, 1e30f, 1e30f};
            Vec3 high{-1e30f, -1e30f, -1e30f};
            u32 left = 0;
            u32 right = 0;
            u32 below = 0;
            u32 above = 0;
            u32 behind = 0;
            for (u32 corner = 0; corner < 8; ++corner) {
                const Vec3 local{(corner & 1) ? bounds.max.x : bounds.min.x,
                                 (corner & 2) ? bounds.max.y : bounds.min.y,
                                 (corner & 4) ? bounds.max.z : bounds.min.z};
                const Vec3 world = xyz(drawable.world * Vec4{local.x, local.y, local.z, 1.0f});
                low = min(low, world);
                high = max(high, world);
                const Vec4 clip = viewProjection * Vec4{world.x, world.y, world.z, 1.0f};
                left += clip.x < -clip.w ? 1 : 0;
                right += clip.x > clip.w ? 1 : 0;
                below += clip.y < -clip.w ? 1 : 0;
                above += clip.y > clip.w ? 1 : 0;
                behind += clip.w <= 0.0f ? 1 : 0;
            }

            // Every object casts shadows, on screen or not: a building behind
            // the camera still darkens the street in front of it.
            const Caster& meshCaster = caster(mesh);
            const Mat4 inverseWorld = inverse(drawable.world);
            m_instances.push_back(Instance{
                .world = drawable.world,
                .inverseWorld = inverseWorld,
                .normalMatrix = transpose(inverseWorld),
                .low = low,
                .high = high,
                .caster = &meshCaster,
                .albedo = drawable.albedo,
                .emissive = drawable.emissive,
            });

            if (left == 8 || right == 8 || below == 8 || above == 8 || behind == 8) {
                continue;
            }

            // How many times to halve: until the longest edge, in WORLD
            // metres, is under the spacing - or the copy would grow too big.
            const f32 scale = std::max({length(xyz(drawable.world.columns[0])), length(xyz(drawable.world.columns[1])),
                                        length(xyz(drawable.world.columns[2]))});
            f32 longest = meshCaster.longestEdge * scale;
            const f32 spacing = std::max(settings.vertexSpacing, 0.01f);
            u32 levels = 0;
            usize triangles = mesh.indices().size() / 3;
            while (levels < 12 && longest > spacing && triangles * 4 <= kMaxDenseTriangles) {
                longest *= 0.5f;
                triangles *= 4;
                ++levels;
            }

            const DenseMesh& dense = denseMesh(mesh, levels);
            const u64 denseKey = (mesh.serial() << 4) | levels;
            std::unique_ptr<Accumulator>& accumulator = m_accumulators[drawable.id];
            if (!accumulator || accumulator->denseKey != denseKey) {
                accumulator = std::make_unique<Accumulator>();
                accumulator->denseKey = denseKey;
                accumulator->samples.resize(dense.vertices.size());
            }
            accumulator->lastUsed = m_frame;
            visibleSet.add(drawable.id);
            visibleSet.add(denseKey);
            pending.push_back(Pending{&drawable, &dense, accumulator.get()});
        }
    }

    // Something came into view, or changed shape: it may have samples to take.
    if (visibleSet.value != m_visibleSignature) {
        m_visibleSignature = visibleSet.value;
        m_converged = false;
    }

    // Offsets first, on one thread; then every vertex of every object at once.
    usize vertexCount = 0;
    usize indexCount = 0;
    for (const Pending& p : pending) {
        m_visible.push_back(Visible{
            .accumulator = p.accumulator,
            .firstVertex = vertexCount,
            .vertexCount = p.dense->vertices.size(),
        });
        m_draws.push_back(Draw{
            .firstIndex = static_cast<u32>(indexCount),
            .indexCount = static_cast<u32>(p.dense->indices.size()),
            .vertexOffset = static_cast<i32>(vertexCount),
            .materialIndex = p.drawable->materialIndex,
            .highlight = p.drawable->highlight,
        });
        vertexCount += p.dense->vertices.size();
        indexCount += p.dense->indices.size();
    }

    const FrameProfiler::Scope scope = profiler.cpu(ProfileStage::Transform);
    m_vertices.resize(vertexCount);
    m_indices.resize(indexCount);
    for (usize i = 0; i < pending.size(); ++i) {
        std::memcpy(m_indices.data() + m_draws[i].firstIndex, pending[i].dense->indices.data(),
                    pending[i].dense->indices.size() * sizeof(u32));
    }

    // Cut into pieces of a few thousand vertices, so one big object is spread
    // over every core rather than given to one.
    struct Piece {
        usize object;
        usize begin;
        usize end;
    };
    std::vector<Piece> pieces;
    constexpr usize kPiece = 4096;
    for (usize i = 0; i < pending.size(); ++i) {
        const usize count = pending[i].dense->vertices.size();
        for (usize begin = 0; begin < count; begin += kPiece) {
            pieces.push_back(Piece{i, begin, std::min(begin + kPiece, count)});
        }
    }

    std::vector<Mat4> normalMatrices(pending.size());
    for (usize i = 0; i < pending.size(); ++i) {
        // Inverse-transpose takes normals along with a non-uniform scale.
        normalMatrices[i] = transpose(inverse(pending[i].drawable->world));
    }

    m_pool.parallelFor(pieces.size(), 1, [&](usize first, usize last) {
        for (usize p = first; p < last; ++p) {
            const Piece& piece = pieces[p];
            const Mat4& world = pending[piece.object].drawable->world;
            const Mat4& normalMatrix = normalMatrices[piece.object];
            const std::vector<Vertex>& source = pending[piece.object].dense->vertices;
            Vertex* out = m_vertices.data() + m_visible[piece.object].firstVertex;
            for (usize v = piece.begin; v < piece.end; ++v) {
                const Vertex& in = source[v];
                Vertex& o = out[v];
                o.position = xyz(world * Vec4{in.position.x, in.position.y, in.position.z, 1.0f});
                o.normal = xyz(normalMatrix * Vec4{in.normal.x, in.normal.y, in.normal.z, 0.0f});
                const Vec3 tangent = xyz(world * Vec4{in.tangent.x, in.tangent.y, in.tangent.z, 0.0f});
                o.tangent = Vec4{tangent.x, tangent.y, tangent.z, in.tangent.w};
                o.uv = in.uv;
            }
        }
    });

    // Whatever has not been drawn for a while is let go.
    std::erase_if(m_dense, [&](const auto& entry) { return m_frame - entry.second->lastUsed > kEvictAfterFrames; });
    std::erase_if(m_casters, [&](const auto& entry) { return m_frame - entry.second->lastUsed > kEvictAfterFrames; });
    std::erase_if(m_accumulators,
                  [&](const auto& entry) { return m_frame - entry.second->lastUsed > kEvictAfterFrames; });
}

// --- light: the rays -------------------------------------------------------------------

void CpuWorkload::light(FrameProfiler& profiler) {
    const FrameProfiler::Scope scope = profiler.cpu(ProfileStage::Lighting);

    // Anything that changes what a vertex sees starts the averages over. The
    // old values stay on screen until each vertex's first new sample replaces
    // them, so a moving object does not flash.
    Hasher signature;
    for (const Instance& instance : m_instances) {
        signature.add(instance.world);
        signature.add(instance.caster);
        signature.add(instance.albedo);
        signature.add(instance.emissive);
    }
    signature.add(inputs.sunDirection);
    signature.add(inputs.sunAngularRadius);
    signature.add(inputs.sunRadiance);
    signature.add(inputs.skyZenith);
    signature.add(inputs.skyHorizon);
    signature.add(inputs.ground);
    signature.add(inputs.skyIntensity);
    signature.add(inputs.occlusionRadius);
    signature.add(inputs.lampCount);
    for (u32 i = 0; i < inputs.lampCount; ++i) {
        signature.add(inputs.lamps[i].position);
        signature.add(inputs.lamps[i].range);
        signature.add(inputs.lamps[i].sourceRadius);
        signature.add(inputs.lamps[i].castsShadow);
    }
    if (signature.value != m_signature) {
        m_signature = signature.value;
        m_converged = false;
        for (auto& [id, accumulator] : m_accumulators) {
            for (Accumulator::Sample& sample : accumulator->samples) {
                sample.count = 0;
            }
        }
    }

    // --- the scene's two questions --------------------------------------------
    // Both test the ray against each object's world box, then carry it into
    // the object's own space - where its tree was built - by the inverse of
    // its world matrix. The ray's parameter t means the same distance in both
    // spaces, because the direction is carried along by the same matrix.
    const auto occluded = [&](Vec3 origin, Vec3 direction, f32 tMax) {
        for (const Instance& instance : m_instances) {
            if (!hitsBox(instance.low, instance.high, origin, direction, tMax)) {
                continue;
            }
            const Vec3 o = xyz(instance.inverseWorld * Vec4{origin.x, origin.y, origin.z, 1.0f});
            const Vec3 d = xyz(instance.inverseWorld * Vec4{direction.x, direction.y, direction.z, 0.0f});
            if (instance.caster->tree.occluded(o, d, tMax)) {
                return true;
            }
        }
        return false;
    };

    const auto closest = [&](Vec3 origin, Vec3 direction, f32 tMax) -> std::optional<SceneHit> {
        std::optional<SceneHit> best;
        f32 limit = tMax;
        for (u32 i = 0; i < m_instances.size(); ++i) {
            const Instance& instance = m_instances[i];
            if (!hitsBox(instance.low, instance.high, origin, direction, limit)) {
                continue;
            }
            const Vec3 o = xyz(instance.inverseWorld * Vec4{origin.x, origin.y, origin.z, 1.0f});
            const Vec3 d = xyz(instance.inverseWorld * Vec4{direction.x, direction.y, direction.z, 0.0f});
            const auto hit = instance.caster->tree.closest(o, d, limit);
            if (!hit) {
                continue;
            }
            limit = hit->t;
            const Caster& c = *instance.caster;
            const Vec3 a = c.positions[c.indices[hit->face * 3]];
            const Vec3 b = c.positions[c.indices[hit->face * 3 + 1]];
            const Vec3 e = c.positions[c.indices[hit->face * 3 + 2]];
            const Vec3 local = cross(b - a, e - a);
            best = SceneHit{.t = hit->t, .normal = xyz(instance.normalMatrix * Vec4{local.x, local.y, local.z, 0.0f}), .instance = i};
        }
        return best;
    };

    const Vec3 sun = normalize(inputs.sunDirection);
    Vec3 sunTangent;
    Vec3 sunBitangent;
    orthonormalBasis(sun, sunTangent, sunBitangent);
    const f32 sunSpread = std::tan(std::max(inputs.sunAngularRadius, 0.0001f));

    // One more sample for one vertex: the sun, each lamp, and one direction
    // over the hemisphere - which answers sky visibility, occlusion and
    // bounced light at once, as indirectBounce does on the GPU.
    const auto trace = [&](const Vertex& vertex, Accumulator::Sample& sample) {
        const Vec3 normal = lengthSquared(vertex.normal) > 1e-12f ? normalize(vertex.normal) : Vec3{0.0f, 1.0f, 0.0f};
        const Vec3 origin = vertex.position + normal * kSurfaceOffset;
        const u32 index = sampleIndex(sample.count);
        const f32 rotation = hash13(vertex.position) * 6.2831853f;

        f32 sunVisible = 1.0f;
        if (dot(normal, sun) > 0.0f) {
            const Vec2 offset = vogelDisc(index, kMaxSamples, rotation) * sunSpread;
            const Vec3 direction = normalize(sun + sunTangent * offset.x + sunBitangent * offset.y);
            sunVisible = occluded(origin, direction, 2000.0f) ? 0.0f : 1.0f;
        }

        std::array<f32, CpuLightInputs::kMaxLamps> lamps{1.0f, 1.0f, 1.0f, 1.0f};
        for (u32 l = 0; l < inputs.lampCount; ++l) {
            const CpuLightInputs::Lamp& lamp = inputs.lamps[l];
            const Vec3 toLamp = lamp.position - origin;
            const f32 distance = length(toLamp);
            if (!lamp.castsShadow || distance > lamp.range || distance < 1e-4f || dot(normal, toLamp) <= 0.0f) {
                continue;
            }
            Vec3 t;
            Vec3 b;
            orthonormalBasis(toLamp / distance, t, b);
            const Vec2 offset = vogelDisc(index, kMaxSamples, rotation + 1.7f) * lamp.sourceRadius;
            const Vec3 target = lamp.position + t * offset.x + b * offset.y;
            lamps[l] = occluded(origin, target - origin, 0.999f) ? 0.0f : 1.0f;
        }

        Vec3 t;
        Vec3 b;
        orthonormalBasis(normal, t, b);
        const Vec2 disc = vogelDisc(index, kMaxSamples, rotation + 3.1f);
        const f32 height = std::sqrt(std::max(1.0f - dot(disc, disc), 0.0f));
        const Vec3 direction = normalize(t * disc.x + b * disc.y + normal * height);

        f32 sky = 1.0f;
        f32 occlusion = 1.0f;
        Vec3 bounce{};
        if (const auto hit = closest(origin, direction, 200.0f)) {
            sky = 0.0f;
            occlusion = hit->t > inputs.occlusionRadius ? 1.0f : 0.0f;

            // What the ray found, lit the way traceScene lights it: the sky
            // from above, the sun if the spot can see it, and its own glow.
            Vec3 n = normalize(hit->normal);
            if (dot(n, direction) > 0.0f) {
                n = -n;
            }
            const Instance& instance = m_instances[hit->instance];
            const Vec3 point = origin + direction * hit->t;
            Vec3 lit = hadamard(skyIrradiance(inputs, n), instance.albedo);
            const f32 nDotL = dot(n, sun);
            if (nDotL > 0.0f && !occluded(point + n * kSurfaceOffset, sun, 2000.0f)) {
                lit += hadamard(instance.albedo, inputs.sunRadiance) * (nDotL / kPi);
            }
            lit += instance.emissive;
            const f32 brightness = luminance(lit);
            bounce = brightness > kIndirectClamp ? lit * (kIndirectClamp / brightness) : lit;
        }

        // A running mean: the first sample after a reset replaces the old
        // value outright, every later one moves it by 1/n.
        const f32 w = 1.0f / static_cast<f32>(sample.count + 1);
        sample.sun += (sunVisible - sample.sun) * w;
        sample.sky += (sky - sample.sky) * w;
        sample.occlusion += (occlusion - sample.occlusion) * w;
        for (u32 l = 0; l < CpuLightInputs::kMaxLamps; ++l) {
            sample.lamps[l] += (lamps[l] - sample.lamps[l]) * w;
        }
        sample.bounce += (bounce - sample.bounce) * w;
        ++sample.count;
    };

    // --- spend the budget -----------------------------------------------------
    // A sweep goes over every visible vertex in turn, one sample each, in
    // batches sized from how long the last one took; it stops when the time is
    // up. A sweep that found nothing left to do means everything has all its
    // samples, and the CPU rests until something changes.
    const usize total = m_vertices.size();
    const auto deadline = Clock::now() + std::chrono::duration<f64, std::milli>(std::max(settings.budgetMs, 0.0f));
    while (total > 0 && !m_converged) {
        const auto now = Clock::now();
        if (now >= deadline) {
            break;
        }
        m_cursor %= total;
        const usize count = std::min(std::max<usize>(m_batch, 256), total);
        std::atomic<usize> worked{0};
        m_pool.parallelFor(count, 64, [&](usize begin, usize end) {
            usize done = 0;
            usize object = 0;
            for (usize i = begin; i < end; ++i) {
                const usize v = (m_cursor + i) % total;
                // The visible list is in vertex order: find the object once,
                // then walk forward.
                if (object >= m_visible.size() || v < m_visible[object].firstVertex ||
                    v >= m_visible[object].firstVertex + m_visible[object].vertexCount) {
                    object = static_cast<usize>(
                        std::upper_bound(m_visible.begin(), m_visible.end(), v,
                                         [](usize value, const Visible& vis) { return value < vis.firstVertex; }) -
                        m_visible.begin() - 1);
                }
                Accumulator::Sample& sample = m_visible[object].accumulator->samples[v - m_visible[object].firstVertex];
                if (sample.count < kMaxSamples) {
                    trace(m_vertices[v], sample);
                    ++done;
                }
            }
            worked.fetch_add(done, std::memory_order_relaxed);
        });

        const std::chrono::duration<f64, std::milli> spent = Clock::now() - now;
        const std::chrono::duration<f64, std::milli> left = deadline - Clock::now();
        if (worked.load() > 0 && spent.count() > 0.0) {
            // Aim each batch at a quarter of what is left, so the last one
            // does not run far past the deadline.
            const f64 perVertex = spent.count() / static_cast<f64>(count);
            m_batch = static_cast<usize>(std::clamp(left.count() * 0.25 / perVertex, 256.0, 1.0e6));
        } else {
            m_batch = count * 2;
        }

        m_sweepWork += worked.load();
        m_cursor += count;
        if (m_cursor >= total) {
            if (m_sweepWork == 0) {
                m_converged = true;
            }
            m_sweepWork = 0;
        }
    }

    // --- hand the answers over -----------------------------------------------
    m_light.resize(total);
    u32 least = kMaxSamples;
    for (const Visible& visible : m_visible) {
        for (usize i = 0; i < visible.vertexCount; ++i) {
            const Accumulator::Sample& s = visible.accumulator->samples[i];
            least = std::min(least, s.count);
            m_light[visible.firstVertex + i] = CpuLightVertex{
                .visibility = Vec4{s.sun, s.sky, s.occlusion, 0.0f},
                .lamps = Vec4{s.lamps[0], s.lamps[1], s.lamps[2], s.lamps[3]},
                .bounce = Vec4{s.bounce.x, s.bounce.y, s.bounce.z, 0.0f},
            };
        }
    }
    m_minSamples = m_visible.empty() ? kMaxSamples : least;
}

} // namespace fumar
