#include "history.hpp"

#include "fumar/core/log.hpp"

#include <utility>

namespace fumar {
namespace {

void captureRecursive(const Scene& scene, NodeId id, i32 parentIndex, NodeId selected,
                      SceneSnapshot& out) {
    const Node& node = scene.node(id);

    const auto index = static_cast<i32>(out.nodes.size());
    out.nodes.push_back(NodeRecord{
        .name = node.name,
        .transform = node.transform,
        .mesh = node.mesh,
        .material = node.material,
        .script = node.script,
        .component = node.component,
        .light = node.light,
        .visible = node.visible,
        .parent = parentIndex,
    });

    if (id == selected) {
        out.selected = index;
    }

    // Depth first, so every parent is written before its children and the
    // rebuild below can go straight through the list.
    for (const NodeId child : node.children) {
        captureRecursive(scene, child, index, selected, out);
    }
}

/// Rebuilds the records under `parent` and returns the new id of each, in
/// record order, so callers can map an index back to a node.
std::vector<NodeId> rebuild(Scene& scene, const SceneSnapshot& snapshot, NodeId parent,
                            bool makeNamesUnique) {
    std::vector<NodeId> created(snapshot.nodes.size(), kInvalidNode);

    for (usize i = 0; i < snapshot.nodes.size(); ++i) {
        const NodeRecord& record = snapshot.nodes[i];

        // -1 means the record was a top-level node; it goes under whatever the
        // caller asked for, which is the scene root for an undo and the paste
        // target for a paste.
        const NodeId parentId = record.parent >= 0 ? created[static_cast<usize>(record.parent)]
                                                   : parent;
        if (parentId == kInvalidNode) {
            continue;
        }

        // Only the roots of what is being pasted need renaming. Renaming
        // children too would rewrite a whole hierarchy for no reason - two
        // objects can perfectly well each own a child called "Wheel".
        const std::string name = (makeNamesUnique && record.parent < 0)
                                     ? scene.uniqueName(record.name)
                                     : record.name;

        const NodeId id = scene.createNode(name, parentId);
        created[i] = id;

        Node& node = scene.node(id);
        node.transform = record.transform;
        node.mesh = record.mesh;
        node.material = record.material;
        node.script = record.script;
        node.component = record.component;
        node.light = record.light;
        node.visible = record.visible;
    }

    return created;
}

/// Empties the scene without touching the root itself.
void clearScene(Scene& scene) {
    // Copied first: destroyNode edits the very list being iterated.
    const std::vector<NodeId> roots = scene.node(kRootNode).children;
    for (const NodeId id : roots) {
        scene.destroyNode(id);
    }
}

NodeId selectionFrom(const SceneSnapshot& snapshot, const std::vector<NodeId>& created) {
    if (snapshot.selected < 0 || static_cast<usize>(snapshot.selected) >= created.size()) {
        return kInvalidNode;
    }
    return created[static_cast<usize>(snapshot.selected)];
}

} // namespace

SceneSnapshot captureScene(const Scene& scene, NodeId selected) {
    SceneSnapshot snapshot;
    for (const NodeId child : scene.node(kRootNode).children) {
        captureRecursive(scene, child, -1, selected, snapshot);
    }
    return snapshot;
}

SceneSnapshot captureSubtree(const Scene& scene, NodeId root) {
    SceneSnapshot snapshot;
    if (root == kInvalidNode || root == kRootNode || !scene.isAlive(root)) {
        return snapshot;
    }
    captureRecursive(scene, root, -1, kInvalidNode, snapshot);
    return snapshot;
}

NodeId pasteInto(Scene& scene, const SceneSnapshot& snapshot, NodeId parent) {
    if (snapshot.empty()) {
        return kInvalidNode;
    }
    const std::vector<NodeId> created = rebuild(scene, snapshot, parent, true);
    return created.empty() ? kInvalidNode : created.front();
}

void History::record(const Scene& scene, NodeId selected) {
    m_undo.push_back(captureScene(scene, selected));

    // Anything that was undone is no longer reachable once a new change is
    // made: the timeline branched, and the branch not taken is dropped. Every
    // editor behaves this way, and the alternative is a tree nobody navigates.
    m_redo.clear();

    if (m_undo.size() > kMaxSteps) {
        m_undo.erase(m_undo.begin());
    }
}

bool History::undo(Scene& scene, NodeId currentSelection, NodeId& restoredSelection) {
    if (m_undo.empty()) {
        return false;
    }

    // The state being left has to go somewhere, or redo would have nothing to
    // return to.
    m_redo.push_back(captureScene(scene, currentSelection));

    const SceneSnapshot snapshot = std::move(m_undo.back());
    m_undo.pop_back();

    clearScene(scene);
    restoredSelection = selectionFrom(snapshot, rebuild(scene, snapshot, kRootNode, false));
    return true;
}

bool History::redo(Scene& scene, NodeId currentSelection, NodeId& restoredSelection) {
    if (m_redo.empty()) {
        return false;
    }

    m_undo.push_back(captureScene(scene, currentSelection));

    const SceneSnapshot snapshot = std::move(m_redo.back());
    m_redo.pop_back();

    clearScene(scene);
    restoredSelection = selectionFrom(snapshot, rebuild(scene, snapshot, kRootNode, false));
    return true;
}

void History::clear() {
    m_undo.clear();
    m_redo.clear();
}

} // namespace fumar
