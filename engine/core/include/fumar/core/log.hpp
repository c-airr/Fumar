#pragma once

#include "fumar/core/types.hpp"

#include <format>
#include <string_view>

namespace fumar::log {

enum class Level : u8 {
    Trace, ///< very chatty, per-frame details
    Debug, ///< useful while working on a subsystem
    Info,  ///< normal lifecycle events: device picked, swapchain created
    Warn,  ///< something is off but the engine keeps going
    Error, ///< an operation failed
    Fatal, ///< the engine cannot continue
    Off,   ///< only valid as a threshold, never as a message level
};

/// Messages below this level are dropped. Defaults to Info in release builds
/// and Trace in debug builds.
void setLevel(Level level);

Level level();

/// Cheap threshold check used by the macros so that std::format is not even
/// called for a message that would be discarded.
bool enabled(Level level);

namespace detail {

/// Formats and writes one line. Not meant to be called directly - the macros
/// below capture the source location for you.
void write(Level level, std::string_view file, int line, std::string_view message);

} // namespace detail

} // namespace fumar::log

// ---------------------------------------------------------------------------
// Logging macros.
//
// A macro rather than a function because __FILE__ and __LINE__ have to be
// expanded at the call site - a function would only ever report its own
// location. The do/while(false) wrapper makes the macro behave like a single
// statement, so `if (x) FUMAR_INFO(...); else ...` still compiles.
// ---------------------------------------------------------------------------

#define FUMAR_LOG(lvl, ...)                                                                                  \
    do {                                                                                                     \
        if (::fumar::log::enabled(lvl)) {                                                                    \
            ::fumar::log::detail::write((lvl), __FILE__, __LINE__, ::std::format(__VA_ARGS__));               \
        }                                                                                                    \
    } while (false)

#define FUMAR_TRACE(...) FUMAR_LOG(::fumar::log::Level::Trace, __VA_ARGS__)
#define FUMAR_DEBUG(...) FUMAR_LOG(::fumar::log::Level::Debug, __VA_ARGS__)
#define FUMAR_INFO(...) FUMAR_LOG(::fumar::log::Level::Info, __VA_ARGS__)
#define FUMAR_WARN(...) FUMAR_LOG(::fumar::log::Level::Warn, __VA_ARGS__)
#define FUMAR_ERROR(...) FUMAR_LOG(::fumar::log::Level::Error, __VA_ARGS__)
#define FUMAR_FATAL(...) FUMAR_LOG(::fumar::log::Level::Fatal, __VA_ARGS__)
