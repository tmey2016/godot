/**************************************************************************/
/*  editor_external_reload.h                                              */
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

#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/variant/variant.h"

class EditorDebuggerNode;
class EditorFileSystemDirectory;

// Detects files edited outside the editor (by modification time, across the whole project) and
// forwards them to a running debug game so they hot-reload. Disabled by default; gated by the
// "Synchronize External File Changes" debug option. Per-type changes still defer to the existing
// "Synchronize Script/Scene Changes" options.
class EditorExternalReload {
	bool enabled = false;
	bool session_active = false;
	double scan_timeout = 0.0;
	HashMap<String, uint64_t> file_modified_times;

	void _collect_changed_files(EditorFileSystemDirectory *p_dir, PackedStringArray &r_scripts, PackedStringArray &r_scenes, PackedStringArray &r_resources);

public:
	void set_enabled(bool p_enabled) { enabled = p_enabled; }
	bool is_enabled() const { return enabled; }

	// Driven by EditorDebuggerNode on session connect/disconnect. While no game is connected, polling
	// is fully inert, so we never scan the filesystem when idle (e.g. with "Keep Debug Server Open",
	// where the debugger node keeps processing after the game exits).
	void set_session_active(bool p_active) { session_active = p_active; }

	// Called every frame while debugging. Throttles internally and, when enabled with a game
	// connected, scans the project for externally modified files and forwards them to the running game
	// through `p_debugger`. No-op otherwise.
	void poll(EditorDebuggerNode *p_debugger, double p_delta);
};
