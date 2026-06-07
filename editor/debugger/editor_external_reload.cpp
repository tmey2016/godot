/**************************************************************************/
/*  editor_external_reload.cpp                                            */
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

#include "editor_external_reload.h"

#include "core/object/class_db.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/script/script_editor_plugin.h"

void EditorExternalReload::poll(EditorDebuggerNode *p_debugger, double p_delta) {
	// `session_active` is toggled by EditorDebuggerNode on connect/disconnect, so we never scan the
	// filesystem while no game is connected (e.g. with "Keep Debug Server Open" the debugger node
	// keeps processing after the game exits).
	if (!enabled || !session_active || !p_debugger) {
		return;
	}

	// Throttle the filesystem scan; matches the editor's own background `scan_changes_timer` cadence.
	scan_timeout -= p_delta;
	if (scan_timeout > 0.0) {
		return;
	}
	scan_timeout = 0.5;

	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (!efs) {
		return;
	}

	// Refresh the filesystem's cached metadata (this also reimports changed imported assets, which
	// are then forwarded to the running game by the existing reimport pipeline).
	efs->scan_changes();

	PackedStringArray scripts;
	PackedStringArray scenes;
	PackedStringArray resources;
	_collect_changed_files(efs->get_filesystem(), scripts, scenes, resources);

	// Scripts go through the script editor's live-reload path, which honors "Synchronize Script
	// Changes" and skips scripts that fail to parse.
	if (!scripts.is_empty()) {
		if (ScriptEditor *se = ScriptEditor::get_singleton()) {
			for (const String &path : scripts) {
				se->trigger_live_script_reload(path);
			}
		}
	}

	// Non-scene resources (materials, native resources, shaders, etc.) reload from disk in the game.
	if (!resources.is_empty()) {
		p_debugger->reload_cached_files(resources);
	}

	// Scenes are reconciled into running instances by unique node id (honors "Synchronize Scene
	// Changes").
	for (const String &path : scenes) {
		p_debugger->reconcile_scene(path);
	}
}

void EditorExternalReload::_collect_changed_files(EditorFileSystemDirectory *p_dir, PackedStringArray &r_scripts, PackedStringArray &r_scenes, PackedStringArray &r_resources) {
	if (!p_dir) {
		return;
	}

	for (int i = 0; i < p_dir->get_file_count(); i++) {
		// Imported assets (those with a `.import`) are handled by the reimport pipeline, not here.
		if (p_dir->get_file_import_modified_time(i) != 0) {
			continue;
		}

		const String path = p_dir->get_file_path(i);
		// Use the modification time cached by `scan_changes()` (called just before) to avoid a
		// `stat()` per file on every poll.
		const uint64_t modified_time = p_dir->get_file_modified_time(i);
		const uint64_t *last_modified_time = file_modified_times.getptr(path);
		const bool changed = last_modified_time && *last_modified_time != modified_time;
		file_modified_times[path] = modified_time;
		if (!changed) {
			continue;
		}

		const StringName type = p_dir->get_file_type(i);
		if (ClassDB::is_parent_class(type, "Script")) {
			r_scripts.push_back(path);
		} else if (type == SNAME("PackedScene")) {
			r_scenes.push_back(path);
		} else {
			r_resources.push_back(path);
		}
	}

	for (int i = 0; i < p_dir->get_subdir_count(); i++) {
		_collect_changed_files(p_dir->get_subdir(i), r_scripts, r_scenes, r_resources);
	}
}
