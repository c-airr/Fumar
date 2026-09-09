#include "fumar/platform/paths.hpp"

#include "fumar/core/log.hpp"

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

namespace fumar {

const std::filesystem::path& executableDirectory() {
    // Function-local static: computed once, on first use, and thread-safe
    // initialisation is guaranteed by the standard. Avoids the static
    // initialisation order problem that a namespace-scope global would have.
    static const std::filesystem::path directory = [] {
        const char* base = SDL_GetBasePath();
        if (base == nullptr) {
            FUMAR_WARN("SDL_GetBasePath failed ({}), falling back to the working directory",
                       SDL_GetError());
            return std::filesystem::current_path();
        }
        return std::filesystem::path(base);
    }();

    return directory;
}

} // namespace fumar
