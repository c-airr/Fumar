#include "fumar/script/script_context.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace fumar {
namespace {

/// Lowercase names, because that is what reads naturally in a script:
/// fumar.key("space") rather than fumar.key("Space").
constexpr std::array<std::pair<std::string_view, ScriptKey>,
                     static_cast<usize>(ScriptKey::Count)>
    kNames{{
        {"w", ScriptKey::W},
        {"a", ScriptKey::A},
        {"s", ScriptKey::S},
        {"d", ScriptKey::D},
        {"q", ScriptKey::Q},
        {"e", ScriptKey::E},
        {"space", ScriptKey::Space},
        {"shift", ScriptKey::Shift},
        {"ctrl", ScriptKey::Control},
        {"up", ScriptKey::Up},
        {"down", ScriptKey::Down},
        {"left", ScriptKey::Left},
        {"right", ScriptKey::Right},
        {"mouse_left", ScriptKey::MouseLeft},
        {"mouse_right", ScriptKey::MouseRight},
    }};

} // namespace

ScriptKey scriptKeyFromName(std::string_view name) {
    const auto found = std::find_if(kNames.begin(), kNames.end(),
                                    [name](const auto& entry) { return entry.first == name; });

    // Count, not an error: a typo in a script name should leave that key simply
    // never pressed rather than stopping the script. The alternative is a
    // gameplay script dying mid-frame over a spelling mistake.
    return found != kNames.end() ? found->second : ScriptKey::Count;
}

} // namespace fumar
