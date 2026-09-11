// Smoke test for C++ hot reloading, driven without the editor.
//
// Proves the whole loop: load the library, instantiate a component, run it,
// change the SOURCE, rebuild through the same path the Compile button uses,
// and confirm the new code is what runs afterwards.

#include "fumar/core/log.hpp"
#include "fumar/core/math.hpp"
#include "fumar/native/native_engine.hpp"
#include "fumar/scene/scene.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace fumar;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    std::printf(condition ? "  PASS  %s\n" : "  FAIL  %s\n", what.c_str());
    if (!condition) {
        ++failures;
    }
}

/// Degrees turned about any axis, from a quaternion. w = cos(theta/2).
f32 rotationDegrees(const Quat& q) {
    return degrees(2.0f * std::acos(clamp(q.w, -1.0f, 1.0f)));
}

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
}

/// One second of updates, from a node whose rotation starts at identity.
f32 spinOverOneSecond(NativeEngine& native, Scene& scene, NodeId id) {
    scene.node(id).transform.rotation = Quat{};

    ScriptContext context;
    // Ten steps of a tenth, because a component integrating per frame should
    // land in the same place either way - and one step would hide a bug where
    // it does not.
    for (int i = 0; i < 10; ++i) {
        native.update(scene, context, 0.1f);
    }
    return rotationDegrees(scene.node(id).transform.rotation);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: native_smoke <library-directory> <components.cpp>\n");
        return 2;
    }

    const std::string libraryDirectory = argv[1];
    const std::string sourcePath = argv[2];

    log::setLevel(log::Level::Warn);

    Scene scene;
    NativeEngine native(libraryDirectory);

    // --- 1. load ------------------------------------------------------------
    const u32 count = native.reload(scene);
    check(count > 0, "the game library loads");
    check(native.has("Spinner"), "it registered Spinner");
    check(native.has("Hover"), "it registered Hover");
    check(native.has("Follow"), "it registered Follow");

    if (count == 0) {
        for (const std::string& error : native.errors()) {
            std::printf("  ! %s\n", error.c_str());
        }
        return 1;
    }

    // --- 2. run -------------------------------------------------------------
    const NodeId id = scene.createNode("Subject");
    scene.node(id).component = "Spinner";

    const f32 before = spinOverOneSecond(native, scene, id);
    std::printf("  ..    one second of Spinner: %.1f degrees\n", static_cast<f64>(before));
    check(std::abs(before - 45.0f) < 1.0f, "Spinner turns 45 degrees per second");

    // --- 3. edit the source -------------------------------------------------
    const std::string original = readFile(sourcePath);
    check(!original.empty(), "the component source is readable");

    const std::string needle = "m_degreesPerSecond = 45.0f";
    check(original.find(needle) != std::string::npos, "the rate to change is where expected");

    std::string edited = original;
    edited.replace(edited.find(needle), needle.size(), "m_degreesPerSecond = 180.0f");
    writeFile(sourcePath, edited);

    // --- 4. rebuild through the same path the editor uses --------------------
    std::printf("  ..    rebuilding\n");
    const bool rebuilt = native.rebuild(scene);
    check(rebuilt, "rebuild succeeds");

    if (!rebuilt) {
        for (const std::string& error : native.errors()) {
            std::printf("  ! %s\n", error.c_str());
        }
    }

    // --- 5. the new code is what runs ---------------------------------------
    const f32 after = spinOverOneSecond(native, scene, id);
    std::printf("  ..    one second after the edit: %.1f degrees\n", static_cast<f64>(after));
    check(std::abs(after - 180.0f) < 2.0f, "the EDITED rate is what runs now");

    // --- 6. put the source back ---------------------------------------------
    writeFile(sourcePath, original);
    const bool restored = native.rebuild(scene);
    check(restored, "rebuild after restoring the source succeeds");

    const f32 finally = spinOverOneSecond(native, scene, id);
    check(std::abs(finally - 45.0f) < 1.0f, "and the original rate is back");

    std::printf(failures == 0 ? "\nall good\n" : "\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
