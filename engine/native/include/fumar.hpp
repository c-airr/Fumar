#pragma once

/// Everything gameplay written in C++ needs, in one include.
///
/// The modules underneath are split the way they are for the ENGINE's benefit -
/// so that the renderer cannot quietly start depending on the window system,
/// and the scene can be tested without a GPU. None of that is any of gameplay's
/// business. A component wants a node, some maths, a way to log, and input;
/// making it learn which of five headers each of those lives in buys nothing.
///
///     #include <fumar.hpp>
///
///     class Spinner final : public fumar::Component { ... };
///
/// The individual headers are still there and still work. This is a convenience
/// rather than a replacement, and engine code keeps including exactly what it
/// uses - a module that pulled in the whole engine to reach one type is how the
/// dependency graph stops meaning anything.

// The component interface itself: Component, ComponentContext, the registry and
// the registration macro. Brings Scene and ScriptContext with it.
#include "fumar/native/component.hpp"

#include "fumar/core/log.hpp"
#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"
