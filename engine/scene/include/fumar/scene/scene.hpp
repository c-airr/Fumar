#pragma once

#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"
#include "fumar/scene/handles.hpp"
#include "fumar/scene/transform.hpp"

#include <functional>
#include <string>
#include <vector>

namespace fumar {

using NodeId = u32;

inline constexpr NodeId kInvalidNode = ~0u;

/// The root node, created with the scene and never destroyed. Everything else
/// descends from it, so there is always exactly one tree rather than a forest.
inline constexpr NodeId kRootNode = 0;

/// One entry in the scene tree.
///
/// A node holds a transform and, optionally, something to draw. That is
/// deliberately closer to a scene graph than to an ECS: an editor needs a tree
/// it can show, reorder and select in, and a tree is the shape that maps onto.
/// The rendering side flattens it back into a linear list each frame, which is
/// where the ECS-style performance actually comes from.
struct Node {
    std::string name;
    Transform transform;

    NodeId parent = kInvalidNode;
    std::vector<NodeId> children;

    /// Geometry to draw at this node's world transform. An invalid handle means
    /// the node is a pure grouping or pivot, which is common in glTF files.
    MeshHandle mesh;
    MaterialHandle material;

    /// Hides the node and everything under it. Kept separate from destruction
    /// so an editor can toggle it without losing the node.
    bool visible = true;
};

/// A tree of nodes, with no knowledge of graphics.
///
/// Nothing here includes Vulkan or SDL. That is what makes the scene something
/// that can be loaded, edited, saved and unit-tested on its own - and it is why
/// this is a separate module rather than part of the renderer.
class Scene {
public:
    Scene();

    /// Adds a node under `parent`. The returned id stays valid until the node
    /// is destroyed.
    NodeId createNode(std::string name, NodeId parent = kRootNode);

    /// Removes a node and its entire subtree.
    void destroyNode(NodeId id);

    bool isAlive(NodeId id) const;

    /// Access to a node's data.
    ///
    /// The reference is invalidated by createNode(), which may reallocate the
    /// backing storage. Hold the NodeId, not the reference, across any call
    /// that can add a node.
    Node& node(NodeId id);
    const Node& node(NodeId id) const;

    /// Moves a node to a different parent, keeping its LOCAL transform - so it
    /// visually jumps to wherever that transform means under the new parent.
    void setParent(NodeId id, NodeId newParent);

    /// Recomputes every world transform. Call once per frame, after any change
    /// and before rendering.
    ///
    /// Computed in one pass over the tree rather than on demand per node: a
    /// naive worldTransform() that walks up to the root each time is O(depth)
    /// per query and repeats the same multiplications for every sibling.
    void updateWorldTransforms();

    /// World transform from the last updateWorldTransforms() call.
    const Mat4& worldTransform(NodeId id) const;

    /// Visits every visible node that has geometry, with its world transform.
    void forEachDrawable(const std::function<void(const Node&, const Mat4&)>& visit) const;

    usize nodeCount() const;

    /// Depth-first traversal, used by the editor to draw the hierarchy panel.
    /// `depth` starts at 0 for the root's children.
    void traverse(const std::function<void(NodeId, u32 depth)>& visit) const;

private:
    void destroyRecursive(NodeId id);
    void detachFromParent(NodeId id);

    // Parallel arrays keyed by NodeId. A slot map rather than a vector of
    // nodes, so ids stay stable when other nodes are destroyed - an editor
    // holding a selection cannot have it silently point at something else.
    std::vector<Node> m_nodes;
    std::vector<Mat4> m_worldTransforms;
    std::vector<bool> m_alive;
    std::vector<NodeId> m_freeSlots;
};

} // namespace fumar
