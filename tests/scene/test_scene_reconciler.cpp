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

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"
#include "core/variant/callable.h"
#include "scene/2d/node_2d.h"
#include "scene/debugger/scene_reconciler.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/packed_scene.h"
#include "tests/test_utils.h"

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

// Inherited-scene reconciliation. Authoring an inherited scene requires instantiating a base scene
// with an editor edit state, which is only available in tools builds, so these are gated on
// TOOLS_ENABLED (the live reconciler itself runs in any debug build).
#ifdef TOOLS_ENABLED

// Packs `p_root`, saves it to a temp .tscn, and loads it back with a real resource path. An
// inheritance base must be loadable by path, because packing an inherited scene reloads its base
// from disk (see SceneState::pack).
static Ref<PackedScene> save_as_base(Node *p_root, const String &p_file_suffix) {
	const String path = TestUtils::get_temp_path(p_file_suffix);
	Error err = ResourceSaver::save(pack(p_root), path);
	REQUIRE(err == OK);
	Ref<PackedScene> base = ResourceLoader::load(path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(base.is_valid());
	return base;
}

// Instantiates `p_base` as the root of a NEW inherited scene (base nodes present, inheritance state
// set), so the caller can author derived nodes on top before packing.
static Node *new_inherited_root(const Ref<PackedScene> &p_base) {
	Node *root = p_base->instantiate(PackedScene::GEN_EDIT_STATE_MAIN_INHERITED);
	REQUIRE(root != nullptr);
	return root;
}

TEST_CASE("[SceneTree][SceneReconciler] Inherited scene removes a derived-added node but keeps base nodes") {
	// Base: BaseRoot(1) -> BaseChild(2).
	Node *base_root = make_node(nullptr, "BaseRoot", 1);
	make_node<Node2D>(base_root, "BaseChild", 2);
	Ref<PackedScene> base = save_as_base(base_root, "reconciler_inherited_remove_base.tscn");
	memdelete(base_root);

	// Inherited v1: adds DerivedChild(3) under the inherited root.
	Node *inh1 = new_inherited_root(base);
	make_node<Node2D>(inh1, "DerivedChild", 3);
	Ref<PackedScene> ps1 = pack(inh1);
	memdelete(inh1);

	Node *instance = instantiate_in_tree(ps1);
	Node *live_base_child = instance->get_node_or_null(NodePath("BaseChild"));
	Node *live_derived_child = instance->get_node_or_null(NodePath("DerivedChild"));
	REQUIRE(live_base_child != nullptr);
	REQUIRE(live_derived_child != nullptr);

	// Seed the snapshot from v1: records derived_added_ids = {3} (the base child is not derived-added).
	SceneReconciler::SceneSnapshot snapshot = reconcile(instance, ps1->get_state(), SceneReconciler::SceneSnapshot());

	// Inherited v2: DerivedChild dropped; base subtree untouched.
	Node *inh2 = new_inherited_root(base);
	Ref<PackedScene> ps2 = pack(inh2);
	memdelete(inh2);

	reconcile(instance, ps2->get_state(), snapshot);

	// The derived-added node is removed (deferred); the inherited base node is preserved. This is the
	// core safety property: an absent base node must NOT be treated as a deletion.
	CHECK(live_derived_child->is_queued_for_deletion());
	CHECK_FALSE(live_base_child->is_queued_for_deletion());

	destroy_instance(instance);
}

TEST_CASE("[SceneTree][SceneReconciler] Inherited scene never removes an untouched base node") {
	// Base: BaseRoot(1) -> BaseChild(2). The inherited scene adds nothing.
	Node *base_root = make_node(nullptr, "BaseRoot", 1);
	make_node<Node2D>(base_root, "BaseChild", 2);
	Ref<PackedScene> base = save_as_base(base_root, "reconciler_inherited_keep_base.tscn");
	memdelete(base_root);

	Node *inh1 = new_inherited_root(base);
	Ref<PackedScene> ps1 = pack(inh1);
	memdelete(inh1);

	Node *instance = instantiate_in_tree(ps1);
	Node *live_base_child = instance->get_node_or_null(NodePath("BaseChild"));
	REQUIRE(live_base_child != nullptr);

	// The base child is enumerated by the base, not the derived state, so it never appears in
	// `desired`. Reconciling must still leave it untouched.
	SceneReconciler::SceneSnapshot snapshot = reconcile(instance, ps1->get_state(), SceneReconciler::SceneSnapshot());
	reconcile(instance, ps1->get_state(), snapshot);

	CHECK_FALSE(live_base_child->is_queued_for_deletion());
	CHECK(instance->get_node_or_null(NodePath("BaseChild")) != nullptr);

	destroy_instance(instance);
}

TEST_CASE("[SceneTree][SceneReconciler] Inherited scene renames a derived-added node") {
	// Base: BaseRoot(1) -> BaseChild(2).
	Node *base_root = make_node(nullptr, "BaseRoot", 1);
	make_node<Node2D>(base_root, "BaseChild", 2);
	Ref<PackedScene> base = save_as_base(base_root, "reconciler_inherited_rename_base.tscn");
	memdelete(base_root);

	// v1: DerivedChild(3).
	Node *inh1 = new_inherited_root(base);
	make_node<Node2D>(inh1, "DerivedChild", 3);
	Ref<PackedScene> ps1 = pack(inh1);
	memdelete(inh1);

	Node *instance = instantiate_in_tree(ps1);
	SceneReconciler::SceneSnapshot snapshot = reconcile(instance, ps1->get_state(), SceneReconciler::SceneSnapshot());

	// v2: same id (3), renamed to RenamedChild.
	Node *inh2 = new_inherited_root(base);
	make_node<Node2D>(inh2, "RenamedChild", 3);
	Ref<PackedScene> ps2 = pack(inh2);
	memdelete(inh2);

	reconcile(instance, ps2->get_state(), snapshot);

	CHECK(instance->get_node_or_null(NodePath("RenamedChild")) != nullptr);
	CHECK(instance->get_node_or_null(NodePath("DerivedChild")) == nullptr);

	destroy_instance(instance);
}

TEST_CASE("[SceneTree][SceneReconciler] Inherited scene reparents a derived-added node under a base node") {
	// Base: BaseRoot(1) -> BaseChild(2).
	Node *base_root = make_node(nullptr, "BaseRoot", 1);
	make_node<Node2D>(base_root, "BaseChild", 2);
	Ref<PackedScene> base = save_as_base(base_root, "reconciler_inherited_reparent_base.tscn");
	memdelete(base_root);

	// v1: DerivedChild(3) directly under the inherited root.
	Node *inh1 = new_inherited_root(base);
	make_node<Node2D>(inh1, "DerivedChild", 3);
	Ref<PackedScene> ps1 = pack(inh1);
	memdelete(inh1);

	Node *instance = instantiate_in_tree(ps1);
	SceneReconciler::SceneSnapshot snapshot = reconcile(instance, ps1->get_state(), SceneReconciler::SceneSnapshot());

	// v2: DerivedChild(3) moved under the inherited BaseChild.
	Node *inh2 = new_inherited_root(base);
	Node *base_child = inh2->get_node_or_null(NodePath("BaseChild"));
	REQUIRE(base_child != nullptr);
	make_node<Node2D>(base_child, "DerivedChild", 3);
	Ref<PackedScene> ps2 = pack(inh2);
	memdelete(inh2);

	reconcile(instance, ps2->get_state(), snapshot);

	CHECK(instance->get_node_or_null(NodePath("BaseChild/DerivedChild")) != nullptr);
	CHECK(instance->get_node_or_null(NodePath("DerivedChild")) == nullptr);

	destroy_instance(instance);
}

TEST_CASE("[SceneTree][SceneReconciler] Multi-level inheritance removes only the leaf scene's own node") {
	// A(1) -> AChild(2).
	Node *a_root = make_node(nullptr, "A", 1);
	make_node<Node2D>(a_root, "AChild", 2);
	Ref<PackedScene> a = save_as_base(a_root, "reconciler_inherited_multi_a.tscn");
	memdelete(a_root);

	// B inherits A and adds BChild(3); save B as a base too.
	Node *b_root = new_inherited_root(a);
	make_node<Node2D>(b_root, "BChild", 3);
	Ref<PackedScene> b = save_as_base(b_root, "reconciler_inherited_multi_b.tscn");
	memdelete(b_root);

	// C inherits B and adds CChild(4).
	Node *c1 = new_inherited_root(b);
	make_node<Node2D>(c1, "CChild", 4);
	Ref<PackedScene> ps_c1 = pack(c1);
	memdelete(c1);

	Node *instance = instantiate_in_tree(ps_c1);
	Node *live_a_child = instance->get_node_or_null(NodePath("AChild"));
	Node *live_b_child = instance->get_node_or_null(NodePath("BChild"));
	Node *live_c_child = instance->get_node_or_null(NodePath("CChild"));
	REQUIRE(live_a_child != nullptr);
	REQUIRE(live_b_child != nullptr);
	REQUIRE(live_c_child != nullptr);

	SceneReconciler::SceneSnapshot snapshot = reconcile(instance, ps_c1->get_state(), SceneReconciler::SceneSnapshot());

	// C v2 drops CChild. AChild and BChild are inherited (empty type in C's state) and must survive;
	// only C's own node is eligible for removal.
	Node *c2 = new_inherited_root(b);
	Ref<PackedScene> ps_c2 = pack(c2);
	memdelete(c2);

	reconcile(instance, ps_c2->get_state(), snapshot);

	CHECK(live_c_child->is_queued_for_deletion());
	CHECK_FALSE(live_a_child->is_queued_for_deletion());
	CHECK_FALSE(live_b_child->is_queued_for_deletion());

	destroy_instance(instance);
}

#endif // TOOLS_ENABLED

} // namespace TestSceneReconciler

#endif // DEBUG_ENABLED
