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
