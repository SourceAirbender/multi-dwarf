// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
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

#pragma once

#include "httplib.h"

#include "camera.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dfcapture {

struct BuildingPanelInfo {
    int32_t id = -1;
    std::string name;
    bool exists = false;
    bool built = false;
    bool has_jobs = false;
    bool suspended = false;
    bool marked = false;
    bool passage_control = false;
    bool passage_forbidden = false;
    bool passage_closed = false;
    bool is_farm_plot = false;
    bool is_cage = false;
    bool is_trade_depot = false;
    int cage_assigned_units = 0;
    int cage_assigned_items = 0;
};

struct ZonePanelInfo {
    int32_t id = -1;
    bool exists = false;
    std::string name;
    std::string type;
    bool active = false;
    int assigned_units = 0;
    bool is_pit_pond = false;
    bool is_pen = false;
    bool can_squads = false;
    int assigned_squads = 0;
    bool filling_pond = false;
    bool can_owner = false;
    int32_t owner_id = -1;
    std::string owner_name;
    bool can_location = false;
    int32_t location_id = -1;
    std::string location_name;
    std::string location_type;
    bool is_gather = false;
    bool gather_trees = false;
    bool gather_shrubs = false;
    bool gather_fallen = false;
    bool is_tomb = false;
    bool tomb_pets = false;
    bool tomb_citizens = false;
    bool is_archery = false;
    std::string archery_dir;
};

struct ZoneRepaintPlan {
    bool found = false;
    bool changed = false;
    bool removed = false;
    int32_t z = 0;
    int new_x1 = 0;
    int new_y1 = 0;
    int new_x2 = 0;
    int new_y2 = 0;
    std::vector<uint8_t> extents;
};

bool building_info_on_core_thread(int32_t id, BuildingPanelInfo& out);
bool building_action_on_core_thread(int32_t id, const std::string& action, std::string* err);
bool building_rename_on_core_thread(int32_t id, const std::string& name, std::string* err);
std::string building_info_json(const BuildingPanelInfo& b);
std::string building_cage_json_on_core_thread(int32_t building_id, std::string* err = nullptr);
bool building_cage_action_on_core_thread(int32_t building_id, int32_t target_id, bool assign,
                                         const std::string& kind,
                                         std::string* err = nullptr);

std::string farm_plot_json_on_core_thread(int32_t building_id, std::string* err = nullptr);
bool farm_plot_set_season_crop_on_core_thread(int32_t building_id, int season, int plant_id,
                                              std::string* err);
bool farm_plot_set_seasonal_fertilize_on_core_thread(int32_t building_id, bool enabled,
                                                     std::string* err);

bool zone_info_on_core_thread(int32_t id, ZonePanelInfo& out);
bool zone_action_on_core_thread(int32_t id, const std::string& action, std::string* err);
std::string zone_info_json(const ZonePanelInfo& z);
std::string zone_squads_json_on_core_thread(int32_t zone_id, std::string* err = nullptr);
bool zone_squad_action_on_core_thread(int32_t zone_id, int32_t squad_id,
                                      const std::string& mode, bool enabled,
                                      std::string* err = nullptr);
bool plan_zone_repaint_on_core_thread(int32_t id, int x1, int y1, int x2, int y2,
                                      const std::string& mode, ZoneRepaintPlan& out,
                                      std::string* err = nullptr);
bool apply_zone_repaint_in_place_on_core_thread(int32_t id, const ZoneRepaintPlan& plan,
                                                std::string* err = nullptr);

std::string zone_units_json_on_core_thread(int32_t zone_id, std::string* err = nullptr);
bool zone_unit_action_on_core_thread(int32_t zone_id, int32_t unit_id, bool assign,
                                     const std::string& kind, std::string* err);

std::string zone_owners_json_on_core_thread(int32_t zone_id, std::string* err = nullptr);
bool zone_owner_action_on_core_thread(int32_t zone_id, int32_t unit_id, std::string* err);
std::string zones_json_on_core_thread(const std::string& player, const Camera& camera,
                                      std::string* err = nullptr);

} // namespace dfcapture
