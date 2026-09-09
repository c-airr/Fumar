#pragma once

#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"

namespace fumar {

/// Position, rotation and scale, kept apart instead of as one matrix.
///
/// A matrix can express the same thing, but it cannot be edited: pulling a
/// clean rotation back out of a matrix that also carries scale is lossy, and
/// interpolating two matrices directly produces shearing. Keeping the three
/// components separate is what lets an editor show three fields, and what lets
/// animation interpolate rotation as a quaternion.
///
/// The matrix is built on demand, in the order scale, then rotate, then
/// translate. Any other order gives a different result - scaling after rotating
/// skews the object rather than resizing it.
struct Transform {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};

    Mat4 matrix() const { return translation(position) * toMat4(rotation) * scaling(scale); }

    /// Recovers a Transform from a matrix.
    ///
    /// Needed because glTF nodes may store either a TRS triple or a raw matrix,
    /// and the editor needs the triple either way. Only valid for matrices
    /// built from translation, rotation and non-negative scale - shear and
    /// mirroring cannot be represented and are silently lost.
    static Transform fromMatrix(const Mat4& matrix);

    friend bool operator==(const Transform&, const Transform&) = default;
};

} // namespace fumar
