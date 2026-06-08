/**************************************************************************/
/*  test_scene_reconciler.cpp                                             */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_scene_reconciler)

#ifdef DEBUG_ENABLED

#include "core/object/class_db.h"
#include "core/variant/callable.h"
#include "scene/2d/node_2d.h"
#include "scene/debugger/scene_reconciler.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/packed_scene.h"

namespace TestSceneReconciler {

// Creates a node of type `T` with the given name and unique scene id. When `p_parent` is non-null the
// node is added to it and owned by the scene root (so it is packed), mirroring how the editor authors
// a scene. Returns the node so the caller can set extra properties / wire connections inline.
template <typename T = Node>
static T *make_node(Node *p_parent, const String &p_name, int32_t p_id) {
	T *node = memnew(T);
	node->set_name(p_name);
	node->set_unique_scene_id(p_id);
	if (p_parent) {
		p_parent->add_child(node);
		Node *owner = p_parent->get_owner();
		node->set_owner(owner ? owner : p_parent);
	}
	return node;
}

// Packs `p_root` (which the caller owns) and returns the packed scene; its `SceneState` is the
// "edited on disk" version a reconcile runs against.
static Ref<PackedScene> pack(Node *p_root) {
	Ref<PackedScene> packed_scene;
	packed_scene.instantiate();
	const Error err = packed_scene->pack(p_root);
	CHECK(err == OK);
	return packed_scene;
}

// Instantiates `p_scene` and adds it to the scene tree as a running instance.
static Node *instantiate_in_tree(const Ref<PackedScene> &p_scene) {
	Node *instance = p_scene->instantiate();
	SceneTree::get_singleton()->get_root()->add_child(instance);
	return instance;
}

// Removes a running instance from the tree and frees it.
static void destroy_instance(Node *p_instance) {
	SceneTree::get_singleton()->get_root()->remove_child(p_instance);
	memdelete(p_instance);
}

// Reconciles `p_instance` against `p_state`, returning the resulting snapshot.
static SceneReconciler::SceneSnapshot reconcile(Node *p_instance, const Ref<SceneState> &p_state, const SceneReconciler::SceneSnapshot &p_previous) {
	LocalVector<Node *> instances;
	instances.push_back(p_instance);
	return SceneReconciler::reconcile_against_state(instances, p_state, p_previous);
}

// Counts the connections of `p_signal` on `p_source` whose callable resolves to `p_method`, ignoring
// bound-argument equality (the granularity the reconciler treats as a connection's identity).
static int count_connections(Node *p_source, const StringName &p_signal, const StringName &p_method) {
	List<Object::Connection> conns;
	p_source->get_signal_connection_list(p_signal, &conns);
	int count = 0;
	for (const Object::Connection &c : conns) {
		if (c.callable.get_method() == p_method) {
			count++;
		}
	}
	return count;
}

// A node whose `value` setter has an observable side effect (a call counter), so a test can detect a
// property being applied more than once during a single reconcile.
class _ReconcilerSetterProbe : public Node {
	GDCLASS(_ReconcilerSetterProbe, Node);

	int value = 0;
	int set_count = 0;

protected:
	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("set_value", "value"), &_ReconcilerSetterProbe::set_value);
		ClassDB::bind_method(D_METHOD("get_value"), &_ReconcilerSetterProbe::get_value);
		ADD_PROPERTY(PropertyInfo(Variant::INT, "value"), "set_value", "get_value");
	}

public:
	void set_value(int p_value) {
		value = p_value;
		set_count++;
	}
	int get_value() const { return value; }
	int get_set_count() const { return set_count; }
};

TEST_CASE("[SceneTree][SceneReconciler] Applies a changed property value") {
	// v1: root -> child @ (1, 1).
	Node *root1 = make_node(nullptr, "Root", 1);
	make_node<Node2D>(root1, "Child", 2)->set_position(Vector2(1, 1));
	Node *instance = instantiate_in_tree(pack(root1));
	memdelete(root1);

	// v2: same node ids, child @ (2, 2).
	Node *root2 = make_node(nullptr, "Root", 1);
	make_node<Node2D>(root2, "Child", 2)->set_position(Vector2(2, 2));
	Ref<PackedScene> ps2 = pack(root2);
	memdelete(root2);

	reconcile(instance, ps2->get_state(), SceneReconciler::SceneSnapshot());

	Node2D *live_child = Object::cast_to<Node2D>(instance->get_child(0));
	CHECK(live_child != nullptr);
	CHECK(live_child->get_position() == Vector2(2, 2));

	destroy_instance(instance);
}

