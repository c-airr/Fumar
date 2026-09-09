#include "fumar/scene/scene.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/core/log.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace fumar {

Transform Transform::fromMatrix(const Mat4& matrix) {
    Transform result;

    // Translation is simply the fourth column.
    result.position = xyz(matrix.columns[3]);

    // Scale is the length of each basis vector: the unit axes are stretched by
    // exactly that much before rotation is applied.
    const Vec3 axisX = xyz(matrix.columns[0]);
    const Vec3 axisY = xyz(matrix.columns[1]);
    const Vec3 axisZ = xyz(matrix.columns[2]);
    result.scale = Vec3{length(axisX), length(axisY), length(axisZ)};

    // Dividing the scale out leaves a pure rotation matrix. A zero scale would
    // divide by zero and there is no rotation to recover anyway, so it is left
    // as identity.
    if (result.scale.x == 0.0f || result.scale.y == 0.0f || result.scale.z == 0.0f) {
        return result;
    }

    Mat4 rotationMatrix = identity();
    rotationMatrix.columns[0] = direction(axisX / result.scale.x);
    rotationMatrix.columns[1] = direction(axisY / result.scale.y);
    rotationMatrix.columns[2] = direction(axisZ / result.scale.z);

    // Shepperd's method: the naive formula divides by w, which vanishes for
    // 180-degree rotations. Picking whichever component is largest keeps the
    // divisor away from zero in every case.
    const f32 m00 = rotationMatrix.at(0, 0);
    const f32 m11 = rotationMatrix.at(1, 1);
    const f32 m22 = rotationMatrix.at(2, 2);
    const f32 trace = m00 + m11 + m22;

    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        result.rotation.w = 0.25f * s;
        result.rotation.x = (rotationMatrix.at(2, 1) - rotationMatrix.at(1, 2)) / s;
        result.rotation.y = (rotationMatrix.at(0, 2) - rotationMatrix.at(2, 0)) / s;
        result.rotation.z = (rotationMatrix.at(1, 0) - rotationMatrix.at(0, 1)) / s;
    } else if (m00 > m11 && m00 > m22) {
        const f32 s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        result.rotation.w = (rotationMatrix.at(2, 1) - rotationMatrix.at(1, 2)) / s;
        result.rotation.x = 0.25f * s;
        result.rotation.y = (rotationMatrix.at(0, 1) + rotationMatrix.at(1, 0)) / s;
        result.rotation.z = (rotationMatrix.at(0, 2) + rotationMatrix.at(2, 0)) / s;
    } else if (m11 > m22) {
        const f32 s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        result.rotation.w = (rotationMatrix.at(0, 2) - rotationMatrix.at(2, 0)) / s;
        result.rotation.x = (rotationMatrix.at(0, 1) + rotationMatrix.at(1, 0)) / s;
        result.rotation.y = 0.25f * s;
        result.rotation.z = (rotationMatrix.at(1, 2) + rotationMatrix.at(2, 1)) / s;
    } else {
        const f32 s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        result.rotation.w = (rotationMatrix.at(1, 0) - rotationMatrix.at(0, 1)) / s;
        result.rotation.x = (rotationMatrix.at(0, 2) + rotationMatrix.at(2, 0)) / s;
        result.rotation.y = (rotationMatrix.at(1, 2) + rotationMatrix.at(2, 1)) / s;
        result.rotation.z = 0.25f * s;
    }

    result.rotation = normalize(result.rotation);
    return result;
}

Scene::Scene() {
    // Slot 0 is the root, created up front so every other node has somewhere to
    // attach and no caller ever has to handle "the scene is empty".
    // Assigned rather than brace-initialised: a designated initialiser that
    // names only some fields warns, and listing all of them would have to be
    // updated every time Node grows a member.
    Node root;
    root.name = "root";
    m_nodes.push_back(std::move(root));
    m_worldTransforms.push_back(identity());
    m_alive.push_back(true);
}

NodeId Scene::createNode(std::string name, NodeId parent) {
    FUMAR_ASSERT_MSG(isAlive(parent), "createNode called with a dead parent");

    NodeId id;
    if (m_freeSlots.empty()) {
        id = static_cast<NodeId>(m_nodes.size());
        m_nodes.emplace_back();
        m_worldTransforms.push_back(identity());
        m_alive.push_back(true);
    } else {
        // Reusing a slot keeps ids dense, which keeps the parallel arrays
        // compact and the world-transform pass cache-friendly.
        id = m_freeSlots.back();
        m_freeSlots.pop_back();
        m_nodes[id] = Node{};
        m_worldTransforms[id] = identity();
        m_alive[id] = true;
    }

    m_nodes[id].name = std::move(name);
    m_nodes[id].parent = parent;
    m_nodes[parent].children.push_back(id);
    return id;
}

