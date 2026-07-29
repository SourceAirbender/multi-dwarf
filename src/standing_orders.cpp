// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// Copyright (C) 2026 Jake Taplin
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

#include "standing_orders.h"

#include "Core.h"
#include "http_server.h"
#include "json_util.h"
#include "save_barrier.h"
#include "sdl_capture.h"

#include "modules/Translation.h"
#include "modules/Units.h"

#include "df/global_objects.h"
#include "df/labor_infost.h"
#include "df/plotinfost.h"
#include "df/unit.h"
#include "df/unit_labor.h"
#include "df/world.h"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace DFHack;

namespace dfcapture {
namespace {

std::recursive_mutex g_standing_orders_mutex;

template <typename Fn>
bool run_standing_orders_locked(Fn&& fn) {
    std::lock_guard<std::recursive_mutex> lock(g_standing_orders_mutex);
    std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
    DFHack::CoreSuspender suspend;
    if (save_barrier_active())
        return false;
    return fn();
}

// Every df::global::standing_orders_* field is a `uint8_t*` (DFHack's usual "address of a
// singleton bool-byte pattern. The
// table stores the ADDRESS OF that pointer variable (uint8_t**) so read/write is generic:
// *addr is the uint8_t* (null if this DF build doesn't expose it), **addr is the live value.
struct StandingOrderDef {
    const char* key;
    uint8_t** addr;
    const char* label;
    const char* group_id;
    // Petition orders use a three-state cycle
    // (prompt=0 / accept=1 / reject=2), not a bool. tristate items serve a raw value and accept
    // 0/1/2 on POST; every other order stays boolean.
    bool tristate = false;
};

struct StandingOrderGroup {
    const char* id;
    const char* label;
};

// DF's real 7 Standing Orders tabs, in on-screen order (df::standing_orders_category_type,
// 16b-labor-standing-orders.png: Workshops/Hauling/Refuse/Forbidding/Petitions/Chores/Other).
const std::vector<StandingOrderGroup>& groups() {
    static const std::vector<StandingOrderGroup> value = {
        {"workshops", "Workshops"},
        {"hauling", "Hauling"},
        {"refuse", "Refuse"},
        {"forbidding", "Forbidding"},
        {"petitions", "Petitions"},
        {"chores", "Chores"},
        {"other", "Other"},
    };
    return value;
}

// Field -> (label, group) table. Labels follow DF's Workshops tab and df-structures semantics.
const std::vector<StandingOrderDef>& order_defs() {
    static const std::vector<StandingOrderDef> value = {
        // Workshops (AUTOMATED_WORKSHOPS)
        {"auto_loom", &df::global::standing_orders_auto_loom, "Automatically weave all thread", "workshops"},
        {"use_dyed_cloth", &df::global::standing_orders_use_dyed_cloth, "Use any cloth", "workshops"},
        {"auto_collect_webs", &df::global::standing_orders_auto_collect_webs, "Automatically collect webs", "workshops"},
        {"auto_slaughter", &df::global::standing_orders_auto_slaughter, "Slaughter any marked animal", "workshops"},
        {"auto_butcher", &df::global::standing_orders_auto_butcher, "Automatically butcher carcasses", "workshops"},
        {"auto_fishery", &df::global::standing_orders_auto_fishery, "Automatically clean fish", "workshops"},
        {"auto_kitchen", &df::global::standing_orders_auto_kitchen, "Automate kitchen", "workshops"},
        {"auto_tan", &df::global::standing_orders_auto_tan, "Automate tannery", "workshops"},
        {"auto_smelter", &df::global::standing_orders_auto_smelter, "Automate smelter", "workshops"},
        {"auto_kiln", &df::global::standing_orders_auto_kiln, "Automate kiln", "workshops"},
        {"auto_other", &df::global::standing_orders_auto_other, "Automate other workshops", "workshops"},
        // Hauling
        {"gather_wood", &df::global::standing_orders_gather_wood, "Gather wood", "hauling"},
        {"gather_food", &df::global::standing_orders_gather_food, "Gather food", "hauling"},
        {"gather_furniture", &df::global::standing_orders_gather_furniture, "Gather furniture", "hauling"},
        {"gather_minerals", &df::global::standing_orders_gather_minerals, "Gather minerals", "hauling"},
        {"gather_animals", &df::global::standing_orders_gather_animals, "Gather stray animals", "hauling"},
        {"gather_refuse", &df::global::standing_orders_gather_refuse, "Gather refuse", "hauling"},
        {"gather_refuse_outside", &df::global::standing_orders_gather_refuse_outside, "Gather refuse from the outside", "hauling"},
        {"gather_vermin_remains", &df::global::standing_orders_gather_vermin_remains, "Gather vermin remains", "hauling"},
        {"zoneonly_drink", &df::global::standing_orders_zoneonly_drink, "Haul drinks to food stockpiles only", "hauling"},
        {"zoneonly_fish", &df::global::standing_orders_zoneonly_fish, "Haul fish to food stockpiles only", "hauling"},
        // Refuse
        {"gather_bodies", &df::global::standing_orders_gather_bodies, "Gather bodies for burial", "refuse"},
        {"dump_bones", &df::global::standing_orders_dump_bones, "Dump bones", "refuse"},
        {"dump_corpses", &df::global::standing_orders_dump_corpses, "Dump corpses", "refuse"},
        {"dump_hair", &df::global::standing_orders_dump_hair, "Dump hair", "refuse"},
        {"dump_shells", &df::global::standing_orders_dump_shells, "Dump shells", "refuse"},
        {"dump_skins", &df::global::standing_orders_dump_skins, "Dump skins", "refuse"},
        {"dump_skulls", &df::global::standing_orders_dump_skulls, "Dump skulls", "refuse"},
        {"dump_other", &df::global::standing_orders_dump_other, "Dump other refuse", "refuse"},
        // Forbidding
        {"forbid_own_dead", &df::global::standing_orders_forbid_own_dead, "Forbid own dead", "forbidding"},
        {"forbid_own_dead_items", &df::global::standing_orders_forbid_own_dead_items, "Forbid own dead's belongings", "forbidding"},
        {"forbid_other_dead_items", &df::global::standing_orders_forbid_other_dead_items, "Forbid other's dead items", "forbidding"},
        {"forbid_other_nohunt", &df::global::standing_orders_forbid_other_nohunt, "Forbid hunting others' wildlife", "forbidding"},
        {"forbid_used_ammo", &df::global::standing_orders_forbid_used_ammo, "Forbid used ammo", "forbidding"},
        {"forbid_rearming_traps", &df::global::standing_orders_forbid_rearming_traps, "Forbid rearming of traps", "forbidding"},
        {"forbid_trap_cleaning", &df::global::standing_orders_forbid_trap_cleaning, "Forbid trap cleaning", "forbidding"},
        {"forbid_cages_from_sprung_traps", &df::global::standing_orders_forbid_cages_from_sprung_traps, "Forbid cages from sprung traps", "forbidding"},
        {"forbid_toppled_building_items", &df::global::standing_orders_forbid_toppled_building_items, "Forbid items from toppled buildings", "forbidding"},
        {"forbid_floor_and_wall_cleaning", &df::global::standing_orders_forbid_floor_and_wall_cleaning, "Forbid floor and wall cleaning", "forbidding"},
        // Petitions use prompt/accept/reject; the state suffix is rendered client-side.
        {"petition_citizenship", &df::global::standing_orders_petition_citizenship, "Citizenship petitions", "petitions", true},
        {"petition_resident_performer", &df::global::standing_orders_petition_resident_performer, "Performer petitions", "petitions", true},
        {"petition_resident_monster_hunter", &df::global::standing_orders_petition_resident_monster_hunter, "Monster slayer petitions", "petitions", true},
        {"petition_resident_mercenary", &df::global::standing_orders_petition_resident_mercenary, "Mercenary petitions", "petitions", true},
        {"petition_resident_scholar", &df::global::standing_orders_petition_resident_scholar, "Scholar petitions", "petitions", true},
        {"petition_resident_sanctuary", &df::global::standing_orders_petition_resident_sanctuary, "Sanctuary petitions", "petitions", true},
        // Chores
        {"farmer_harvest", &df::global::standing_orders_farmer_harvest, "Farmers harvest and plant automatically", "chores"},
        {"ignore_damp_stone", &df::global::standing_orders_ignore_damp_stone, "Ignore damp stone when dumping", "chores"},
        {"ignore_warm_stone", &df::global::standing_orders_ignore_warm_stone, "Ignore warm stone when dumping", "chores"},
        // Other
        {"job_cancel_announce", &df::global::standing_orders_job_cancel_announce, "Announce job cancellations", "other"},
        {"mix_food", &df::global::standing_orders_mix_food, "Dwarves mix food types when eating", "other"},
    };
    return value;
}

const StandingOrderDef* find_def(const std::string& key) {
    for (const auto& def : order_defs())
        if (key == def.key)
            return &def;
    return nullptr;
}

std::string build_standing_orders_json() {
    std::ostringstream body;
    bool ok = run_standing_orders_locked([&]() -> bool {
        body << "{\"ok\":true,\"groups\":[";
        bool first_group = true;
        for (const auto& group : groups()) {
            if (!first_group) body << ",";
            first_group = false;
            body << "{\"id\":" << json_string(group.id) << ",\"label\":"
                 << json_string(group.label) << ",\"items\":[";
            bool first_item = true;
            for (const auto& def : order_defs()) {
                if (std::string(def.group_id) != group.id)
                    continue;
                if (!def.addr || !*def.addr)
                    continue; // field not present in this DF build -- omit rather than fabricate
                if (!first_item) body << ",";
                first_item = false;
                int raw = static_cast<int>(**def.addr);
                bool value = raw != 0;
                // bool `value` serves binary clients; `raw` and `tristate` expose all three states.
                body << "{\"key\":" << json_string(def.key) << ",\"label\":"
                     << json_string(def.label)
                     << ",\"value\":" << (value ? "true" : "false")
                     << ",\"raw\":" << raw
                     << ",\"tristate\":" << (def.tristate ? "true" : "false") << "}";
            }
            body << "]}";
        }
        body << "]}\n";
        return true;
    });
    return ok ? body.str() : "{\"ok\":false,\"error\":\"world unavailable\"}\n";
}

bool set_standing_order(const std::string& key, int raw, std::string* err) {
    return run_standing_orders_locked([&]() -> bool {
        const StandingOrderDef* def = find_def(key);
        if (!def) { if (err) *err = "unknown standing-order key"; return false; }
        if (!def->addr || !*def->addr) { if (err) *err = "field unavailable in this DF build"; return false; }
        if (def->tristate) {
            // Petitions accept 0/1/2 (prompt/accept/reject); anything else is a 400.
            if (raw < 0 || raw > 2) { if (err) *err = "petition value must be 0, 1 or 2"; return false; }
            **def->addr = static_cast<uint8_t>(raw);
        } else {
            **def->addr = raw != 0 ? 1 : 0;
        }
        return true;
    });
}

// ---- Children roster and chore-type flags ---------------------------------------------------
// Backed by plotinfo.labor_info:
//   flags.children_do_chores  -- the global "Children do/don't do chores" toggle
//   chores[unit_labor]        -- per chore-type enable (the 14 native chore labors below)
//   chores_exempted_children  -- unit ids of children opted OUT (so enabled == NOT exempt)
// The 14 chore types use DF's native captions and order (the captions match
// live unit-labor captions).
struct ChoreType {
    const char* key;
    const char* label;
    df::unit_labor labor;
};

const std::vector<ChoreType>& chore_types() {
    using L = df::unit_labor;
    static const std::vector<ChoreType> value = {
        {"feed_patients_prisoners", "Feed Patients/Prisoners", L::FEED_WATER_CIVILIANS},
        {"milking", "Milking", L::MILK},
        {"stone_hauling", "Stone Hauling", L::HAUL_STONE},
        {"wood_hauling", "Wood Hauling", L::HAUL_WOOD},
        {"item_hauling", "Item Hauling", L::HAUL_ITEM},
        {"burial", "Burial", L::HAUL_BODY},
        {"food_hauling", "Food Hauling", L::HAUL_FOOD},
        {"refuse_hauling", "Refuse Hauling", L::HAUL_REFUSE},
        {"furniture_hauling", "Furniture Hauling", L::HAUL_FURNITURE},
        {"animal_hauling", "Animal Hauling", L::HAUL_ANIMALS},
        {"trade_good_hauling", "Trade Good Hauling", L::HAUL_TRADE},
        {"water_hauling", "Water Hauling", L::HAUL_WATER},
        {"cleaning", "Cleaning", L::CLEAN},
        {"lever_operation", "Lever Operation", L::PULL_LEVER},
    };
    return value;
}

const ChoreType* find_chore(const std::string& key) {
    for (const auto& c : chore_types())
        if (key == c.key)
            return &c;
    return nullptr;
}

bool labor_index_valid(df::unit_labor labor) {
    int idx = static_cast<int>(labor);
    return idx >= 0 && idx <= df::enum_traits<df::unit_labor>::last_item_value;
}

bool child_exempt(df::plotinfost* pi, int32_t uid) {
    for (int32_t id : pi->labor_info.chores_exempted_children)
        if (id == uid)
            return true;
    return false;
}

std::string build_chores_json() {
    std::ostringstream body;
    bool ok = run_standing_orders_locked([&]() -> bool {
        auto pi = df::global::plotinfo;
        auto world = df::global::world;
        if (!pi || !world)
            return false;
        body << "{\"ok\":true,\"childrenDoChores\":"
             << (pi->labor_info.flags.bits.children_do_chores ? "true" : "false")
             << ",\"choreTypes\":[";
        bool first = true;
        for (const auto& c : chore_types()) {
            bool enabled = labor_index_valid(c.labor) &&
                           pi->labor_info.chores[static_cast<int>(c.labor)];
            if (!first) body << ",";
            first = false;
            body << "{\"key\":" << json_string(c.key) << ",\"label\":" << json_string(c.label)
                 << ",\"enabled\":" << (enabled ? "true" : "false") << "}";
        }
        body << "],\"children\":[";
        first = true;
        for (auto unit : world->units.active) {
            // Seeded-bad guard: ONLY fortress children -- never an adult resident, and an empty
            // left pane on a fort with no children (isChild gates it, isCitizen scopes to the fort).
            if (!unit || !DFHack::Units::isCitizen(unit, true) || !DFHack::Units::isChild(unit))
                continue;
            std::string name = DFHack::Translation::translateName(&unit->name, true);
            if (name.empty())
                name = "Child " + std::to_string(unit->id);
            if (!first) body << ",";
            first = false;
            body << "{\"unitId\":" << unit->id << ",\"name\":" << json_string(name)
                 << ",\"portraitTexpos\":" << unit->portrait_texpos
                 << ",\"enabled\":" << (child_exempt(pi, unit->id) ? "false" : "true") << "}";
        }
        body << "]}\n";
        return true;
    });
    if (!ok)
        return "{\"ok\":false,\"error\":\"world unavailable\"}\n";
    return body.str();
}

bool set_chore_global(bool on, std::string* err) {
    return run_standing_orders_locked([&]() -> bool {
        auto pi = df::global::plotinfo;
        if (!pi) { if (err) *err = "world unavailable"; return false; }
        pi->labor_info.flags.bits.children_do_chores = on;
        return true;
    });
}

bool set_chore_type(const std::string& key, bool on, std::string* err) {
    return run_standing_orders_locked([&]() -> bool {
        auto pi = df::global::plotinfo;
        if (!pi) { if (err) *err = "world unavailable"; return false; }
        const ChoreType* c = find_chore(key);
        if (!c) { if (err) *err = "unknown chore type"; return false; }
        if (!labor_index_valid(c->labor)) { if (err) *err = "bad chore labor"; return false; }
        pi->labor_info.chores[static_cast<int>(c->labor)] = on;
        return true;
    });
}

bool set_child_chore(int32_t uid, bool on, std::string* err) {
    return run_standing_orders_locked([&]() -> bool {
        auto pi = df::global::plotinfo;
        if (!pi) { if (err) *err = "world unavailable"; return false; }
        auto unit = df::unit::find(uid);
        if (!unit || !DFHack::Units::isChild(unit)) { if (err) *err = "not a child"; return false; }
        auto& vec = pi->labor_info.chores_exempted_children;
        auto it = std::find(vec.begin(), vec.end(), uid);
        if (on) {              // does chores => not exempt
            if (it != vec.end()) vec.erase(it);
        } else {               // opted out => exempt
            if (it == vec.end()) vec.push_back(uid);
        }
        return true;
    });
}

} // namespace

void register_standing_orders_routes(httplib::Server& server) {
    // GET /standing-orders -> every category + its toggles' current values.
    server.Get("/standing-orders", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(build_standing_orders_json(), "application/json; charset=utf-8");
    });

