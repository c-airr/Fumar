#include "fumar/core/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

#if defined(_WIN32)
// The only Windows-specific code in the engine, and it is here purely so the
// legacy console understands ANSI colour escapes. Everything else in fumar goes
// through SDL or Vulkan and stays portable.
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fumar::log {
namespace {

std::atomic<Level> g_level{
#if defined(NDEBUG)
    Level::Info
#else
    Level::Trace
#endif
};

/// Serialises writes so that two threads cannot interleave halves of a line.
std::mutex g_mutex;

/// Program start, used as the time origin. A monotonic clock is a better fit
/// than a wall clock here: it never jumps, and "seconds since start" is what
/// you actually want when reading frame timings.
const std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

struct LevelStyle {
    std::string_view name;
    std::string_view colour; // ANSI SGR sequence
};

constexpr LevelStyle levelStyle(Level level) {
    switch (level) {
    case Level::Trace: return {"TRACE", "\x1b[90m"};    // bright black
    case Level::Debug: return {"DEBUG", "\x1b[36m"};    // cyan
    case Level::Info: return {"INFO ", "\x1b[32m"};     // green
    case Level::Warn: return {"WARN ", "\x1b[33m"};     // yellow
    case Level::Error: return {"ERROR", "\x1b[31m"};    // red
    case Level::Fatal: return {"FATAL", "\x1b[97;41m"}; // white on red
    case Level::Off: break;
    }
    return {"?????", ""};
}

/// Colours are only emitted when stderr is an actual terminal. Redirecting the
/// output to a file then yields clean text instead of escape sequences.
bool detectColourSupport() {
#if defined(_WIN32)
    if (_isatty(_fileno(stderr)) == 0) {
        return false;
    }
    // Modern terminals understand ANSI, but the classic console needs the
    // virtual terminal mode switched on explicitly.
    HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
        return false;
    }
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode) == 0) {
        return false;
    }
    return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return isatty(fileno(stderr)) != 0;
#endif
}

const bool g_colour = detectColourSupport();

/// Trims a full source path down to the file name. __FILE__ expands to whatever
/// path the compiler was given, which is long and mostly noise.
std::string_view fileName(std::string_view path) {
    const usize slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace

void setLevel(Level level) {
    g_level.store(level, std::memory_order_relaxed);
}

Level level() {
    return g_level.load(std::memory_order_relaxed);
}

bool enabled(Level level) {
    return level >= g_level.load(std::memory_order_relaxed);
}

void detail::write(Level level, std::string_view file, int line, std::string_view message) {
    const LevelStyle style = levelStyle(level);
    const auto elapsed = std::chrono::duration<f64>(std::chrono::steady_clock::now() - g_start).count();

    // Source location is only worth the visual noise once something is wrong,
    // and callers that forward messages from elsewhere (the Vulkan validation
    // layers, for instance) pass an empty file to suppress it entirely.
    const bool withLocation = level >= Level::Warn && !file.empty();

    std::string formatted;
    if (g_colour) {
        formatted = std::format("\x1b[90m[{:8.3f}]\x1b[0m {}{}\x1b[0m {}", elapsed, style.colour, style.name,
                                message);
    } else {
        formatted = std::format("[{:8.3f}] {} {}", elapsed, style.name, message);
    }

    if (withLocation) {
        formatted += g_colour ? std::format("\x1b[90m  ({}:{})\x1b[0m", fileName(file), line)
                              : std::format("  ({}:{})", fileName(file), line);
    }

    const std::lock_guard lock(g_mutex);
    std::fputs(formatted.c_str(), stderr);
    std::fputc('\n', stderr);
    // Unbuffered on purpose: if the next line crashes the process, the log that
    // explains why must already be on screen.
    std::fflush(stderr);
}

} // namespace fumar::log
