// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>
#include <string>

namespace httplib {
class Server;
}

namespace dfcapture {

struct UnitOwnership {
    bool owned = false;
    bool online = false;
    std::string player_id;
    std::string player_name;
    std::string assigned_by;
    long long assigned_at_ms = 0;
    std::string notes;
};

void ownership_note_world(const std::string& save_dir);
void ownership_clear_world();
void ownership_scheduler_update();
bool ownership_lookup_unit(int32_t unit_id, UnitOwnership& ownership);
void register_player_ownership_routes(httplib::Server& server);

} // namespace dfcapture