    // POST /standing-orders?key=&value=1|0 -> flip one toggle.
    auto toggle_handler = [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("key")) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing key\"}\n", "application/json; charset=utf-8");
            return;
        }
        std::string key = req.get_param_value("key");
        int value = 1;
        query_int(req, "value", value);
        std::string err;
        if (!set_standing_order(key, value, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n", "application/json; charset=utf-8");
            return;
        }
        notify_player_input();
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Post("/standing-orders", toggle_handler);

    // GET /chores -> children-do-chores toggle + 14 chore-type flags + fort children roster.
    server.Get("/chores", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(build_chores_json(), "application/json; charset=utf-8");
    });

    // POST /chores -- one of:
    //   ?global=0|1                  toggle "Children do/don't do chores"
    //   ?chore=<key>&value=0|1       toggle one chore type
    //   ?child=<unitId>&value=0|1    toggle one child (value=1 => does chores; 0 => exempt)
    server.Post("/chores", [](const httplib::Request& req, httplib::Response& res) {
        std::string err;
        bool ok = false;
        if (req.has_param("global")) {
            int v = 1; query_int(req, "global", v);
            ok = set_chore_global(v != 0, &err);
        } else if (req.has_param("chore")) {
            int v = 1; query_int(req, "value", v);
            ok = set_chore_type(req.get_param_value("chore"), v != 0, &err);
        } else if (req.has_param("child")) {
            int cid = -1; query_int(req, "child", cid);
            int v = 1; query_int(req, "value", v);
            ok = set_child_chore(cid, v != 0, &err);
        } else {
            err = "missing global/chore/child param";
        }
        if (!ok) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n", "application/json; charset=utf-8");
            return;
        }
        notify_player_input();
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    });
}

} // namespace dfcapture
