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

#include "worldmap_panel.h"

#include "Core.h"
#include "json_util.h"
#include "save_barrier.h"
#include "sdl_capture.h"

#include "modules/Translation.h"

#include "df/global_objects.h"
#include "df/army_controller.h"
#include "df/diplomacy_statest.h"
#include "df/entity_event.h"
#include "df/historical_entity.h"
#include "df/historical_entity_type.h"
#include "df/plotinfost.h"
#include "df/mission_report.h"
#include "df/region_map_entry.h"
#include "df/world.h"
#include "df/world_data.h"
#include "df/world_region.h"
#include "df/world_site.h"
#include "df/world_site_type.h"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <string>

using namespace DFHack;

namespace dfcapture {
namespace {

std::recursive_mutex g_worldmap_mutex;

template <typename Fn>
bool run_worldmap_locked(Fn&& fn) {
    std::lock_guard<std::recursive_mutex> worldmap_lock(g_worldmap_mutex);
    std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
    DFHack::CoreSuspender suspend;
    if (save_barrier_active())
        return false;
    return fn();
}

void set_no_store_json(httplib::Response& res, const std::string& json) {
    res.set_header("Cache-Control", "no-store");
    res.set_content(json, "application/json; charset=utf-8");
}

void json_error(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content("{\"ok\":false,\"error\":" + json_string(message) + "}\n",
                    "application/json; charset=utf-8");
}

std::string entity_name(df::historical_entity* entity) {
    if (!entity)
        return "";
    std::string name = DFHack::Translation::translateName(&entity->name, true);
    return name;
}

std::string site_name(df::world_data* wd, int32_t site_id) {
    if (!wd) return "";
    for (auto* site : wd->sites)
        if (site && site->id == site_id)
            return DFHack::Translation::translateName(&site->name, true);
    return "";
}

std::string diplomacy_relation_to(df::historical_entity* entity, int32_t group_id,
                                  int32_t civ_id) {
    if (!entity) return "Unknown";
    for (auto* state : entity->relations.diplomacy.state) {
        if (!state || (state->group_id != group_id && state->group_id != civ_id)) continue;
        std::string key = DFHack::enum_item_key(state->relation);
        return key.empty() ? "Unknown" : key;
    }
    return entity->id == civ_id ? "Own civilization" : "Unknown";
}

// Classify one region tile into a single biome char consumed by
// dfcapture-worldmap.js WORLD_TERRAIN_COLORS. Thresholds follow the df-structures field docs:
// elevation 0-99 = ocean/water, 150+ = mountains, 100-149 = all other biomes; vegetation/
// rainfall/salinity are 0-100. Kept deliberately coarse (land/water/mountain is high-confidence;
// vegetation only tints the land) so the map reads as DF's world map without inventing detail.
char classify_region_char(const df::region_map_entry& e) {
    int elev = e.elevation;
    if (elev < 100)
        return (e.salinity >= 33) ? '~' : 'l'; // salt ocean vs fresh lake
    if (elev >= 150)
        return '^';                             // mountains
    int veg = e.vegetation;
    if (veg >= 66) return 'T';                  // forest
    if (veg >= 33) return '.';                  // grassland / light vegetation
    if (e.rainfall < 33) return 'd';            // arid -> desert
    return 'n';                                 // barren / rock
}

// Emit the additive ",\"terrain\":{...}" field: a downsampled biome-char grid of the whole world
// (rows are y, chars are x). Emits NOTHING when region_map is unpopulated (pocket/degenerate world
// or pre-worldgen) so the client cleanly falls back. Runs inside the world lock/CoreSuspender
// (passive memory reads only); the grid is capped so the read stays cheap even on a 257-wide world.
void append_terrain_json(std::ostringstream& body, df::world_data* wd) {
    if (!wd || !wd->region_map || wd->world_width <= 0 || wd->world_height <= 0)
        return;
    const int W = wd->world_width, H = wd->world_height;
    const int MAX_DIM = 200; // cap the larger world dimension in samples
    int step = (std::max(W, H) + MAX_DIM - 1) / MAX_DIM;
    if (step < 1) step = 1;
    const int OW = (W + step - 1) / step;
    const int OH = (H + step - 1) / step;
    body << ",\"terrain\":{\"w\":" << OW << ",\"h\":" << OH << ",\"step\":" << step << ",\"rows\":[";
    std::string row;
    row.reserve(OW);
    for (int ty = 0; ty < OH; ++ty) {
        int wy = ty * step;
        if (wy >= H) wy = H - 1;
        row.clear();
        for (int tx = 0; tx < OW; ++tx) {
            int wx = tx * step;
            if (wx >= W) wx = W - 1;
            row.push_back(classify_region_char(wd->region_map[wx][wy]));
        }
        if (ty) body << ",";
        body << json_string(row);
    }
    body << "]}";
}

std::string build_worldmap_json(const std::string& player, std::string* err) {
    std::ostringstream body;
    bool ok = run_worldmap_locked([&]() -> bool {
        auto world = df::global::world;
        auto plotinfo = df::global::plotinfo;
        if (!world || !world->world_data) { if (err) *err = "world data unavailable"; return false; }
        auto wd = world->world_data;
        int32_t own_site = plotinfo ? plotinfo->site_id : -1;
        int32_t own_group = plotinfo ? plotinfo->group_id : -1;
        int32_t own_civ = plotinfo ? plotinfo->civ_id : -1;
        auto* fort_entity = df::historical_entity::find(own_group);

        // region_map uses world-tile coordinates, and each entry's region_id indexes
        // world_data->regions. Pocket worlds and early world loading may not provide the map.
        std::string region_name;
        for (auto site : wd->sites) {
            if (!site || site->id != own_site)
                continue;
            if (wd->region_map && wd->world_width > 0 && wd->world_height > 0 &&
                site->pos.x >= 0 && site->pos.x < wd->world_width &&
                site->pos.y >= 0 && site->pos.y < wd->world_height) {
                auto& entry = wd->region_map[site->pos.x][site->pos.y];
                int32_t region_id = entry.region_id;
                if (region_id >= 0 && static_cast<size_t>(region_id) < wd->regions.size() &&
                    wd->regions[region_id]) {
                    region_name = DFHack::Translation::translateName(&wd->regions[region_id]->name, true);
                }
            }
            break;
        }

        body << "{\"player\":" << json_string(player)
             << ",\"width\":" << wd->world_width
             << ",\"height\":" << wd->world_height
             << ",\"ownSiteId\":" << own_site
             << ",\"regionName\":" << json_string(region_name)
             << ",\"sites\":[";
        bool first = true;
        int count = 0;
        for (auto site : wd->sites) {
            if (!site)
                continue;
            if (!first) body << ",";
            first = false;
            std::string name = DFHack::Translation::translateName(&site->name, true);
            body << "{\"id\":" << site->id
                 << ",\"name\":" << json_string(name)
                 << ",\"type\":" << json_string(DFHack::enum_item_key(site->type))
                 << ",\"x\":" << site->pos.x
                 << ",\"y\":" << site->pos.y
                 << ",\"civId\":" << site->civ_id
                 << ",\"own\":" << (site->id == own_site ? "true" : "false")
                 << "}";
            if (++count >= 2000)
                break;
        }
        body << "],\"civs\":[";
        first = true;
        for (auto entity : world->entities.all) {
            if (!entity || entity->type != df::historical_entity_type::Civilization)
                continue;
            std::string name = entity_name(entity);
            if (name.empty())
                continue;
            if (!first) body << ",";
            first = false;
            int site_count = 0;
            for (auto* site : wd->sites)
                if (site && site->civ_id == entity->id) ++site_count;
            body << "{\"id\":" << entity->id
                 << ",\"name\":" << json_string(name)
                 << ",\"race\":" << entity->race
                 << ",\"siteCount\":" << site_count
                 << ",\"knownSiteCount\":" << entity->relations.known_sites.size()
                 << ",\"population\":" << std::max(0, entity->total_pop)
                 << ",\"relation\":" << json_string(diplomacy_relation_to(entity, own_group, own_civ))
                 << ",\"warFatigue\":" << entity->war_fatigue
                 << ",\"meetingCount\":" << entity->meeting_events.size()
                 << ",\"lastReportYear\":" << entity->last_report_year
                 << "}";
        }
        body << "],\"missions\":[";
        first = true;
        for (auto* controller : world->army_controllers.all) {
            if (!controller || controller->assigned_squads.empty()) continue;
            bool fort_mission = false;
            if (fort_entity) {
                for (int32_t squad_id : controller->assigned_squads)
                    if (std::find(fort_entity->squads.begin(), fort_entity->squads.end(), squad_id) != fort_entity->squads.end()) {
                        fort_mission = true;
                        break;
                    }
            }
            if (!fort_mission && controller->entity_id != own_group && controller->entity_id != own_civ) continue;
            if (!first) body << ",";
            first = false;
            std::string goal = DFHack::enum_item_key(controller->goal);
            std::string target = site_name(wd, controller->site_id);
            body << "{\"id\":" << controller->id
                 << ",\"goal\":" << json_string(goal.empty() ? "Unknown mission" : goal)
                 << ",\"targetSiteId\":" << controller->site_id
                 << ",\"targetSite\":" << json_string(target)
                 << ",\"year\":" << controller->year
                 << ",\"yearTick\":" << controller->year_tick
                 << ",\"reportTitle\":" << json_string(controller->mission_report ? controller->mission_report->title : "")
                 << ",\"squadIds\":[";
            for (size_t i = 0; i < controller->assigned_squads.size(); ++i) {
                if (i) body << ",";
                body << controller->assigned_squads[i];
            }
            body << "]}";
        }
        body << "],\"news\":[";
        first = true;
        auto append_news = [&](df::historical_entity* source) {
            if (!source) return;
            int emitted = 0;
            for (auto it = source->rumor_info.events.rbegin(); it != source->rumor_info.events.rend() && emitted < 200; ++it) {
                auto* event = *it;
                if (!event) continue;
                if (!first) body << ",";
                first = false;
                std::string type = DFHack::enum_item_key(event->type);
                body << "{\"sourceEntityId\":" << source->id
                     << ",\"source\":" << json_string(entity_name(source))
                     << ",\"type\":" << json_string(type.empty() ? "Unknown rumor" : type)
                     << ",\"year\":" << event->year
                     << ",\"yearTick\":" << event->year_tick << "}";
                ++emitted;
            }
        };
        append_news(fort_entity);
        if (own_civ != own_group) append_news(df::historical_entity::find(own_civ));
        body << "]";
        // Add the terrain biome grid so the world map renders coherently.
        append_terrain_json(body, wd);
        body << "}\n";
        return true;
    });
    if (!ok)
        return "";
    return body.str();
}

} // namespace

void register_worldmap_routes(httplib::Server& server) {
    // GET /world-map -> read-only world overview: sites, civilizations, fort marker.
    server.Get("/world-map", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        std::string err;
        std::string json = build_worldmap_json(player, &err);
        if (json.empty()) { json_error(res, 503, err.empty() ? "world map unavailable" : err); return; }
        set_no_store_json(res, json);
    });
}

} // namespace dfcapture