void Scene::destroyNode(NodeId id) {
    if (id == kRootNode) {
        FUMAR_WARN("refusing to destroy the scene root");
        return;
    }
    if (!isAlive(id)) {
        return;
    }

    // Unlink first: the recursive pass below rewrites children as it goes, and
    // removing this node from its parent afterwards would touch freed state.
    detachFromParent(id);
    destroyRecursive(id);
}

void Scene::destroyRecursive(NodeId id) {
    // Copied, because destroying a child would otherwise mutate the vector
    // being iterated.
    const std::vector<NodeId> children = m_nodes[id].children;
    for (NodeId child : children) {
        destroyRecursive(child);
    }

    m_nodes[id] = Node{};
    m_alive[id] = false;
    m_freeSlots.push_back(id);
}

void Scene::detachFromParent(NodeId id) {
    const NodeId parent = m_nodes[id].parent;
    if (parent == kInvalidNode || !isAlive(parent)) {
        return;
    }

    std::vector<NodeId>& siblings = m_nodes[parent].children;
    siblings.erase(std::remove(siblings.begin(), siblings.end(), id), siblings.end());
}

bool Scene::isAlive(NodeId id) const {
    return id < m_alive.size() && m_alive[id];
}

Node& Scene::node(NodeId id) {
    FUMAR_ASSERT_MSG(isAlive(id), "node() on a dead or out-of-range id");
    return m_nodes[id];
}

const Node& Scene::node(NodeId id) const {
    FUMAR_ASSERT_MSG(isAlive(id), "node() on a dead or out-of-range id");
    return m_nodes[id];
}

void Scene::setParent(NodeId id, NodeId newParent) {
    if (id == kRootNode || !isAlive(id) || !isAlive(newParent)) {
        return;
    }

    // Reparenting a node under its own descendant would detach that whole
    // branch from the root and turn it into a cycle - the transform pass would
    // then never terminate.
    for (NodeId ancestor = newParent; ancestor != kInvalidNode; ancestor = m_nodes[ancestor].parent) {
        if (ancestor == id) {
            FUMAR_WARN("refusing to reparent '{}' under its own descendant", m_nodes[id].name);
            return;
        }
    }

    detachFromParent(id);
    m_nodes[id].parent = newParent;
    m_nodes[newParent].children.push_back(id);
}

void Scene::updateWorldTransforms() {
    // Iterative depth-first walk from the root. A parent is always processed
    // before its children, so each node needs one matrix multiply against an
    // already-final parent transform - O(n) for the whole tree.
    //
    // Recursion would read more cleanly but a deep hierarchy could overflow the
    // stack, and scene depth is data, not something the engine controls.
    std::vector<NodeId> stack;
    stack.push_back(kRootNode);
    m_worldTransforms[kRootNode] = m_nodes[kRootNode].transform.matrix();

    while (!stack.empty()) {
        const NodeId current = stack.back();
        stack.pop_back();

        const Mat4& parentWorld = m_worldTransforms[current];
        for (NodeId child : m_nodes[current].children) {
            if (!isAlive(child)) {
                continue;
            }
            m_worldTransforms[child] = parentWorld * m_nodes[child].transform.matrix();
            stack.push_back(child);
        }
    }
}

const Mat4& Scene::worldTransform(NodeId id) const {
    FUMAR_ASSERT_MSG(isAlive(id), "worldTransform() on a dead or out-of-range id");
    return m_worldTransforms[id];
}

void Scene::forEachDrawable(const std::function<void(NodeId, const Node&, const Mat4&)>& visit) const {
    // Visibility is inherited, so a hidden node hides its whole subtree - which
    // means this cannot simply scan the flat array and check the flag.
    std::vector<NodeId> stack;
    stack.push_back(kRootNode);

    while (!stack.empty()) {
        const NodeId current = stack.back();
        stack.pop_back();

        const Node& node = m_nodes[current];
        if (!node.visible) {
            continue;
        }

        if (node.mesh.valid()) {
            visit(current, node, m_worldTransforms[current]);
        }

        for (NodeId child : node.children) {
            if (isAlive(child)) {
                stack.push_back(child);
            }
        }
    }
}

usize Scene::nodeCount() const {
    return m_nodes.size() - m_freeSlots.size();
}

void Scene::traverse(const std::function<void(NodeId, u32 depth)>& visit) const {
    struct Entry {
        NodeId id;
        u32 depth;
    };

    // Children are pushed in reverse so they come off the stack in the order
    // they were added - an editor hierarchy that reshuffles itself between
    // frames would be unusable.
    std::vector<Entry> stack;
    for (auto it = m_nodes[kRootNode].children.rbegin(); it != m_nodes[kRootNode].children.rend(); ++it) {
        stack.push_back(Entry{*it, 0});
    }

    while (!stack.empty()) {
        const Entry entry = stack.back();
        stack.pop_back();

        if (!isAlive(entry.id)) {
            continue;
        }
        visit(entry.id, entry.depth);

        const std::vector<NodeId>& children = m_nodes[entry.id].children;
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            stack.push_back(Entry{*it, entry.depth + 1});
        }
    }
}

} // namespace fumar
