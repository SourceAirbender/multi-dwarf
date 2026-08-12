// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// Copyright (C) 2026 Jake Taplin
// SPDX-License-Identifier: AGPL-3.0-only

#include "save_barrier.h"

#include "DataDefs.h"
#include "diagnostics.h"
#include "modules/Gui.h"

#include "df/gamest.h"
#include "df/global_objects.h"
#include "df/plotinfost.h"
#include "df/viewscreen_export_regionst.h"
#include "df/viewscreen_savegamest.h"

#include <atomic>

namespace dfcapture {
namespace {

std::atomic<bool> g_active{true};
std::atomic<bool> g_world_loaded{false};
std::atomic<bool> g_save_cleanup{false};
std::atomic<bool> g_shutting_down{false};
int g_clear_frames = 0; // core thread only

bool df_still_saving() {
    if (df::global::plotinfo && df::global::plotinfo->main.autosave_request)
        return true;
    // do_manual_save is a request/latch, not a completion signal. DF can leave it set
    // after the save viewscreen has closed and the fortress is interactive again. Using it here
    // can strand every browser route
    // behind the barrier. plugin_save_site_data() already engages us before serialization; the
    // active save/export viewscreen and autosave request are the authoritative "still saving"
    // signals after that point.
    auto screen = DFHack::Gui::getCurViewscreen(true);
    return strict_virtual_cast<df::viewscreen_savegamest>(screen) ||
           strict_virtual_cast<df::viewscreen_export_regionst>(screen);
}

} // namespace

void save_barrier_begin() {
    g_clear_frames = 0;
    g_save_cleanup.store(true);
    if (!g_active.exchange(true))
        diagnostics_log("SAVE-BARRIER engaged; browser world operations are blocked");
}

void save_barrier_update() {
    if (g_shutting_down.load() || !g_world_loaded.load() || !g_save_cleanup.load())
        return;
    if (df_still_saving()) {
        g_clear_frames = 0;
        return;
    }

    // DF can finish writing before transient save bookkeeping is retired. Require three complete
    // core updates after every save signal disappears before admitting browser work again.
    if (++g_clear_frames < 3)
        return;
    g_clear_frames = 0;
    g_save_cleanup.store(false);
    g_active.store(false);
    diagnostics_log("SAVE-BARRIER cleared after completed save cleanup");
}

void save_barrier_set_world_loaded(bool loaded) {
    g_world_loaded.store(loaded);
    g_clear_frames = 0;
    g_save_cleanup.store(false);
    if (!loaded) {
        if (!g_active.exchange(true))
            diagnostics_log("WORLD-BARRIER engaged; browser world operations are blocked");
        return;
    }
    if (!g_shutting_down.load()) {
        g_active.store(false);
        diagnostics_log("WORLD-BARRIER cleared; world is loaded");
    }
}

void save_barrier_shutdown() {
    g_shutting_down.store(true);
    g_active.store(true);
    g_save_cleanup.store(false);
    diagnostics_log("SHUTDOWN-BARRIER engaged; new browser world operations are blocked");
}

bool save_barrier_active() {
    return g_active.load();
}

} // namespace dfcapture
