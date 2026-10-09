#pragma once

#include "fumar/geometry/editable_mesh.hpp"

#include <optional>
#include <vector>

namespace fumar::geometry {

/// Shoots rays at a mesh, fast enough to do it once per vertex per frame.
///
/// The Modeler needs two answers, constantly: which face is under the cursor,
/// and which vertices are hidden behind the mesh itself. Testing a ray against
/// every triangle answers both and costs rays x triangles - for the helmet in
/// the starter scene, fourteen thousand vertices against forty-six thousand
/// triangles, which is most of a second per frame.
///
/// A bounding volume hierarchy brings that down to a few dozen triangles per
/// ray. The triangles are sorted into a tree of boxes, each box holding the
/// boxes or triangles inside it; a ray that misses a box skips everything in
/// it. Same idea as the GPU's acceleration structures, built on the CPU in the
/// simplest form that works: split the longest axis at the median triangle,
/// stop at four.
///
/// Built from a snapshot of the mesh. Change the mesh, build a new one.
class RayCaster {
public:
    RayCaster() = default;
    explicit RayCaster(const EditableMesh& mesh);

    struct Hit {
        /// Distance along the ray in multiples of `direction`, which need not
        /// be unit length - pass the offset to a target and t = 1 is the target.
        f32 t = 0.0f;
        u32 face = 0;
    };

    /// The nearest face the ray meets before tMax, from either side.
    std::optional<Hit> closest(Vec3 origin, Vec3 direction, f32 tMax = 1e30f) const;

    /// Whether anything at all lies on the ray before tMax. Stops at the first
    /// hit, which is what makes it cheaper than closest().
    bool occluded(Vec3 origin, Vec3 direction, f32 tMax) const;

    bool empty() const { return m_triangles.empty(); }

private:
    struct Triangle {
        Vec3 a;
        Vec3 b;
        Vec3 c;
        u32 face = 0;
    };

    struct Node {
        Vec3 low;
        Vec3 high;

        /// For a leaf, the triangles [first, first + count). For an interior
        /// node count is zero and `first` is the left child; the right child
        /// is first + 1.
        u32 first = 0;
        u32 count = 0;
    };

    void build(u32 index, u32 first, u32 count);

    template <bool AnyHit>
    std::optional<Hit> trace(Vec3 origin, Vec3 direction, f32 tMax) const;

    std::vector<Triangle> m_triangles;
    std::vector<Node> m_nodes;
};

} // namespace fumar::geometry
