/**************************************************************************/
/*  scene_reconciler.h                                                     */
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

#pragma once

#ifdef DEBUG_ENABLED

#include "core/object/ref_counted.h"
#include "core/string/node_path.h"
#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

class Node;
class SceneState;

// Reconciles running scene instances against the scene as it exists on disk, matching nodes by their
// stable unique scene id (with a path-based fallback for nodes that have none). Used by the live
// debugger to apply external scene-file edits to a running game while preserving runtime state.
// Owns the per-scene snapshots that make reverting removed overrides/groups/connections possible.
class SceneReconciler {
public:
	// A persistent signal connection authored in a scene.
	struct ConnectionInfo {
		NodePath source;
		StringName signal;
		NodePath target;
		StringName method;
		Array binds;
		int unbinds = 0;
		int flags = 0;
		String key() const; // Stable identity: source|signal|target|method.
	};

	// The overrides applied on a reconciliation pass, used to revert them when later removed.
	struct SceneSnapshot {
		HashMap<int32_t, HashMap<StringName, Variant>> node_props; // id -> explicitly set properties.
		HashMap<int32_t, HashSet<StringName>> node_groups; // id -> persistent groups.
		HashMap<String, ConnectionInfo> connections; // connection key -> connection.
	};

	// Captures the scene's currently cached state as the baseline for revert, unless one already
	// exists. Call when a scene instance is first registered.
	void seed_snapshot(const String &p_scene_path);

	// Drops all baselines. Call when the feature is disabled so a later re-enable re-seeds from the
	// current scene state instead of diffing against a stale, pre-disable baseline.
	void clear_snapshots() { snapshots.clear(); }

	// Reloads `p_scene_path` from disk and reconciles every instance in `p_instances` against it.
	void reconcile(const String &p_scene_path, const LocalVector<Node *> &p_instances);

	// I/O-free core (unit-testable): reconciles `p_instances` against `p_state`, reverting overrides
	// removed since `p_previous`. Returns the snapshot to feed back next time.
	static SceneSnapshot reconcile_against_state(const LocalVector<Node *> &p_instances, const Ref<SceneState> &p_state, const SceneSnapshot &p_previous);

private:
	HashMap<String, SceneSnapshot> snapshots;

	static SceneSnapshot _build_snapshot(const Ref<SceneState> &p_state);
};

#endif // DEBUG_ENABLED