TEST_CASE("[SceneTree][SceneReconciler] Reverts a removed property override to default") {
	// v1: child @ (5, 5) (an override).
	Node *root1 = make_node(nullptr, "Root", 1);
	make_node<Node2D>(root1, "Child", 2)->set_position(Vector2(5, 5));
	Ref<PackedScene> ps1 = pack(root1);
	Node *instance = instantiate_in_tree(ps1);
	memdelete(root1);

	// v2: child at default position (override removed, so not stored).
	Node *root2 = make_node(nullptr, "Root", 1);
	make_node<Node2D>(root2, "Child", 2);
	Ref<PackedScene> ps2 = pack(root2);
	memdelete(root2);

	// First pass records the (5, 5) override; second pass must revert it to the default.
	SceneReconciler::SceneSnapshot snapshot = reconcile(instance, ps1->get_state(), SceneReconciler::SceneSnapshot());
	reconcile(instance, ps2->get_state(), snapshot);

	Node2D *live_child = Object::cast_to<Node2D>(instance->get_child(0));
	CHECK(live_child != nullptr);
	CHECK(live_child->get_position() == Vector2(0, 0));

	destroy_instance(instance);
}

TEST_CASE("[SceneTree][SceneReconciler] Adds a new node") {
	// v1: root -> A.
	Node *root1 = make_node(nullptr, "Root", 1);
	make_node(root1, "A", 2);
	Node *instance = instantiate_in_tree(pack(root1));
	memdelete(root1);

	// v2: root -> A, B (B is new).
	Node *root2 = make_node(nullptr, "Root", 1);
	make_node(root2, "A", 2);
	make_node(root2, "B", 3);
	Ref<PackedScene> ps2 = pack(root2);
	memdelete(root2);

	reconcile(instance, ps2->get_state(), SceneReconciler::SceneSnapshot());

	CHECK(instance->get_child_count() == 2);
	CHECK(instance->get_node_or_null(NodePath("B")) != nullptr);

	destroy_instance(instance);
}

TEST_CASE("[SceneTree][SceneReconciler] Removes a deleted node") {
	// v1: root -> A, B.
	Node *root1 = make_node(nullptr, "Root", 1);
	make_node(root1, "A", 2);
	make_node(root1, "B", 3);
	Node *instance = instantiate_in_tree(pack(root1));
	memdelete(root1);

	// v2: root -> A (B removed).
	Node *root2 = make_node(nullptr, "Root", 1);
	make_node(root2, "A", 2);
	Ref<PackedScene> ps2 = pack(root2);
	memdelete(root2);

	Node *live_b = instance->get_node_or_null(NodePath("B"));
	CHECK(live_b != nullptr);

	reconcile(instance, ps2->get_state(), SceneReconciler::SceneSnapshot());

	// Removal is deferred via queue_free().
	CHECK(live_b->is_queued_for_deletion());

	destroy_instance(instance);
}

TEST_CASE("[SceneTree][SceneReconciler] Reparents a moved node") {
	// v1: root -> A -> B.
	Node *root1 = make_node(nullptr, "Root", 1);
	Node *a1 = make_node(root1, "A", 2);
	make_node(a1, "B", 3);
	Node *instance = instantiate_in_tree(pack(root1));
	memdelete(root1);

	// v2: root -> A, B (B moved up to root).
	Node *root2 = make_node(nullptr, "Root", 1);
	make_node(root2, "A", 2);
	make_node(root2, "B", 3);
	Ref<PackedScene> ps2 = pack(root2);
	memdelete(root2);

	reconcile(instance, ps2->get_state(), SceneReconciler::SceneSnapshot());

	Node *live_b = instance->get_node_or_null(NodePath("B"));
	CHECK(live_b != nullptr);
	CHECK(live_b->get_parent() == instance);

	destroy_instance(instance);
}

