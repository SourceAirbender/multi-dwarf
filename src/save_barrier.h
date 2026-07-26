// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// Copyright (C) 2026 Jake Taplin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

namespace dfcapture {

// Engages before DF serializes site data. Browser operations stay blocked until DF has completed
// several clean update frames after its save request and save viewscreen disappear.
void save_barrier_begin();
void save_barrier_update();

// Keeps browser operations blocked while there is no loaded world. This is separate from the save
// cleanup state so plugin_onupdate cannot accidentally reopen the gate after a world unload.
void save_barrier_set_world_loaded(bool loaded);

// Permanently closes the gate for the remainder of plugin shutdown.
void save_barrier_shutdown();

// Safe to call from HTTP and render worker threads.
bool save_barrier_active();

} // namespace dfcapture
