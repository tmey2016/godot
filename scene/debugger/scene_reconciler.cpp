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
Variant _scene_property_default(Node *p_node, const StringName &p_prop) {
	bool valid = false;
	const Variant def = ClassDB::class_get_default_property_value(p_node->get_class_name(), p_prop, &valid);
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

// Structural description of a node that carries a unique id, in scene (parents-first) order.
struct DesiredNode {
	int32_t id = Node::UNIQUE_SCENE_ID_UNASSIGNED;
	int32_t parent_id = Node::UNIQUE_SCENE_ID_UNASSIGNED;
	bool is_root = false;
	StringName name;
	StringName type;
	Ref<PackedScene> instance;
	int sibling_index = 0;
};

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
		HashMap<StringName, Variant> props;
		for (int j = 0; j < p_state->get_node_property_count(i); j++) {
			props[p_state->get_node_property_name(i, j)] = p_state->get_node_property_value(i, j);
		}
		snapshot.node_props[id] = props;

		HashSet<StringName> groups;
		for (const StringName &g : p_state->get_node_groups(i)) {
			groups.insert(g);
		}
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
	HashMap<int32_t, int> sibling_counter;
	LocalVector<UnassignedNode> unassigned;
	for (int i = 0; i < node_count; i++) {
		const int32_t id = p_state->get_node_unique_id(i);
		if (id == Node::UNIQUE_SCENE_ID_UNASSIGNED) {
			UnassignedNode un;
			un.path = p_state->get_node_path(i);
			for (int j = 0; j < p_state->get_node_property_count(i); j++) {
				un.props[p_state->get_node_property_name(i, j)] = p_state->get_node_property_value(i, j);
			}
			for (const StringName &g : p_state->get_node_groups(i)) {
				un.groups.insert(g);
			}
			unassigned.push_back(un);
			continue;
		}

		DesiredNode dn;
		dn.id = id;
		dn.is_root = (i == 0);
		dn.name = p_state->get_node_name(i);
		dn.type = p_state->get_node_type(i);
		dn.instance = p_state->get_node_instance(i);
		if (!dn.is_root) {
			const NodePath parent_path = p_state->get_node_path(i, true);
			if (path_to_id.has(parent_path)) {
				dn.parent_id = path_to_id[parent_path];
			}
		}
		int *cnt = sibling_counter.getptr(dn.parent_id);
		dn.sibling_index = cnt ? *cnt : 0;
		sibling_counter[dn.parent_id] = dn.sibling_index + 1;

		desired_index[id] = desired.size();
		desired.push_back(dn);
	}

	// Inherited scenes may not enumerate their inherited base nodes in this state, so removing or
	// reparenting nodes merely because they are "absent" is unsafe (it could delete base nodes).
	// Restrict inherited scenes to additive reconciliation (properties, groups, connections, adds).
	const bool allow_structural_changes = p_state->get_base_scene_state().is_null();

	SceneTree *scene_tree = SceneTree::get_singleton();

	for (Node *instance_root : p_instances) {
		HashMap<int32_t, Node *> live;
		_collect_scene_nodes_by_id(instance_root, instance_root, live);
		const int32_t root_id = instance_root->get_unique_scene_id();

		// 1) Remove nodes that no longer exist (only subtree roots; children go with their parent).
		LocalVector<Node *> to_remove;
		for (const KeyValue<int32_t, Node *> &kv : live) {
			if (!allow_structural_changes) {
				break; // Never remove from an inherited scene (see `allow_structural_changes`).
			}
			if (kv.key == root_id || desired_index.has(kv.key)) {
				continue;
			}
			bool ancestor_removed = false;
			for (Node *p = kv.value->get_parent(); p && p != instance_root; p = p->get_parent()) {
				const int32_t pid = p->get_unique_scene_id();
				if (pid != Node::UNIQUE_SCENE_ID_UNASSIGNED && live.has(pid) && !desired_index.has(pid)) {
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
			Node *parent = (dn.parent_id == root_id) ? instance_root : (live.has(dn.parent_id) ? live[dn.parent_id] : nullptr);
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
			if (const HashMap<StringName, Variant> *props = new_snapshot.node_props.getptr(dn.id)) {
				_apply_node_properties(node, *props);
			}
			if (const HashSet<StringName> *groups = new_snapshot.node_groups.getptr(dn.id)) {
				_apply_node_groups(node, *groups);
			}
			live[dn.id] = node;
		}

		// 3) Reparent / rename existing nodes whose place in the tree changed. Skipped for inherited
		// scenes, whose state may not fully describe the tree (see `allow_structural_changes`).
		for (const DesiredNode &dn : desired) {
			if (!allow_structural_changes) {
				break;
			}
			if (dn.is_root || !live.has(dn.id)) {
				continue;
			}
			Node *node = live[dn.id];
			Node *want_parent = (dn.parent_id == root_id) ? instance_root : (live.has(dn.parent_id) ? live[dn.parent_id] : nullptr);
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
		// sibling order in this state may not account for inherited base nodes).
		for (const DesiredNode &dn : desired) {
			if (!allow_structural_changes) {
				break;
			}
			if (dn.is_root || !live.has(dn.id)) {
				continue;
			}
			Node *node = live[dn.id];
			Node *parent = node->get_parent();
			if (parent && dn.sibling_index < parent->get_child_count()) {
				parent->move_child(node, dn.sibling_index);
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
		for (const KeyValue<String, ConnectionInfo> &kv : new_snapshot.connections) {
			Node *source = instance_root->get_node_or_null(kv.value.source);
			Node *target = instance_root->get_node_or_null(kv.value.target);
			if (!source || !target) {
				continue;
			}
			const Callable callable = _make_connection_callable(target, kv.value);
			if (!source->is_connected(kv.value.signal, callable)) {
				source->connect(kv.value.signal, callable, kv.value.flags);
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
			const Callable callable = _make_connection_callable(target, kv.value);
			if (source->is_connected(kv.value.signal, callable)) {
				source->disconnect(kv.value.signal, callable);
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