TEST_CASE("[SceneTree][SceneReconciler] Reorders siblings") {
	// v1: root -> [A, B].
	Node *root1 = make_node(nullptr, "Root", 1);
	make_node(root1, "A", 2);
	make_node(root1, "B", 3);
	Node *instance = instantiate_in_tree(pack(root1));
	memdelete(root1);

	// v2: root -> [B, A] (swapped).
	Node *root2 = make_node(nullptr, "Root", 1);
	make_node(root2, "B", 3);
	make_node(root2, "A", 2);
	Ref<PackedScene> ps2 = pack(root2);
	memdelete(root2);

	reconcile(instance, ps2->get_state(), SceneReconciler::SceneSnapshot());

	CHECK(instance->get_child(0)->get_name() == StringName("B"));
	CHECK(instance->get_child(1)->get_name() == StringName("A"));

	destroy_instance(instance);
}

TEST_CASE("[SceneTree][SceneReconciler] Adds and reverts a group membership") {
	// v1: child in no groups.
	Node *root1 = make_node(nullptr, "Root", 1);
	make_node(root1, "Child", 2);
	Ref<PackedScene> ps1 = pack(root1);
	Node *instance = instantiate_in_tree(ps1);
	memdelete(root1);

	// v2: child in the "enemies" group (persistent).
	Node *root2 = make_node(nullptr, "Root", 1);
	make_node(root2, "Child", 2)->add_to_group("enemies", true);
	Ref<PackedScene> ps2 = pack(root2);
	memdelete(root2);

	Node *live_child = instance->get_node_or_null(NodePath("Child"));
	CHECK(live_child != nullptr);
	CHECK_FALSE(live_child->is_in_group("enemies"));

	// Add the group, then revert it (v1 has no groups).
	SceneReconciler::SceneSnapshot snapshot = reconcile(instance, ps2->get_state(), SceneReconciler::SceneSnapshot());
	CHECK(live_child->is_in_group("enemies"));

	reconcile(instance, ps1->get_state(), snapshot);
	CHECK_FALSE(live_child->is_in_group("enemies"));

	destroy_instance(instance);
}

TEST_CASE("[SceneTree][SceneReconciler] Adds and reverts a signal connection") {
	// v1: no connections.
	Node *root1 = make_node(nullptr, "Root", 1);
	make_node(root1, "A", 2);
	make_node(root1, "B", 3);
	Ref<PackedScene> ps1 = pack(root1);
	Node *instance = instantiate_in_tree(ps1);
	memdelete(root1);

	// v2: A.ready -> B.set_process (persistent).
	Node *root2 = make_node(nullptr, "Root", 1);
	Node *a2 = make_node(root2, "A", 2);
	Node *b2 = make_node(root2, "B", 3);
	a2->connect("ready", Callable(b2, "set_process"), Object::CONNECT_PERSIST);
	Ref<PackedScene> ps2 = pack(root2);
	memdelete(root2);

	Node *live_a = instance->get_node_or_null(NodePath("A"));
	Node *live_b = instance->get_node_or_null(NodePath("B"));
	CHECK(live_a != nullptr);
	CHECK(live_b != nullptr);
	const Callable live_callable = Callable(live_b, "set_process");
	CHECK_FALSE(live_a->is_connected("ready", live_callable));

	// Add the connection, then revert it (v1 has none).
	SceneReconciler::SceneSnapshot snapshot = reconcile(instance, ps2->get_state(), SceneReconciler::SceneSnapshot());
	CHECK(live_a->is_connected("ready", live_callable));

	reconcile(instance, ps1->get_state(), snapshot);
	CHECK_FALSE(live_a->is_connected("ready", live_callable));

	destroy_instance(instance);
}

TEST_CASE("[SceneTree][SceneReconciler] Reorder is robust to an id-less sibling") {
	// Regression: the reorder pass once treated the dense rank over id'd nodes as an absolute live
	// child index, so an id-less sibling (e.g. runtime-added) made it move id'd nodes to wrong slots.
	// v1: root -> [A, B].
	Node *root1 = make_node(nullptr, "Root", 1);
	make_node(root1, "A", 2);
	make_node(root1, "B", 3);
	Node *instance = instantiate_in_tree(pack(root1));
	memdelete(root1);

	// Insert an id-less child at the front of the live instance (no unique id, not owned by the root,
	// so the reconciler ignores it structurally). Live order is now [X, A, B].
	Node *x = memnew(Node);
	x->set_name("X");
	instance->add_child(x);
	instance->move_child(x, 0);

	// v2: root -> [B, A] (id'd children swapped).
	Node *root2 = make_node(nullptr, "Root", 1);
	make_node(root2, "B", 3);
	make_node(root2, "A", 2);
	Ref<PackedScene> ps2 = pack(root2);
	memdelete(root2);

	reconcile(instance, ps2->get_state(), SceneReconciler::SceneSnapshot());

	// The id-less node keeps its slot; the id'd children take the swapped order within their own slots.
	CHECK(instance->get_child(0)->get_name() == StringName("X"));
	CHECK(instance->get_child(1)->get_name() == StringName("B"));
	CHECK(instance->get_child(2)->get_name() == StringName("A"));

	destroy_instance(instance);
}

