#pragma once

#include "fumar/core/types.hpp"

#include <vulkan/vulkan.hpp>

#include <array>
#include <chrono>
#include <vector>

namespace fumar {

namespace rhi {
class Device;
}

/// The stages of a frame the profiler times, on either processor. One list for
/// both, so the Statistics panel can put a stage's CPU and GPU cost side by
/// side - which is the whole point of the workload switch.
enum class ProfileStage : u32 {
    Wait,       // CPU: blocked on the GPU finishing the frame two back
    Nodes,      // node matrices: CPU hierarchy pass / GPU compute
    Culling,    // dropping what is outside the view
    Transform,  // vertex positions into world space (Cpu workload)
    Lighting,   // shadows, occlusion, bounced light (Cpu workload, on the CPU)
    Materials,  // material records and frame uniforms
    Structure,  // ray-tracing structure: CPU instance list / GPU build
    Scene,      // the scene pass: CPU recording / GPU raster + shading + rays
    Temporal,
    Bloom,
    Tonemap,
    Interface,  // CPU: building the editor's widgets / GPU: drawing them
    Present,    // CPU: submit and present, which can wait for vsync
    Count,
};

inline constexpr usize kProfileStageCount = static_cast<usize>(ProfileStage::Count);

/// Where a frame's milliseconds went, on the CPU and on the GPU.
///
/// The CPU side is a clock read around each stage. The GPU side cannot be
/// timed from the CPU at all - recording a command takes microseconds whatever
/// it will cost to run - so the GPU is asked to write its own clock into a
/// query pool before and after each pass. Those numbers exist only once the
/// frame has finished on the GPU, so they are read when its frame slot comes
/// round again, after the fence: two frames late, and never waited for.
///
/// Every figure is smoothed, because one frame's timing jitters too much to
/// read.
class FrameProfiler {
public:
    /// Creates the query pool, unless the graphics queue cannot write
    /// timestamps, in which case only the CPU side works.
    void init(const rhi::Device& device, u32 framesInFlight);
    void shutdown();

    bool gpuSupported() const { return m_gpuSupported; }

    /// Adds `ms` of CPU time to a stage of the frame being built.
    void addCpu(ProfileStage stage, f64 ms) { m_cpuRaw[index(stage)] += ms; }

    /// Times its own lifetime into a stage.
    class Scope {
    public:
        Scope(FrameProfiler& profiler, ProfileStage stage)
            : m_profiler(profiler), m_stage(stage), m_start(std::chrono::steady_clock::now()) {}
        ~Scope() {
            const std::chrono::duration<f64, std::milli> elapsed = std::chrono::steady_clock::now() - m_start;
            m_profiler.addCpu(m_stage, elapsed.count());
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        FrameProfiler& m_profiler;
        ProfileStage m_stage;
        std::chrono::steady_clock::time_point m_start;
    };

    Scope cpu(ProfileStage stage) { return Scope(*this, stage); }

    /// Reads back what the GPU wrote the last time `frame` was used. Call
    /// after that slot's fence, before resetQueries.
    void collect(vk::Device device, u32 frame);

    /// Clears this slot's queries, at the start of its command buffer.
    void resetQueries(vk::CommandBuffer cmd, u32 frame);

    void gpuBegin(vk::CommandBuffer cmd, u32 frame, ProfileStage stage);
    void gpuEnd(vk::CommandBuffer cmd, u32 frame, ProfileStage stage);

    /// Folds the frame's CPU times into the averages and starts a new frame.
    void endCpuFrame();

    /// Smoothed milliseconds; negative when the stage did not run there.
    f32 cpuMs(ProfileStage stage) const { return m_cpu[index(stage)]; }
    f32 gpuMs(ProfileStage stage) const { return m_gpu[index(stage)]; }

    static const char* name(ProfileStage stage);

private:
    static usize index(ProfileStage stage) { return static_cast<usize>(stage); }
    u32 query(u32 frame, ProfileStage stage, bool end) const {
        return (frame * static_cast<u32>(kProfileStageCount) + static_cast<u32>(stage)) * 2 + (end ? 1 : 0);
    }

    static void smooth(f32& average, f64 sample, bool ran);

    vk::UniqueQueryPool m_pool;
    bool m_gpuSupported = false;
    f64 m_tickNs = 1.0;
    u64 m_tickMask = ~u64{0};
    std::vector<bool> m_recorded;

    std::array<f64, kProfileStageCount> m_cpuRaw{};
    std::array<f32, kProfileStageCount> m_cpu{};
    std::array<f32, kProfileStageCount> m_gpu{};
};

} // namespace fumar
