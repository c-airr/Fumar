#pragma once

// The Cpu workload: everything that answers "what can this point see" moved
// off the GPU and onto every core of the CPU.
//
// The GPU, in the Balanced and Gpu workloads, asks that question per PIXEL,
// with ray queries in the fragment shader: shadows from the sun and the lamps,
// how much of the sky is blocked, what light bounces in from nearby surfaces.
// Here the CPU asks it per VERTEX, against its own copy of the scene's
// triangles, and hands the answers to the GPU as one more vertex attribute.
// The GPU still rasterises and still evaluates the material per pixel; it just
// no longer traces anything.
//
// Two consequences a reader should expect, both visible in the viewport:
//
// - Lighting is only as fine as the vertices. A ground plane of four corners
//   would have no shadows at all, so every mesh is drawn from a subdivided copy
//   whose edges are no longer than the vertex spacing setting. Shadow edges
//   are as soft as that spacing.
//
// - Rays cost time, and the CPU has a fixed budget of it per frame. The answer
//   is refined over several frames - one sample per vertex per sweep, averaged
//   - and starts over whenever something in the scene moves. A drag looks
//   coarse; let go and it settles.
//
// Private to the renderer: nothing outside it should depend on how this works.

#include "fumar/core/job_pool.hpp"
#include "fumar/core/math.hpp"
#include "fumar/geometry/raycast.hpp"
#include "fumar/render/mesh.hpp"
#include "fumar/scene/scene.hpp"

#include <array>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace fumar {

class FrameProfiler;

/// What the lighting depends on besides geometry, gathered from the frame's
/// uniforms so the CPU traces against exactly what the GPU would.
struct CpuLightInputs {
    Vec3 sunDirection{0.0f, 1.0f, 0.0f};
    f32 sunAngularRadius = 0.0f;
    Vec3 sunRadiance{1.0f, 1.0f, 1.0f};
    Vec3 skyZenith{};
    Vec3 skyHorizon{};
    Vec3 ground{};
    f32 skyIntensity = 1.0f;
    f32 occlusionRadius = 1.0f;

    /// The first four placed lights of the frame, in its order: the CPU
    /// traces shadows for these and the shader reads them by the same index.
    struct Lamp {
        Vec3 position{};
        f32 range = 0.0f;
        f32 sourceRadius = 0.0f;
        bool castsShadow = false;
    };
    static constexpr u32 kMaxLamps = 4;
    std::array<Lamp, kMaxLamps> lamps{};
    u32 lampCount = 0;
};

/// One object as the renderer hands it over.
struct CpuDrawable {
    NodeId id = kInvalidNode;
    const Mesh* mesh = nullptr;
    Mat4 world;
    u32 materialIndex = 0;
    f32 highlight = 0.0f;

    /// What a ray that lands on this object sees: the colour it reflects and
    /// the light it gives off. Approximate where a texture is involved - the
    /// CPU has no copy of the pixels - see CpuWorkload::prepare's caller.
    Vec3 albedo{0.5f, 0.5f, 0.5f};
    Vec3 emissive{};
};

/// The light stream: one per vertex, beside the ordinary vertex, at shader
/// locations 4 to 6. Raw answers - the shader applies the strength sliders, so
/// moving one does not throw the accumulated samples away.
struct CpuLightVertex {
    Vec4 visibility; ///< sun, sky, occlusion within the AO radius, unused
    Vec4 lamps;      ///< visibility of lamps 0..3
    Vec4 bounce;     ///< bounced light, rgb
};
static_assert(sizeof(CpuLightVertex) == 48);

class CpuWorkload {
public:
    CpuWorkload();
    ~CpuWorkload();

    struct Settings {
        /// The longest a triangle edge may be, in metres, before it is split
        /// for the sake of the lighting.
        f32 vertexSpacing = 0.5f;

        /// Milliseconds of wall time per frame spent tracing, on all cores.
        f32 budgetMs = 8.0f;
    };
    Settings settings;

    /// The sun, sky and lamps to trace against, filled in by the renderer
    /// from the frame's uniforms.
    CpuLightInputs inputs;

    struct Draw {
        u32 firstIndex = 0;
        u32 indexCount = 0;
        i32 vertexOffset = 0;
        u32 materialIndex = 0;
        f32 highlight = 0.0f;
    };

    /// Culls against the view, picks or builds each mesh's subdivided copy,
    /// and transforms what is visible into world space.
    void prepare(std::span<const CpuDrawable> drawables, const Mat4& viewProjection, FrameProfiler& profiler);

    /// Spends this frame's budget tracing against `inputs`, then writes the
    /// light stream.
    void light(FrameProfiler& profiler);

    std::span<const Vertex> vertices() const { return m_vertices; }
    std::span<const CpuLightVertex> lightVertices() const { return m_light; }
    std::span<const u32> indices() const { return m_indices; }
    std::span<const Draw> draws() const { return m_draws; }

    /// How far the lighting has got: samples per vertex, the least any
    /// visible vertex has, out of kMaxSamples.
    u32 samples() const { return m_minSamples; }
    static constexpr u32 kMaxSamples = 64;

    u32 threadCount() const { return m_pool.threadCount(); }

    /// Forgets every cache. Meshes can be rebuilt from scratch afterwards.
    void clear();

private:
    struct DenseMesh;
    struct Caster;
    struct Accumulator;
    struct Instance;
    struct Visible;

    const DenseMesh& denseMesh(const Mesh& mesh, u32 levels);
    const Caster& caster(const Mesh& mesh);

    JobPool m_pool;

    /// Subdivided copies, keyed by the mesh's serial and the number of
    /// halvings; and the ray casters, by serial alone. Entries not used for a
    /// while are dropped, so a session of edits does not keep every shape.
    std::unordered_map<u64, std::unique_ptr<DenseMesh>> m_dense;
    std::unordered_map<u64, std::unique_ptr<Caster>> m_casters;
    u64 m_frame = 0;

    /// The light gathered so far, per node, one entry per vertex of its
    /// subdivided copy.
    std::unordered_map<NodeId, std::unique_ptr<Accumulator>> m_accumulators;

    std::vector<Instance> m_instances;
    std::vector<Visible> m_visible;
    u64 m_signature = 0;
    u64 m_visibleSignature = 0;
    usize m_cursor = 0;
    usize m_sweepWork = 0;
    usize m_batch = 2048;
    bool m_converged = false;
    u32 m_minSamples = 0;

    std::vector<Vertex> m_vertices;
    std::vector<CpuLightVertex> m_light;
    std::vector<u32> m_indices;
    std::vector<Draw> m_draws;
};

} // namespace fumar