// Note: finding 6 (an id'd node whose authored parent has no unique id) is not covered here. It only
// arises for nodes inside a nested foreign instance, which `pack()` cannot reproduce in this flat
// harness: it auto-assigns a fresh unique id (via ResourceUID) to every owned node, so no genuinely
// id-less parent exists, and two independent packs would assign mismatched ids anyway.

TEST_CASE("[SceneTree][SceneReconciler] Applies a new node's properties exactly once") {
	// Regression: a freshly added node had its properties applied in both the add pass and the later
	// apply pass, double-firing non-idempotent setters.
	GDREGISTER_CLASS(_ReconcilerSetterProbe);

	// v1: root -> A.
	Node *root1 = make_node(nullptr, "Root", 1);
	make_node(root1, "A", 2);
	Node *instance = instantiate_in_tree(pack(root1));
	memdelete(root1);

	// v2: root -> A, Probe (Probe is new, with a non-default `value`).
	Node *root2 = make_node(nullptr, "Root", 1);
	make_node(root2, "A", 2);
	// Authoring the override only; the live node is a fresh instance created by reconcile.
	make_node<_ReconcilerSetterProbe>(root2, "Probe", 3)->set_value(7);
	Ref<PackedScene> ps2 = pack(root2);
	memdelete(root2);

	reconcile(instance, ps2->get_state(), SceneReconciler::SceneSnapshot());

	_ReconcilerSetterProbe *live_probe = Object::cast_to<_ReconcilerSetterProbe>(instance->get_node_or_null(NodePath("Probe")));
	CHECK(live_probe != nullptr);
	CHECK(live_probe->get_value() == 7);
	CHECK(live_probe->get_set_count() == 1); // Applied once, not twice.

	destroy_instance(instance);
}

TEST_CASE("[SceneTree][SceneReconciler] Reconciling a bound connection neither duplicates nor leaks it") {
	// Regression: connection identity was matched by full Callable equality, so a bound connection
	// (whose rebuilt Callable may not compare equal to the instantiated one) could be connected twice
	// or fail to disconnect. v1 authors the connection so the live one is created by instantiation.
	Node *root1 = make_node(nullptr, "Root", 1);
	Node *a1 = make_node(root1, "A", 2);
	Node *b1 = make_node(root1, "B", 3);
	a1->connect("ready", Callable(b1, "set_process").bind(true), Object::CONNECT_PERSIST);
	Ref<PackedScene> ps1 = pack(root1);
	Node *instance = instantiate_in_tree(ps1);
	memdelete(root1);

	Node *live_a = instance->get_node_or_null(NodePath("A"));
	CHECK(live_a != nullptr);
	CHECK(count_connections(live_a, "ready", "set_process") == 1);

	// Reconciling against the same authored connection must not add a duplicate.
	SceneReconciler::SceneSnapshot snapshot = reconcile(instance, ps1->get_state(), SceneReconciler::SceneSnapshot());
	CHECK(count_connections(live_a, "ready", "set_process") == 1);

	// v2: connection removed -> the real (bound) connection must be disconnected, leaving none.
	Node *root2 = make_node(nullptr, "Root", 1);
	make_node(root2, "A", 2);
	make_node(root2, "B", 3);
	Ref<PackedScene> ps2 = pack(root2);
	memdelete(root2);

	reconcile(instance, ps2->get_state(), snapshot);
	CHECK(count_connections(live_a, "ready", "set_process") == 0);

	destroy_instance(instance);
}

} // namespace TestSceneReconciler

#endif // DEBUG_ENABLED
