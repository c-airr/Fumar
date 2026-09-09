#include "fumar/core/log.hpp"
#include "fumar/core/types.hpp"
#include "fumar/platform/window.hpp"
#include "fumar/render/renderer.hpp"

#include <algorithm>
#include <chrono>

int main() {
    using namespace fumar;
    using Clock = std::chrono::steady_clock;

    FUMAR_INFO("fumar sandbox starting");

    Window window(WindowDesc{
        .title = "fumar - sandbox",
        .width = 1280,
        .height = 720,
        .resizable = true,
    });

    Renderer renderer(window);

    FUMAR_INFO("controls: hold right mouse to look, WASD to move, space/ctrl up and down,");
    FUMAR_INFO("          shift to sprint, escape to release the cursor or quit");

    const auto startTime = Clock::now();
    auto lastFrameTime = startTime;
    auto lastReport = startTime;
    u64 framesSinceReport = 0;

    while (!window.shouldClose()) {
        window.pumpEvents();

        // Nothing to draw while minimised, and a spin loop here would burn a
        // core for no reason. Blocking on the event queue keeps the window
        // responsive without the busy wait.
        if (window.minimized()) {
            window.waitEvents(100);
            // Reset the clock too, or the first frame after restoring would see
            // a delta of however long the window sat minimised and teleport the
            // camera across the scene.
            lastFrameTime = Clock::now();
            continue;
        }

        const auto now = Clock::now();
        const f32 elapsed = std::chrono::duration<f32>(now - startTime).count();

        // Clamped, because a breakpoint or a stalled frame would otherwise
        // produce one enormous step. Losing a little accuracy after a hitch
        // beats moving the camera a hundred metres in one frame.
        const f32 deltaSeconds =
            std::min(std::chrono::duration<f32>(now - lastFrameTime).count(), 0.1f);
        lastFrameTime = now;

        renderer.camera().update(window, deltaSeconds);
        renderer.drawFrame(elapsed);
        ++framesSinceReport;

        const auto sinceReport = std::chrono::duration<f64>(now - lastReport).count();
        if (sinceReport >= 1.0) {
            FUMAR_INFO("{:.0f} fps ({:.2f} ms/frame)", static_cast<f64>(framesSinceReport) / sinceReport,
                       1000.0 * sinceReport / static_cast<f64>(framesSinceReport));
            lastReport = now;
            framesSinceReport = 0;
        }
    }

    FUMAR_INFO("fumar sandbox shutting down");
    return 0;
}
