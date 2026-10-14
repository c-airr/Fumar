#pragma once

#include "fumar/render/resources.hpp"
#include "fumar/scene/scene.hpp"

#include <functional>
#include <vector>

namespace fumar {

/// One node, flattened: no ids anywhere, and the parent link is an INDEX into
/// the list rather than a NodeId.
///
/// That is the whole trick. A NodeId is only meaningful while the node it names
/// is alive, so anything that has to outlive the node - an undo step, a
/// clipboard - cannot store one. Indices into a self-contained list survive
/// the scene being torn down and rebuilt.
struct NodeRecord {
    std::string name;
    Transform transform;
    MeshHandle mesh;
    MaterialHandle material;
    std::string script;
    std::string component;
    std::optional<Light> light;
    bool visible = true;

    /// Index of the parent within the same list, or -1 for the scene root.
    /// Records are stored depth first, so a parent always comes before its
    /// children and one forward pass is enough to rebuild the tree.
    i32 parent = -1;
};

/// A scene, or a piece of one, detached from the scene it came from.
struct SceneSnapshot {
    std::vector<NodeRecord> nodes;

    /// Which record was selected when this was taken, or -1. Restoring puts the
    /// selection back, so undo does not silently drop what you were working on.
    i32 selected = -1;

    bool empty() const { return nodes.empty(); }
};

/// Copies the whole scene.
SceneSnapshot captureScene(const Scene& scene, NodeId selected = kInvalidNode);

/// Copies one node and everything under it. This is what Ctrl+C stores.
SceneSnapshot captureSubtree(const Scene& scene, NodeId root);

/// Adds the snapshot's nodes under `parent` and returns the first one created -
/// the root of what was pasted.
///
/// Names are made unique against what is already there, so pasting twice does
/// not leave two identical rows in the outliner.
NodeId pasteInto(Scene& scene, const SceneSnapshot& snapshot, NodeId parent = kRootNode);

/// Undo and redo, over whole-scene snapshots.
///
/// A command pattern - every action knowing how to reverse itself - is the
/// textbook answer and the right one for a scene of a hundred thousand objects.
/// This is the other answer: snapshot everything, restore wholesale. It is a
/// few hundred lines less, it cannot get out of step with the actions (there is
/// nothing to keep in step), and at the scale this editor works at, copying the
/// node list costs less than the frame it happens in.
///
/// The cost is real and worth stating: it is linear in the size of the scene,
/// per edit. When that starts to hurt, this is the class to replace.
class History {
public:
    /// The materials to keep in step with the scene: each step remembers them
    /// as well as the nodes, and undo puts them back.
    ///
    /// A material is not a node, but editing one is a change to the scene like
    /// any other, and a person pressing Ctrl+Z after dragging a roughness
    /// slider expects the slider to move back. `changed` is told about every
    /// material put back, so the renderer can rewrite its GPU copy. Until this
    /// is called, materials are not recorded.
    void trackMaterials(ResourceRegistry* registry, std::function<void(MaterialHandle)> changed);

    /// Records the state BEFORE a change. Call it immediately before mutating -
    /// at the moment a drag begins, not while it continues, or a single gizmo
    /// pull would fill the stack with a hundred steps.
    void record(const Scene& scene, NodeId selected);

    /// Restores the previous state. Returns the node that was selected then, so
    /// the caller can put the selection back.
    bool undo(Scene& scene, NodeId currentSelection, NodeId& restoredSelection);
    bool redo(Scene& scene, NodeId currentSelection, NodeId& restoredSelection);

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }

    /// Drops everything. Loading a scene invalidates every step, because the
    /// mesh and material handles a snapshot holds belong to the old one.
    void clear();

    usize depth() const { return m_undo.size(); }

    /// How many steps have ever been recorded. Unlike depth() it only grows -
    /// undo does not take it back down, and the oldest step falling off the
    /// bottom does not either - so comparing two readings says whether
    /// anything was recorded in between.
    u64 recordCount() const { return m_records; }

private:
    /// Old steps fall off the bottom. Deep enough that nobody reaches it in
    /// practice, shallow enough that a long session does not grow without
    /// bound.
    static constexpr usize kMaxSteps = 128;

    /// One step: the nodes, and every material as it stood. Materials are
    /// small, and copying the lot keeps a step self-contained - the same trade
    /// the node snapshots make.
    struct Step {
        SceneSnapshot scene;
        std::vector<Material> materials;
    };

    Step capture(const Scene& scene, NodeId selected) const;

    /// Puts a step's materials back. Materials made after the step - past the
    /// end of its list - are left as they are: nothing uses them in the scene
    /// being restored.
    void restoreMaterials(const Step& step);

    std::vector<Step> m_undo;
    std::vector<Step> m_redo;

    ResourceRegistry* m_registry = nullptr;
    std::function<void(MaterialHandle)> m_materialChanged;
    u64 m_records = 0;
};

} // namespace fumar
