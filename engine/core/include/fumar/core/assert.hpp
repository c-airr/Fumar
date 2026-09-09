#pragma once

#include "fumar/core/log.hpp"

#include <format>
#include <string_view>

// ---------------------------------------------------------------------------
// Debug break.
//
// Traps into the debugger at the exact line that failed, which keeps the stack
// intact - far more useful than an abort() several frames up the call stack.
// clang-cl defines both _MSC_VER and __clang__, and it supports __debugbreak(),
// so the MSVC branch is checked first.
// ---------------------------------------------------------------------------
#if defined(_MSC_VER)
#define FUMAR_DEBUG_BREAK() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__)
#define FUMAR_DEBUG_BREAK() __builtin_trap()
#else
#include <cstdlib>
#define FUMAR_DEBUG_BREAK() ::std::abort()
#endif

namespace fumar::detail {

/// Logs a failed check. Separate from the macro so the formatting code is not
/// duplicated into every call site.
void reportAssertion(std::string_view expression, std::string_view file, int line,
                     std::string_view message);

} // namespace fumar::detail

// ---------------------------------------------------------------------------
// FUMAR_VERIFY - always checked, in every build.
// FUMAR_ASSERT - checked in debug builds only; compiled away in release.
//
// Use VERIFY for anything that depends on the outside world (a file that failed
// to open, a Vulkan call that returned an error) and ASSERT for invariants that
// are supposed to be guaranteed by our own code.
// ---------------------------------------------------------------------------

#define FUMAR_VERIFY(cond)                                                                                   \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            ::fumar::detail::reportAssertion(#cond, __FILE__, __LINE__, {});                                 \
            FUMAR_DEBUG_BREAK();                                                                             \
        }                                                                                                    \
    } while (false)

#define FUMAR_VERIFY_MSG(cond, ...)                                                                          \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            ::fumar::detail::reportAssertion(#cond, __FILE__, __LINE__, ::std::format(__VA_ARGS__));          \
            FUMAR_DEBUG_BREAK();                                                                             \
        }                                                                                                    \
    } while (false)

#if defined(NDEBUG)
// `sizeof` never evaluates its operand, so the condition costs nothing at
// runtime while still being parsed - a typo inside a release-disabled assert
// is caught at compile time instead of months later.
#define FUMAR_ASSERT(cond)                                                                                   \
    do {                                                                                                     \
        (void)sizeof(cond);                                                                                  \
    } while (false)
#define FUMAR_ASSERT_MSG(cond, ...)                                                                          \
    do {                                                                                                     \
        (void)sizeof(cond);                                                                                  \
    } while (false)
#else
#define FUMAR_ASSERT(cond) FUMAR_VERIFY(cond)
#define FUMAR_ASSERT_MSG(cond, ...) FUMAR_VERIFY_MSG(cond, __VA_ARGS__)
#endif
