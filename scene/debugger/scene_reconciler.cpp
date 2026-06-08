/**************************************************************************/
/*  scene_reconciler.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "scene_reconciler.h"

#ifdef DEBUG_ENABLED

#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/variant/callable.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/property_utils.h"
#include "scene/resources/packed_scene.h"

namespace {
// Collect every node belonging to the instance rooted at `p_root` (the root itself and nodes owned
// by it) that carries a unique scene id, mapping id -> node.
void _collect_scene_nodes_by_id(Node *p_root, Node *p_node, HashMap<int32_t, Node *> &r_map) {
	if (p_node == p_root || p_node->get_owner() == p_root) {
		const int32_t id = p_node->get_unique_scene_id();
		if (id != Node::UNIQUE_SCENE_ID_UNASSIGNED) {
			r_map[id] = p_node;
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_collect_scene_nodes_by_id(p_root, p_node->get_child(i), r_map);
	}
}

// Best-effort default value of a property, used to revert an override removed from the scene file.
// Uses PropertyUtils so a script's `@export var x = ...` initializer is honored (the raw ClassDB
// default skips the script layer, reverting script-exported properties to the wrong value).
Variant _scene_property_default(Node *p_node, const StringName &p_prop) {
	bool valid = false;
	const Variant def = PropertyUtils::get_property_default_value(p_node, p_prop, &valid);
	if (valid) {
		return def;
	}
	if (p_node->property_can_revert(p_prop)) {
		return p_node->property_get_revert(p_prop);
	}
	return Variant();
}

// Build the callable a persistent connection resolves to (target method, plus any binds/unbinds).
Callable _make_connection_callable(Node *p_target, const SceneReconciler::ConnectionInfo &p_conn) {
	Callable callable(p_target, p_conn.method);
	if (!p_conn.binds.is_empty()) {
		callable = callable.bindv(p_conn.binds);
	}
	if (p_conn.unbinds > 0) {
		callable = callable.unbind(p_conn.unbinds);
	}
	return callable;
}

// Find an existing connection of `p_signal` on `p_source` whose callable targets `p_method` on
// `p_target`, ignoring bound-argument equality. A rebuilt bound Callable may not compare equal to the
// one created at instantiation, so matching on (target, method) avoids both duplicate connects (a
// missed is_connected()) and leaked connections (a failed disconnect()). The connection key already
// treats source|signal|target|method as the identity, so this matches that granularity.
bool _find_signal_connection(Node *p_source, const StringName &p_signal, const Object *p_target, const StringName &p_method, Callable *r_callable = nullptr) {
	List<Object::Connection> conns;
	p_source->get_signal_connection_list(p_signal, &conns);
	for (const Object::Connection &c : conns) {
		if (c.callable.get_object() == p_target && c.callable.get_method() == p_method) {
			if (r_callable) {
				*r_callable = c.callable;
			}
			return true;
		}
	}
	return false;
}

void _apply_node_properties(Node *p_node, const HashMap<StringName, Variant> &p_props) {
	for (const KeyValue<StringName, Variant> &p : p_props) {
		p_node->set(p.key, p.value);
	}
}

void _apply_node_groups(Node *p_node, const HashSet<StringName> &p_groups) {
	for (const StringName &g : p_groups) {
		if (!p_node->is_in_group(g)) {
			p_node->add_to_group(g, true);
		}
	}
}

// Read a state node's explicitly-set properties and persistent groups (used for both id'd and id-less
// nodes).
void _read_node_props_groups(const Ref<SceneState> &p_state, int p_idx, HashMap<StringName, Variant> &r_props, HashSet<StringName> &r_groups) {
	for (int j = 0; j < p_state->get_node_property_count(p_idx); j++) {
		r_props[p_state->get_node_property_name(p_idx, j)] = p_state->get_node_property_value(p_idx, j);
	}
	for (const StringName &g : p_state->get_node_groups(p_idx)) {
		r_groups.insert(g);
	}
}

// Structural description of a node that carries a unique id, in scene (parents-first) order.
struct DesiredNode {
	int32_t id = Node::UNIQUE_SCENE_ID_UNASSIGNED;
	int32_t parent_id = Node::UNIQUE_SCENE_ID_UNASSIGNED;
	bool is_root = false;
	// True when this scene authors the node itself (it has a real type), as opposed to an inherited
	// base node or override (TYPE_INSTANTIATED, empty type). Gates structural edits for inherited
	// scenes: only derived-added nodes may be removed/reparented/renamed there.
	bool derived_added = false;
	StringName name;
	StringName type;
	Ref<PackedScene> instance;
	NodePath parent_path; // Authored parent path; fallback when the parent carries no unique id.
};

// Resolve the live parent for a desired node: prefer its unique id, fall back to its authored path so
// an id'd node whose parent has no unique id (e.g. under a nested foreign instance) can still be
// created/reparented. Returns nullptr if the parent can't be located.
Node *_resolve_desired_parent(const DesiredNode &p_dn, Node *p_instance_root, int32_t p_root_id, const HashMap<int32_t, Node *> &p_live) {
	if (p_dn.parent_id == p_root_id) {
		return p_instance_root;
	}
	if (p_dn.parent_id != Node::UNIQUE_SCENE_ID_UNASSIGNED) {
		Node *const *n = p_live.getptr(p_dn.parent_id);
		return n ? *n : nullptr; // Parent has an id but isn't live; don't guess by path.
	}
	if (!p_dn.parent_path.is_empty()) {
		return p_instance_root->get_node_or_null(p_dn.parent_path);
	}
	return nullptr;
}

// A node without a unique id: reconciled best-effort by path (properties and groups only).
struct UnassignedNode {
	NodePath path;
	HashMap<StringName, Variant> props;
	HashSet<StringName> groups;
};
} // namespace

String SceneReconciler::ConnectionInfo::key() const {
	return String(source) + "|" + String(signal) + "|" + String(target) + "|" + String(method);
}

SceneReconciler::SceneSnapshot SceneReconciler::_build_snapshot(const Ref<SceneState> &p_state) {
	SceneSnapshot snapshot;
	if (p_state.is_null()) {
		return snapshot;
	}

	for (int i = 0; i < p_state->get_node_count(); i++) {
		const int32_t id = p_state->get_node_unique_id(i);
		if (id == Node::UNIQUE_SCENE_ID_UNASSIGNED) {
			continue;
		}
		// A non-empty type means the scene authors this node itself; an empty type is an inherited
		// base node / override (TYPE_INSTANTIATED). The root (i == 0) is never a structural candidate.
		if (i != 0 && !p_state->get_node_type(i).is_empty()) {
			snapshot.derived_added_ids.insert(id);
		}
		HashMap<StringName, Variant> props;
		HashSet<StringName> groups;
		_read_node_props_groups(p_state, i, props, groups);
		snapshot.node_props[id] = props;
		snapshot.node_groups[id] = groups;
	}

	for (int i = 0; i < p_state->get_connection_count(); i++) {
		ConnectionInfo conn;
		conn.source = p_state->get_connection_source(i);
		conn.signal = p_state->get_connection_signal(i);
		conn.target = p_state->get_connection_target(i);
		conn.method = p_state->get_connection_method(i);
		conn.binds = p_state->get_connection_binds(i);
		conn.unbinds = p_state->get_connection_unbinds(i);
		conn.flags = p_state->get_connection_flags(i);
		snapshot.connections[conn.key()] = conn;
	}

	return snapshot;
}

SceneReconciler::SceneSnapshot SceneReconciler::reconcile_against_state(const LocalVector<Node *> &p_instances, const Ref<SceneState> &p_state, const SceneSnapshot &p_previous) {
	SceneSnapshot new_snapshot = _build_snapshot(p_state);
	if (p_state.is_null()) {
		return new_snapshot;
	}

	const int node_count = p_state->get_node_count();
	if (node_count == 0) {
		return new_snapshot;
	}

	// Build the desired structure. Id'd nodes drive structural reconciliation; nodes without an id
	// (e.g. inside nested foreign instances, or scenes predating unique ids) are handled best-effort
	// by path for properties and groups only.
	HashMap<NodePath, int32_t> path_to_id;
	for (int i = 0; i < node_count; i++) {
		path_to_id[p_state->get_node_path(i)] = p_state->get_node_unique_id(i);
	}

	LocalVector<DesiredNode> desired; // Parents before children.
	HashMap<int32_t, uint32_t> desired_index;
	LocalVector<UnassignedNode> unassigned;
	for (int i = 0; i < node_count; i++) {
		const int32_t id = p_state->get_node_unique_id(i);
		if (id == Node::UNIQUE_SCENE_ID_UNASSIGNED) {
			UnassignedNode un;
			un.path = p_state->get_node_path(i);
			_read_node_props_groups(p_state, i, un.props, un.groups);
			unassigned.push_back(un);
			continue;
		}

		DesiredNode dn;
		dn.id = id;
		dn.is_root = (i == 0);
		dn.name = p_state->get_node_name(i);
		dn.type = p_state->get_node_type(i);
		dn.instance = p_state->get_node_instance(i);
		dn.derived_added = !dn.is_root && !String(dn.type).is_empty();
		if (!dn.is_root) {
			dn.parent_path = p_state->get_node_path(i, true);
			if (path_to_id.has(dn.parent_path)) {
				dn.parent_id = path_to_id[dn.parent_path];
			}
		}

		desired_index[id] = desired.size();
		desired.push_back(dn);
	}

	// Inherited scenes don't enumerate unchanged base nodes in this state, so a node being "absent"
	// can't be read as "deleted" for base nodes. `allow_structural_changes` is true only for a
	// non-inherited scene, where the state fully describes the tree and any structural change is safe.
	// For an inherited scene it is false, and structural edits are instead restricted to the nodes the
	// derived scene authors itself (`DesiredNode::derived_added` / the previous snapshot's
	// `derived_added_ids`): those can be removed/reparented/renamed, while inherited base nodes are
	// never touched structurally. (Base nodes can't be deleted or renamed through the editor anyway,
	// and a base-node reparent isn't persisted to the inherited scene file.)
	const bool allow_structural_changes = p_state->get_base_scene_state().is_null();

	SceneTree *scene_tree = SceneTree::get_singleton();

	for (Node *instance_root : p_instances) {
		HashMap<int32_t, Node *> live;
		_collect_scene_nodes_by_id(instance_root, instance_root, live);
		const int32_t root_id = instance_root->get_unique_scene_id();

		// 1) Remove nodes that no longer exist (only subtree roots; children go with their parent).
		// A live id'd node absent from the scene file is a removal candidate. For inherited scenes we
		// additionally require it to have been a derived-added node last pass (`derived_added_ids`):
		// inherited base nodes aren't enumerated in the state, so an absent base node must never be
		// treated as a deletion. Base nodes are never in `derived_added_ids`, so they are never removed.
		LocalVector<Node *> to_remove;
		auto is_removal_candidate = [&](int32_t p_id) {
			if (p_id == root_id || desired_index.has(p_id)) {
				return false;
			}
			return allow_structural_changes || p_previous.derived_added_ids.has(p_id);
		};
		for (const KeyValue<int32_t, Node *> &kv : live) {
			if (!is_removal_candidate(kv.key)) {
				continue;
			}
			bool ancestor_removed = false;
			for (Node *p = kv.value->get_parent(); p && p != instance_root; p = p->get_parent()) {
				const int32_t pid = p->get_unique_scene_id();
				// Skip this node only if an ancestor is itself a removal candidate (it will be freed,
				// taking this subtree with it). Using the same predicate keeps a non-candidate base
				// ancestor from suppressing a legitimate derived-node removal.
				if (pid != Node::UNIQUE_SCENE_ID_UNASSIGNED && live.has(pid) && is_removal_candidate(pid)) {
					ancestor_removed = true;
					break;
				}
			}
			if (!ancestor_removed) {
				to_remove.push_back(kv.value);
			}
		}
		for (Node *n : to_remove) {
			live.erase(n->get_unique_scene_id());
			n->queue_free();
		}

		// 2) Add new nodes (desired is parent-first, so parents are created before their children).
		for (const DesiredNode &dn : desired) {
			if (dn.is_root || live.has(dn.id)) {
				continue;
			}
			Node *parent = _resolve_desired_parent(dn, instance_root, root_id, live);
			if (!parent) {
				continue;
			}
			Node *node = nullptr;
			if (dn.instance.is_valid()) {
				node = dn.instance->instantiate();
			} else if (!String(dn.type).is_empty()) {
				node = Object::cast_to<Node>(ClassDB::instantiate(dn.type));
			}
			if (!node) {
				continue;
			}
			node->set_name(dn.name);
			parent->add_child(node);
			node->set_owner(instance_root);
			node->set_unique_scene_id(dn.id);
			// Properties and groups are applied uniformly in pass 4 (this node is now in `live`).
			// Applying them here too would double-fire non-idempotent setters on a single add.
			live[dn.id] = node;
		}

		// 3) Reparent / rename existing nodes whose place in the tree changed. For inherited scenes this
		// is restricted to derived-added nodes: base nodes can't be renamed/reparented through the
		// editor, and the inherited state may not fully describe the base tree (see
		// `allow_structural_changes`).
		for (const DesiredNode &dn : desired) {
			if (dn.is_root || !live.has(dn.id)) {
				continue;
			}
			if (!allow_structural_changes && !dn.derived_added) {
				continue;
			}
			Node *node = live[dn.id];
			Node *want_parent = _resolve_desired_parent(dn, instance_root, root_id, live);
			if (want_parent && node->get_parent() != want_parent) {
				node->reparent(want_parent, false);
			}
			if (node->get_name() != dn.name) {
				node->set_name(dn.name);
			}
		}

		// 4) Apply property values and groups, and revert overrides removed from the scene file.
		for (const DesiredNode &dn : desired) {
			if (!live.has(dn.id)) {
				continue;
			}
			Node *node = live[dn.id];

			// Preserve the transform of a nested instance root: its placement is owned by the parent
			// scene, not its own scene file (see GH-86659). Capture before, restore after, so any
			// transform-affecting property is covered.
			const bool keep_transform = (node == instance_root) && scene_tree && (node->get_parent() != scene_tree->get_root());
			int transform_kind = 0;
			Variant saved_transform;
			if (keep_transform) {
				if (node->is_class("Node3D")) {
					transform_kind = 1;
					saved_transform = node->call("get_transform");
				} else if (node->is_class("CanvasItem")) {
					transform_kind = 2;
					saved_transform = node->call("_edit_get_state");
				}
			}

			if (const HashMap<StringName, Variant> *props = new_snapshot.node_props.getptr(dn.id)) {
				_apply_node_properties(node, *props);
			}

			// Revert properties that were overridden before but are no longer in the scene.
			if (const HashMap<StringName, Variant> *old_props = p_previous.node_props.getptr(dn.id)) {
				const HashMap<StringName, Variant> *new_props = new_snapshot.node_props.getptr(dn.id);
				for (const KeyValue<StringName, Variant> &p : *old_props) {
					if (!new_props || !new_props->has(p.key)) {
						node->set(p.key, _scene_property_default(node, p.key));
					}
				}
			}

			// Reconcile persistent groups: add new ones, remove ones no longer in the scene.
			if (const HashSet<StringName> *groups = new_snapshot.node_groups.getptr(dn.id)) {
				_apply_node_groups(node, *groups);
				if (const HashSet<StringName> *old_groups = p_previous.node_groups.getptr(dn.id)) {
					for (const StringName &g : *old_groups) {
						if (!groups->has(g) && node->is_in_group(g)) {
							node->remove_from_group(g);
						}
					}
				}
			}

			if (transform_kind == 1) {
				node->call("set_transform", saved_transform);
			} else if (transform_kind == 2) {
				node->call("_edit_set_state", saved_transform);
			}
		}

		// 5) Reorder siblings to match the scene's child order. Skipped for inherited scenes (the
		// sibling order in this state may not account for inherited base nodes). A parent may also hold
		// id-less or pending-free children, so the desired sibling index (a dense rank over id'd nodes)
		// can't be used as an absolute live child index. Instead, reorder the id'd children *within the
		// slots they already occupy*: collect their current indices, sort them, and reassign them to
		// the id'd children in desired (file) order. This leaves id-less/pending-free siblings in place.
		if (allow_structural_changes) {
			HashMap<Node *, LocalVector<Node *>> ordered_children; // parent -> id'd children, file order.
			for (const DesiredNode &dn : desired) {
				if (dn.is_root || !live.has(dn.id)) {
					continue;
				}
				Node *node = live[dn.id];
				if (Node *parent = node->get_parent()) {
					ordered_children[parent].push_back(node);
				}
			}
			for (const KeyValue<Node *, LocalVector<Node *>> &kv : ordered_children) {
				LocalVector<int> slots;
				for (Node *n : kv.value) {
					slots.push_back(n->get_index());
				}
				slots.sort();
				for (uint32_t k = 0; k < kv.value.size(); k++) {
					if (kv.value[k]->get_index() != slots[k]) {
						kv.key->move_child(kv.value[k], slots[k]);
					}
				}
			}
		}

		// 6) Best-effort update of nodes without a unique id, matched by path (no structural changes).
		for (const UnassignedNode &un : unassigned) {
			Node *node = instance_root->get_node_or_null(un.path);
			if (!node) {
				continue;
			}
			_apply_node_properties(node, un.props);
			_apply_node_groups(node, un.groups);
		}

		// 7) Reconcile persistent signal connections: connect new ones, disconnect removed ones.
		// Identity is matched on (target, method) rather than full Callable equality, since a rebuilt
		// bound Callable may not compare equal to the one made at instantiation (see
		// `_find_signal_connection`).
		for (const KeyValue<String, ConnectionInfo> &kv : new_snapshot.connections) {
			Node *source = instance_root->get_node_or_null(kv.value.source);
			Node *target = instance_root->get_node_or_null(kv.value.target);
			if (!source || !target) {
				continue;
			}
			if (!_find_signal_connection(source, kv.value.signal, target, kv.value.method)) {
				source->connect(kv.value.signal, _make_connection_callable(target, kv.value), kv.value.flags);
			}
		}
		for (const KeyValue<String, ConnectionInfo> &kv : p_previous.connections) {
			if (new_snapshot.connections.has(kv.key)) {
				continue;
			}
			Node *source = instance_root->get_node_or_null(kv.value.source);
			Node *target = instance_root->get_node_or_null(kv.value.target);
			if (!source || !target) {
				continue;
			}
			Callable existing;
			if (_find_signal_connection(source, kv.value.signal, target, kv.value.method, &existing)) {
				source->disconnect(kv.value.signal, existing);
			}
		}
	}

	return new_snapshot;
}

void SceneReconciler::seed_snapshot(const String &p_scene_path) {
	if (snapshots.has(p_scene_path)) {
		return;
	}
	Ref<PackedScene> packed_scene = ResourceCache::get_ref(p_scene_path);
	if (packed_scene.is_null()) {
		// A running game typically releases the PackedScene after instantiating it, so it is usually
		// NOT in the resource cache. Load it from disk for the baseline -- the same fallback reconcile()
		// uses. Without this the snapshot is never seeded, so a derived node removed from an inherited
		// scene is never recognized as a deletion (its id is absent from the empty `derived_added_ids`).
		packed_scene = ResourceLoader::load(p_scene_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE);
	}
	if (packed_scene.is_null()) {
		return;
	}
	snapshots[p_scene_path] = _build_snapshot(packed_scene->get_state());
}

void SceneReconciler::reconcile(const String &p_scene_path, const LocalVector<Node *> &p_instances) {
	// Refresh the PackedScene from disk so we reconcile against the latest on-disk state.
	Ref<PackedScene> packed_scene = ResourceCache::get_ref(p_scene_path);
	if (packed_scene.is_valid()) {
		packed_scene->reload_from_file();
	} else {
		packed_scene = ResourceLoader::load(p_scene_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE);
	}
	if (packed_scene.is_null()) {
		return;
	}

	SceneSnapshot previous;
	if (HashMap<String, SceneSnapshot>::ConstIterator it = snapshots.find(p_scene_path)) {
		previous = it->value;
	}
	snapshots[p_scene_path] = reconcile_against_state(p_instances, packed_scene->get_state(), previous);
}

#endif // DEBUG_ENABLED
