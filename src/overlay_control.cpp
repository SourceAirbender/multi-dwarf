// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, version 3 of the License.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// Runs on DFHack (Zlib); descends from DFPlex (Zlib) and webfort (ISC).
// Full license: see LICENSE. Third-party credits: see NOTICE.
//
// SPDX-License-Identifier: AGPL-3.0-only

#include "overlay_control.h"

#include "diagnostics.h"

#include "Core.h"
#include "PluginManager.h"

#include <mutex>

namespace dfcapture {
namespace {

std::mutex g_overlay_mutex;
bool g_overlay_disabled_by_dfcapture = false;

} // namespace

bool disable_overlay_for_stream(DFHack::color_ostream& out, std::string* note) {
    std::lock_guard<std::mutex> lock(g_overlay_mutex);

    auto& core = DFHack::Core::getInstance();
    auto* plugins = core.getPluginManager();
    DFHack::Plugin* overlay = plugins ? plugins->getPluginByName("overlay") : nullptr;
    if (!overlay || !overlay->can_set_enabled() || !overlay->is_enabled()) {
        if (note) note->clear();
        return true;
    }

    auto rc = core.runCommand(out, "disable overlay");
    if (rc != DFHack::CR_OK || overlay->is_enabled()) {
        if (note) *note = "could not disable DFHack overlay plugin";
        return false;
    }

    g_overlay_disabled_by_dfcapture = true;
    diagnostics_log("DIAG: disabled DFHack overlay while dfcapture stream is running; "
                    "offscreen viewscreen rendering would otherwise invoke overlay Lua.");
    if (note) {
        *note = "DFHack overlay was disabled while dfcapture is streaming; "
                "it will be restored when the stream stops.";
    }
    return true;
}

void restore_overlay_after_stream(DFHack::color_ostream* out) {
    std::lock_guard<std::mutex> lock(g_overlay_mutex);
    if (!g_overlay_disabled_by_dfcapture)
        return;

    auto& core = DFHack::Core::getInstance();
    auto* plugins = core.getPluginManager();
    DFHack::Plugin* overlay = plugins ? plugins->getPluginByName("overlay") : nullptr;
    if (overlay && overlay->can_set_enabled() && !overlay->is_enabled()) {
        DFHack::color_ostream& con = out ? *out : core.getConsole();
        auto rc = core.runCommand(con, "enable overlay");
        if (rc != DFHack::CR_OK || !overlay->is_enabled()) {
            diagnostics_log("WARN: dfcapture could not restore DFHack overlay after stream stop.");
            return;
        }
    }

    g_overlay_disabled_by_dfcapture = false;
    diagnostics_log("DIAG: restored DFHack overlay after dfcapture stream stop.");
}

} // namespace dfcapture
