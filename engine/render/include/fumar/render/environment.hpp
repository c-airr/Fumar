#pragma once

#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"

namespace fumar {

/// Everything that lights the scene but is not in it.
///
/// Deliberately a plain struct of numbers a person can understand rather than a
/// pile of pre-baked vectors: the editor edits these fields directly, the scene
/// file stores them verbatim, and the renderer derives whatever the shaders
/// need from them each frame. A "sun direction" stored as a vector would be
/// impossible to drag in a user interface without turning it back into angles
/// first, so it is kept as angles and converted on the way to the GPU.
struct Environment {
    // --- the sun ------------------------------------------------------------

    /// Height above the horizon, in degrees. 90 is straight overhead, 0 is
    /// sunset, negative is night.
    f32 sunElevationDegrees = 42.0f;

    /// Compass direction, in degrees. Rotates the sun around the vertical axis.
    f32 sunAzimuthDegrees = 130.0f;

    /// Slightly warm white. Cooling this towards blue and dropping the
    /// elevation is most of what makes a scene read as evening.
    Vec3 sunColor{1.0f, 0.96f, 0.90f};

    /// How bright the sun is relative to the sky. Not in physical units - real
    /// sunlight is around 100,000 lux and the sky a few thousand, and working
    /// in those numbers would mean every other value in the engine carrying
    /// five zeroes for no benefit.
    f32 sunIntensity = 5.0f;

    /// Half-angle of the sun's disc, in degrees. The real sun is 0.27; larger
    /// values give a bigger, softer sun, and once shadows are ray traced this
    /// is also what decides how soft their edges are.
    f32 sunAngularRadiusDegrees = 0.6f;

    // --- the sky ------------------------------------------------------------

    /// Straight overhead. Deeper and bluer than the horizon, because there is
    /// less air in the way.
    Vec3 skyZenithColor{0.22f, 0.38f, 0.70f};

    /// At the horizon, where sunlight has travelled through far more
    /// atmosphere and lost most of its blue.
    Vec3 skyHorizonColor{0.62f, 0.72f, 0.86f};

    /// What is below the horizon. This is not decoration: it is the light that
    /// bounces up off the ground onto the underside of everything, and without
    /// it objects look like they are floating.
    Vec3 groundColor{0.26f, 0.24f, 0.22f};

    /// Below one because the sun is the dominant light outdoors by roughly
    /// five to one. Raise it and the scene flattens out the way it does under
    /// cloud, where light arrives from everywhere and nothing casts a shadow.
    f32 skyIntensity = 0.55f;

    // --- the camera ---------------------------------------------------------

    /// Multiplies the image before tone mapping. A property of the camera, not
    /// of the scene - the same world at a different exposure is the same world.
    ///
    /// Below one because the intensities above are radiances rather than
    /// numbers picked to land in 0..1, which is the whole point of rendering in
    /// HDR. This is the single knob that decides where that range ends up on
    /// the display.
    f32 exposure = 0.5f;

    /// Unit vector pointing TOWARDS the sun, derived from the two angles.
    ///
    /// Elevation lifts it off the horizon and azimuth swings it around, in a
    /// Y-up right-handed world. Pointing at the light rather than along it is
    /// what makes dot(normal, sun) the brightness directly.
    Vec3 sunDirection() const {
        const f32 elevation = radians(sunElevationDegrees);
        const f32 azimuth = radians(sunAzimuthDegrees);
        const f32 horizontal = std::cos(elevation);
        return normalize(Vec3{
            horizontal * std::sin(azimuth),
            std::sin(elevation),
            horizontal * std::cos(azimuth),
        });
    }
};

} // namespace fumar
