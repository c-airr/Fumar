#include "fumar/render/profiler.hpp"

#include "fumar/core/log.hpp"
#include "fumar/rhi/device.hpp"

namespace fumar {

namespace {

/// How much of each new frame goes into an average. Low enough that a number
/// can be read, high enough that it follows a change within a second.
constexpr f32 kBlend = 0.08f;

} // namespace

void FrameProfiler::init(const rhi::Device& device, u32 framesInFlight) {
    m_cpu.fill(-1.0f);
    m_gpu.fill(-1.0f);

    // A queue that writes no timestamp bits cannot be timed at all; one that
    // writes fewer than 64 wraps, and the mask keeps the subtraction right
    // across the wrap.
    const auto families = device.physicalDevice().getQueueFamilyProperties();
    const u32 bits = families[device.queueFamilies().graphics].timestampValidBits;
    if (bits == 0) {
        FUMAR_INFO("profiler: the graphics queue has no timestamps, GPU times are not shown");
        return;
    }
    m_tickMask = bits >= 64 ? ~u64{0} : (u64{1} << bits) - 1;
    m_tickNs = static_cast<f64>(device.properties().limits.timestampPeriod);

    m_pool = device.handle().createQueryPoolUnique(vk::QueryPoolCreateInfo{
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = framesInFlight * static_cast<u32>(kProfileStageCount) * 2,
    });
    m_recorded.assign(framesInFlight, false);
    m_gpuSupported = true;
}

void FrameProfiler::shutdown() {
    m_pool.reset();
    m_gpuSupported = false;
}

void FrameProfiler::smooth(f32& average, f64 sample, bool ran) {
    if (!ran) {
        average = -1.0f;
        return;
    }
    const f32 value = static_cast<f32>(sample);
    average = average < 0.0f ? value : average + (value - average) * kBlend;
}

void FrameProfiler::collect(vk::Device device, u32 frame) {
    if (!m_gpuSupported || !m_recorded[frame]) {
        return;
    }

    // Value and availability for each query. A stage that did not run this
    // frame was never written, reports itself unavailable, and is shown as
    // not running rather than as zero.
    struct Result {
        u64 value;
        u64 available;
    };
    std::array<Result, kProfileStageCount * 2> results{};
    const vk::Result status = device.getQueryPoolResults(
        *m_pool, query(frame, ProfileStage{}, false), static_cast<u32>(results.size()), sizeof(results),
        results.data(), sizeof(Result), vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
    if (status != vk::Result::eSuccess && status != vk::Result::eNotReady) {
        return;
    }

    for (usize stage = 0; stage < kProfileStageCount; ++stage) {
        const Result& begin = results[stage * 2];
        const Result& end = results[stage * 2 + 1];
        const bool ran = begin.available != 0 && end.available != 0;
        const u64 ticks = (end.value - begin.value) & m_tickMask;
        smooth(m_gpu[stage], static_cast<f64>(ticks) * m_tickNs / 1.0e6, ran);
    }
}

void FrameProfiler::resetQueries(vk::CommandBuffer cmd, u32 frame) {
    if (!m_gpuSupported) {
        return;
    }
    cmd.resetQueryPool(*m_pool, query(frame, ProfileStage{}, false), static_cast<u32>(kProfileStageCount) * 2);
    m_recorded[frame] = true;
}

// eAllCommands on both ends: a timestamp is written once every command before
// it has got past the given stage, so this one waits for the previous pass to
// finish entirely - which is what makes the two ends bracket one pass and not
// a slice of two overlapping ones.
void FrameProfiler::gpuBegin(vk::CommandBuffer cmd, u32 frame, ProfileStage stage) {
    if (m_gpuSupported) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *m_pool, query(frame, stage, false));
    }
}

void FrameProfiler::gpuEnd(vk::CommandBuffer cmd, u32 frame, ProfileStage stage) {
    if (m_gpuSupported) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *m_pool, query(frame, stage, true));
    }
}

void FrameProfiler::endCpuFrame() {
    for (usize stage = 0; stage < kProfileStageCount; ++stage) {
        smooth(m_cpu[stage], m_cpuRaw[stage], m_cpuRaw[stage] > 0.0);
    }
    m_cpuRaw.fill(0.0);
}

const char* FrameProfiler::name(ProfileStage stage) {
    switch (stage) {
    case ProfileStage::Wait: return "Waiting for the GPU";
    case ProfileStage::Nodes: return "Node matrices";
    case ProfileStage::Culling: return "Culling";
    case ProfileStage::Transform: return "Vertex transform";
    case ProfileStage::Lighting: return "Shadows, AO, bounce";
    case ProfileStage::Materials: return "Materials, uniforms";
    case ProfileStage::Structure: return "Ray structure";
    case ProfileStage::Scene: return "Scene pass";
    case ProfileStage::Temporal: return "Temporal filter";
    case ProfileStage::Bloom: return "Bloom";
    case ProfileStage::Tonemap: return "Tone mapping";
    case ProfileStage::Interface: return "Interface";
    case ProfileStage::Present: return "Submit, present";
    case ProfileStage::Count: break;
    }
    return "?";
}

} // namespace fumar
