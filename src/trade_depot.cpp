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

#include "trade_depot.h"
#include "fort_stock.h"
#include "interaction.h"

#include "Core.h"
#include "TileTypes.h"
#include "http_server.h"
#include "json_util.h"
#include "lua_bridge.h"
#include "native_trade_5315.h"
#include "save_barrier.h"
#include "sdl_capture.h"

#include "modules/Buildings.h"
#include "modules/Items.h"
#include "modules/Job.h"
#include "modules/Maps.h"
#include "modules/Translation.h"
#include "modules/Units.h"

#include "df/building.h"
#include "df/building_actual.h"
#include "df/building_bars_floorst.h"
#include "df/building_grate_floorst.h"
#include "df/building_hatchst.h"
#include "df/building_item_role_type.h"
#include "df/buildingitemst.h"
#include "df/building_tradedepotst.h"
#include "df/building_tradedepot_flag.h"
#include "df/caravan_state.h"
#include "df/creature_raw.h"
#include "df/entity_position.h"
#include "df/entity_position_assignment.h"
#include "df/entity_position_responsibility.h"
#include "df/gamest.h"
#include "df/global_objects.h"
#include "df/historical_entity.h"
#include "df/historical_figure.h"
#include "df/item.h"
#include "df/item_flags.h"
#include "df/job.h"
#include "df/job_handler.h"
#include "df/job_item_ref.h"
#include "df/job_list_link.h"
#include "df/job_type.h"
#include "df/main_interface.h"
#include "df/plot_merchant_flag.h"
#include "df/plotinfost.h"
#include "df/trade_interfacest.h"
#include "df/tile_building_occ.h"
#include "df/talk_line_type.h"
#include "df/unit.h"
#include "df/world.h"

#include <mutex>
#include <chrono>
#include <climits>
#include <iomanip>
#include <random>
#include <sstream>
#include <stack>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace DFHack;

namespace dfcapture {
namespace {

std::recursive_mutex g_depot_mutex;

struct PendingCounterOffer {
    std::string player;
    int32_t depot_id = -1;
    int32_t caravan_index = -1;
    std::vector<NativeTradeSelection> merchant;
    std::vector<NativeTradeSelection> fortress;
    std::vector<int32_t> counter_offer_ids;
    std::chrono::steady_clock::time_point expires;
};

std::unordered_map<std::string, PendingCounterOffer> g_pending_counteroffers;

void purge_expired_counteroffers() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_pending_counteroffers.begin(); it != g_pending_counteroffers.end();) {
        if (it->second.expires <= now)
            it = g_pending_counteroffers.erase(it);
        else
            ++it;
    }
}

std::string counteroffer_token() {
    static std::mt19937_64 random(std::random_device{}());
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << random()
        << std::setw(16) << random();
    return out.str();
}

// Lock order: panel mutex -> capture-state mutex -> CoreSuspender. Reads and mutations use the
// same guard.
template <typename Fn>
bool run_depot_locked(Fn&& fn) {
    std::lock_guard<std::recursive_mutex> depot_lock(g_depot_mutex);
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

df::building_tradedepotst* resolve_depot(int32_t id) {
    return virtual_cast<df::building_tradedepotst>(df::building::find(id));
}

bool depot_built(df::building* b) {
    return b && b->getBuildStage() >= b->getMaxBuildStage();
}

// building_tradedepotst::accessible is caravan-maintained state and can remain false when no
// caravan is present. Compute live wagon access from the requested depot instead.
struct WagonFloodContext {
    uint16_t walk_group;
    std::unordered_set<df::coord> seen;
    std::stack<df::coord> search_edge;
    const std::unordered_set<df::coord>& entry_tiles;

    WagonFloodContext(uint16_t group, const std::unordered_set<df::coord>& entries)
        : walk_group(group), entry_tiles(entries) {}
};

bool wagon_dynamic_traversible(df::tiletype_shape shape, const df::coord& pos) {
    auto bld = DFHack::Buildings::findAtTile(pos);
    if (!bld)
        return false;
    if (bld->getType() == df::building_type::Hatch) {
        auto hatch = virtual_cast<df::building_hatchst>(bld);
        return hatch && shape != df::tiletype_shape::RAMP_TOP && hatch->door_flags.bits.closed;
    }
    if (bld->getType() == df::building_type::GrateFloor) {
        auto grate = virtual_cast<df::building_grate_floorst>(bld);
        return grate && grate->gate_flags.bits.closed;
    }
    if (bld->getType() == df::building_type::BarsFloor) {
        auto bars = virtual_cast<df::building_bars_floorst>(bld);
        return bars && bars->gate_flags.bits.closed;
    }
    return false;
}

bool wagon_tile_traversible(df::tiletype tt) {
    auto shape = tileShape(tt);
    if (shape == df::tiletype_shape::RAMP_TOP)
        return true;
    if (shape == df::tiletype_shape::STAIR_UP || shape == df::tiletype_shape::STAIR_DOWN ||
        shape == df::tiletype_shape::STAIR_UPDOWN || shape == df::tiletype_shape::BOULDER ||
        shape == df::tiletype_shape::EMPTY || shape == df::tiletype_shape::NONE)
        return false;
    if (tileSpecial(tt) == df::tiletype_special::TRACK)
        return false;
    auto material = tileMaterial(tt);
    return material != df::tiletype_material::POOL && material != df::tiletype_material::RIVER;
}

bool wagon_traversible(WagonFloodContext& ctx, const df::coord& pos,
                       const df::coord& prev_pos) {
    auto tt = DFHack::Maps::getTileType(pos);
    auto occupancy = DFHack::Maps::getTileOccupancy(pos);
    if (!tt || !occupancy)
        return false;
    auto shape = tileShape(*tt);
    switch (occupancy->bits.building) {
    case df::tile_building_occ::Obstacle:
    case df::tile_building_occ::Well:
    case df::tile_building_occ::Impassable:
        return false;
    case df::tile_building_occ::Dynamic:
        if (!wagon_dynamic_traversible(shape, pos))
            return false;
        break;
    case df::tile_building_occ::None:
    case df::tile_building_occ::Planned:
    case df::tile_building_occ::Passable:
        if (!wagon_tile_traversible(*tt))
            return false;
        break;
    case df::tile_building_occ::Floored:
        break;
    }

    if (ctx.walk_group == DFHack::Maps::getWalkableGroup(pos))
        return true;
    if (shape == df::tiletype_shape::RAMP_TOP) {
        df::coord below = pos + df::coord(0, 0, -1);
        if (DFHack::Maps::getWalkableGroup(below)) {
            ctx.search_edge.emplace(below);
            return true;
        }
    } else if (shape == df::tiletype_shape::WALL) {
        auto prev_tt = DFHack::Maps::getTileType(prev_pos);
        if (prev_tt && tileShape(*prev_tt) == df::tiletype_shape::RAMP) {
            df::coord above = pos + df::coord(0, 0, 1);
            if (DFHack::Maps::getWalkableGroup(above)) {
                ctx.search_edge.emplace(above);
                return true;
            }
        }
    }
    return false;
}

void check_wagon_tile(WagonFloodContext& ctx, const df::coord& pos) {
    if (!ctx.seen.emplace(pos).second)
        return;
    if (ctx.entry_tiles.contains(pos)) {
        ctx.search_edge.emplace(pos);
        return;
    }
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            if (!wagon_traversible(ctx, pos + df::coord(dx, dy, 0), pos))
                return;
    ctx.search_edge.emplace(pos);
}

bool depot_accessible_by_wagons(df::building_tradedepotst* depot) {
    if (!depot || !df::global::plotinfo)
        return false;
    df::coord center(depot->centerx, depot->centery, depot->z);
    auto walk_group = DFHack::Maps::getWalkableGroup(center);
    if (!walk_group)
        return false;

    std::unordered_set<df::coord> entry_tiles;
    uint32_t count_x = 0, count_y = 0, count_z = 0;
    DFHack::Maps::getTileSize(count_x, count_y, count_z);
    if (!count_x || !count_y)
        return false;
    auto& edge = df::global::plotinfo->map_edge;
    for (size_t i = 0; i < edge.surface_x.size(); ++i) {
        df::coord pos(edge.surface_x[i], edge.surface_y[i], edge.surface_z[i]);
        if ((pos.x == 0 || pos.y == 0 || pos.x == count_x - 1 || pos.y == count_y - 1) &&
            DFHack::Maps::getWalkableGroup(pos) == walk_group)
            entry_tiles.emplace(pos);
    }
    if (entry_tiles.empty())
        return false;

    WagonFloodContext ctx(walk_group, entry_tiles);
    ctx.seen.emplace(center);
    ctx.search_edge.emplace(center);
    while (!ctx.search_edge.empty()) {
        df::coord pos = ctx.search_edge.top();
        ctx.search_edge.pop();
        if (entry_tiles.contains(pos))
            return true;
        check_wagon_tile(ctx, pos + df::coord(0, -1, 0));
        check_wagon_tile(ctx, pos + df::coord(0, 1, 0));
        check_wagon_tile(ctx, pos + df::coord(-1, 0, 0));
        check_wagon_tile(ctx, pos + df::coord(1, 0, 0));
    }
    return false;
}

// True iff the depot hosts a live TradeAtDepot job (the broker-comes-to-depot job DF spawns when
// a trader is requested and a caravan is present).
bool depot_has_trade_job(df::building_tradedepotst* depot) {
    for (auto* job : depot->jobs)
        if (job && job->job_type == df::job_type::TradeAtDepot)
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// Broker: the fort noble whose position carries the TRADE responsibility.
// ---------------------------------------------------------------------------
struct BrokerInfo {
    bool found = false;
    int32_t unit_id = -1;
    std::string name;
    std::string position;
};

BrokerInfo find_broker() {
    BrokerInfo out;
    auto plotinfo = df::global::plotinfo;
    if (!plotinfo)
        return out;
    auto fort = df::historical_entity::find(plotinfo->group_id);
    if (!fort)
        return out;
    for (auto position : fort->positions.own) {
        if (!position)
            continue;
        if (!position->responsibilities[df::entity_position_responsibility::TRADE])
            continue;
        out.position = position->name[0].empty() ? std::string("Broker") : position->name[0];
        // Find the assignment holder for this position.
        int32_t holder_hf = -1;
        for (auto asn : fort->positions.assignments) {
            if (asn && asn->position_id == position->id && asn->histfig != -1) {
                holder_hf = asn->histfig;
                break;
            }
        }
        if (holder_hf != -1) {
            df::unit* holder = nullptr;
            for (auto u : df::global::world->units.active) {
                if (u && u->hist_figure_id == holder_hf) { holder = u; break; }
            }
            if (holder) {
                out.found = true;
                out.unit_id = holder->id;
                out.name = DFHack::Units::getReadableName(holder);
            } else {
                auto hf = df::historical_figure::find(holder_hf);
                if (hf) {
                    out.found = true;
                    out.name = DFHack::Translation::translateName(&hf->name, true);
                }
            }
        }
        break; // one TRADE position (the Broker)
    }
    return out;
}

std::string broker_json(const BrokerInfo& b) {
    std::ostringstream js;
    js << "{\"found\":" << (b.found ? "true" : "false")
       << ",\"unitId\":" << b.unit_id
       << ",\"name\":" << json_string(b.name)
       << ",\"position\":" << json_string(b.position) << "}";
    return js.str();
}

// ---------------------------------------------------------------------------
// Caravans from plotinfo->caravans.
// ---------------------------------------------------------------------------
std::string caravan_origin_name(df::caravan_state* car) {
    auto entity = df::historical_entity::find(car->entity);
    if (!entity)
        return "Unknown caravan";
    std::string civ = DFHack::Translation::translateName(&entity->name, false);
    std::string adjective;
    if (auto craw = df::creature_raw::find(entity->race)) {
        adjective = craw->name[2];   // fixed std::string[3]: [singular, plural, adjective]
    }
    if (!adjective.empty() && !civ.empty())
        return adjective + " caravan from " + civ;
    if (!civ.empty())
        return "Caravan from " + civ;
    return "Unknown caravan";
}

bool caravan_is_active(df::caravan_state* car) {
    if (car->flags.bits.tribute)
        return false;
    return car->time_remaining > 0 &&
        (car->trade_state == df::caravan_state::T_trade_state::Approaching ||
         car->trade_state == df::caravan_state::T_trade_state::AtDepot);
}

void append_caravan_flags(std::ostringstream& js, df::caravan_state* car) {
    js << "[";
    bool first = true;
    auto add = [&](bool cond, const char* label) {
        if (!cond) return;
        if (!first) js << ",";
        first = false;
        js << json_string(label);
    };
    add(car->flags.bits.casualty, "Casualty");
    add(car->flags.bits.hardship, "Encountered hardship");
    add(car->flags.bits.seized, "Goods seized");
    add(car->flags.bits.offended, "Offended");
    add(car->flags.bits.greatly_offended, "Greatly offended");
    add(car->flags.bits.tribute, "Tribute");
    js << "]";
}

void append_caravan_json(std::ostringstream& js, df::caravan_state* car, int idx) {
    int days = car->time_remaining > 0 ? car->time_remaining / 120 : 0;
    bool at_depot = car->trade_state == df::caravan_state::T_trade_state::AtDepot;
    js << "{\"index\":" << idx
       << ",\"origin\":" << json_string(caravan_origin_name(car))
       << ",\"state\":" << json_string(DFHack::enum_item_key(car->trade_state))
       << ",\"active\":" << (caravan_is_active(car) ? "true" : "false")
       << ",\"atDepot\":" << (at_depot ? "true" : "false")
       << ",\"tribute\":" << (car->flags.bits.tribute ? "true" : "false")
       << ",\"daysRemaining\":" << days
       << ",\"importValue\":" << car->import_value
       << ",\"exportValue\":" << car->export_value_total
       << ",\"offerValue\":" << car->offer_value
       << ",\"goodsCount\":" << static_cast<int>(car->goods.size())
       << ",\"animalCount\":" << static_cast<int>(car->animals.size())
       << ",\"flags\":";
    append_caravan_flags(js, car);
    js << "}";
}

// ---------------------------------------------------------------------------
// Tradeable-goods enumeration excludes grouped container contents and inventory-held items.
// ---------------------------------------------------------------------------
bool item_hard_rejected(df::item* item) {
    auto& f = item->flags;
    return f.bits.hostile || f.bits.removed || f.bits.dead_dwarf || f.bits.spider_web ||
           f.bits.construction || f.bits.encased || f.bits.murder || f.bits.trader ||
           f.bits.owned || f.bits.garbage_collect || f.bits.on_fire;
}

// Does this item already have a BringItemToDepot job (i.e. it is pending / marked for trade)?
df::job* find_bring_to_depot_job(int32_t item_id) {
    auto world = df::global::world;
    for (df::job_list_link* link = world->jobs.list.next; link; link = link->next) {
        df::job* job = link->item;
        if (!job || job->job_type != df::job_type::BringItemToDepot)
            continue;
        for (auto* ref : job->items)
            if (ref && ref->item && ref->item->id == item_id)
                return job;
    }
    return nullptr;
}

bool item_is_tradeable(df::item* item, df::building_tradedepotst* depot, bool* out_pending) {
    if (out_pending) *out_pending = false;
    if (!is_fort_stock_item(item, FortItemPurpose::TradeDepot) || item_hard_rejected(item))
        return false;
    // Skip items inside another creature/container inventory (default movegoods view).
    if (item->flags.bits.in_inventory)
        return false;
    if (item->flags.bits.in_job) {
        // Busy in a job: tradeable only if that job is a BringItemToDepot (already pending).
        if (find_bring_to_depot_job(item->id)) { if (out_pending) *out_pending = true; return true; }
        return false;
    }
    if (item->flags.bits.in_building) {
        // Part of a building: tradeable only if it is sitting at THIS depot (already at depot).
        if (DFHack::Items::getHolderBuilding(item) != depot)
            return false;
        if (out_pending) *out_pending = true;
        return true;
    }
    // Loose item: must be able to walk from the item to the depot centre.
    df::coord ipos = DFHack::Items::getPosition(item);
    if (!ipos.isValid())
        return false;
    return DFHack::Maps::canWalkBetween(ipos, df::coord(depot->centerx, depot->centery, depot->z));
}

// ---------------------------------------------------------------------------
// JSON builders
// ---------------------------------------------------------------------------
std::string build_depot_info_json(int32_t id, std::string* err) {
    std::ostringstream js;
    bool ok = run_depot_locked([&]() -> bool {
        auto depot = resolve_depot(id);
        if (!depot) { if (err) *err = "not a trade depot"; return false; }
        auto plotinfo = df::global::plotinfo;

        // Count goods physically at the depot + pending haul jobs (cheap scans).
        int at_depot = 0, pending = 0;
        std::vector<df::item*> construction_materials;
        {
            auto world = df::global::world;
            for (df::job_list_link* link = world->jobs.list.next; link; link = link->next) {
                df::job* job = link->item;
                if (job && job->job_type == df::job_type::BringItemToDepot)
                    ++pending;
            }
            // TEMP-role items are trade goods at the depot; PERM-role items are the depot's own
            // construction materials and must be excluded.
            for (auto* ci : depot->contained_items) {
                if (!ci || !ci->item)
                    continue;
                if (ci->use_mode == df::building_item_role_type::TEMP)
                    ++at_depot;
                else if (ci->use_mode == df::building_item_role_type::PERM)
                    construction_materials.push_back(ci->item);
            }
        }

        BrokerInfo broker = find_broker();
        bool has_active = false;
        bool accessible = depot_accessible_by_wagons(depot);
        js << "{\"ok\":true,\"id\":" << id
           << ",\"isDepot\":true"
           << ",\"name\":" << json_string(DFHack::Buildings::getName(depot))
           << ",\"built\":" << (depot_built(depot) ? "true" : "false")
           << ",\"accessible\":" << (accessible ? "true" : "false")
           << ",\"storedAccessible\":" << (depot->accessible ? "true" : "false")
           << ",\"traderRequested\":" << (depot->trade_flags.bits.trader_requested ? "true" : "false")
           << ",\"anyoneCanTrade\":" << (depot->trade_flags.bits.anyone_can_trade ? "true" : "false")
           << ",\"timesUsed\":" << depot->times_used
           << ",\"hasTradeJob\":" << (depot_has_trade_job(depot) ? "true" : "false")
           << ",\"goodsAtDepot\":" << at_depot
           << ",\"pendingCount\":" << pending
           << ",\"broker\":" << broker_json(broker)
           << ",\"constructionMaterials\":[";
        for (size_t i = 0; i < construction_materials.size(); ++i) {
            auto* item = construction_materials[i];
            if (i) js << ",";
            js << "{\"id\":" << item->id
               << ",\"desc\":" << json_string(DFHack::Items::getDescription(item, 0, false))
               << ",\"forbidden\":" << (item->flags.bits.forbid ? "true" : "false")
               << ",\"dump\":" << (item->flags.bits.dump ? "true" : "false") << "}";
        }
        js << "],\"caravans\":[";
        if (plotinfo) {
            bool first = true;
            for (size_t i = 0; i < plotinfo->caravans.size(); ++i) {
                auto car = plotinfo->caravans[i];
                if (!car) continue;
                if (caravan_is_active(car)) has_active = true;
                if (!first) js << ",";
                first = false;
                append_caravan_json(js, car, (int)i);
            }
        }
        js << "],\"hasActiveCaravan\":" << (has_active ? "true" : "false") << "}\n";
        return true;
    });
    if (!ok)
        return "";
    return js.str();
}

std::string build_depot_goods_json(int32_t id, bool all, std::string* err) {
    static const int kCap = 400;
    std::ostringstream js;
    bool ok = run_depot_locked([&]() -> bool {
        auto depot = resolve_depot(id);
        if (!depot) { if (err) *err = "not a trade depot"; return false; }
        auto world = df::global::world;

        js << "{\"ok\":true,\"id\":" << id << ",\"goods\":[";
        int emitted = 0;
        bool truncated = false;
        bool first = true;
        auto& in_play = world->items.other[df::items_other_id::IN_PLAY];
        for (auto* item : in_play) {
            if (!item) continue;
            bool pending = false;
            if (!item_is_tradeable(item, depot, &pending)) continue;
            int value = DFHack::Items::getValue(item);
            if (value <= 0) continue;                       // movegoods: no worthless rows
            if (!all && emitted >= kCap) { truncated = true; break; }
            df::coord ipos = DFHack::Items::getPosition(item);
            int dist = 0;
            if (ipos.isValid())
                dist = std::max(std::abs(depot->centerx - ipos.x), std::abs(depot->centery - ipos.y)) +
                       std::abs(depot->z - ipos.z);
            bool at_depot = item->flags.bits.in_building;
            if (!first) js << ",";
            first = false;
            js << "{\"id\":" << item->id
               << ",\"desc\":" << json_string(item_display_name(item, 0, true))
               << ",\"category\":" << json_string(DFHack::enum_item_key(item->getType()))
               << ",\"value\":" << value
               << ",\"dist\":" << dist
               << ",\"pending\":" << (pending ? "true" : "false")
               << ",\"atDepot\":" << (at_depot ? "true" : "false")
               << ",\"forbidden\":" << (item->flags.bits.forbid ? "true" : "false")
               << ",\"requested\":" << (DFHack::Items::isRequestedTradeGood(item) ? "true" : "false")
               << "}";
            ++emitted;
        }
        js << "],\"count\":" << emitted
           << ",\"truncated\":" << (truncated ? "true" : "false")
           << ",\"cap\":" << kCap << "}\n";
        return true;
    });
    if (!ok)
        return "";
    return js.str();
}

void append_barter_item_json(std::ostringstream& js, df::item* item,
                             df::caravan_state* caravan) {
    int32_t whole = 0, fraction = 0;
    native_trade_item_mass(item, 0, &whole, &fraction);
    js << "{\"id\":" << item->id
       << ",\"desc\":" << json_string(item_display_name(item, 0, true))
       << ",\"category\":" << json_string(DFHack::enum_item_key(item->getType()))
       << ",\"value\":" << native_trade_item_value(item, caravan, 0)
       << ",\"stack\":" << std::max(1, item->getStackSize())
       << ",\"massWhole\":" << whole
       << ",\"massFraction\":" << fraction
       << ",\"artifact\":" << (item->flags.bits.artifact ? "true" : "false")
       << ",\"foreign\":" << (item->flags.bits.foreign ? "true" : "false")
       << "}";
}

std::string build_barter_json(int32_t id, int32_t caravan_index, std::string* err) {
    std::ostringstream js;
    bool ok = run_depot_locked([&]() -> bool {
        auto* depot = resolve_depot(id);
        if (!depot) { if (err) *err = "not a trade depot"; return false; }
        df::trade_interfacest state;
        int32_t resolved_index = -1;
        if (!native_trade_prepare(depot, caravan_index, state, &resolved_index, err))
            return false;
        std::string civ = state.civ
            ? DFHack::Translation::translateName(&state.civ->name, false)
            : std::string();
        js << "{\"ok\":true,\"nativeVersion\":\"53.15\""
           << ",\"depotId\":" << id
           << ",\"caravanIndex\":" << resolved_index
           << ",\"merchantCiv\":" << json_string(civ)
           << ",\"talker\":" << json_string(state.talker)
           << ",\"fortressTrader\":"
           << json_string(state.fortress_trader
                ? DFHack::Units::getReadableName(state.fortress_trader) : std::string())
           << ",\"merchantMood\":" << state.mer->mood
           << ",\"merchant\":[";
        for (size_t i = 0; i < state.good[0].size(); ++i) {
            if (i) js << ",";
            append_barter_item_json(js, state.good[0][i], state.mer);
        }
        js << "],\"fortress\":[";
        for (size_t i = 0; i < state.good[1].size(); ++i) {
            if (i) js << ",";
            append_barter_item_json(js, state.good[1][i], state.mer);
        }
        js << "]}\n";
        return true;
    });
    return ok ? js.str() : std::string();
}

bool parse_trade_selections(const std::string& raw,
                            std::vector<NativeTradeSelection>* out,
                            std::string* err) {
    out->clear();
    if (raw.empty())
        return true;
    size_t start = 0;
    while (start < raw.size()) {
        size_t comma = raw.find(',', start);
        std::string part = raw.substr(start,
            comma == std::string::npos ? std::string::npos : comma - start);
        size_t colon = part.find(':');
        std::string id_text = part.substr(0, colon);
        std::string amount_text = colon == std::string::npos ? "0" : part.substr(colon + 1);
        try {
            size_t id_used = 0, amount_used = 0;
            long id = std::stol(id_text, &id_used);
            long amount = std::stol(amount_text, &amount_used);
            if (id_used != id_text.size() || amount_used != amount_text.size() ||
                id < 0 || id > INT32_MAX || amount < 0 || amount > INT32_MAX) {
                if (err) *err = "invalid trade selection";
                return false;
            }
            out->push_back({static_cast<int32_t>(id), static_cast<int32_t>(amount)});
        } catch (...) {
            if (err) *err = "invalid trade selection";
            return false;
        }
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return true;
}

std::string talkline_message(int32_t talkline) {
    switch (static_cast<df::talk_line_type>(talkline)) {
    case df::talk_line_type::Trade:
        return "The merchants have agreed to the trade.";
    case df::talk_line_type::CounterOffer:
        return "The merchants will agree if you include their requested counteroffer.";
    case df::talk_line_type::CouldNotFindCounterOffer1:
    case df::talk_line_type::CouldNotFindCounterOffer2:
        return "The offer is too low, and the merchants could not find a suitable counteroffer.";
    case df::talk_line_type::NoMoreTradeHaggleFailure:
        return "The merchants have lost patience and will not trade further.";
    case df::talk_line_type::ICannotAfford:
        return "The merchants cannot carry or afford this exchange.";
    case df::talk_line_type::YouCannotAfford:
        return "Your offer is not sufficient.";
    case df::talk_line_type::AnimalTreeReject:
    case df::talk_line_type::AnimalReject:
    case df::talk_line_type::TreeReject:
    case df::talk_line_type::LiveAnimalReject:
        return "The merchants reject one or more restricted goods.";
    case df::talk_line_type::NoGoods:
    case df::talk_line_type::NoGoods1:
    case df::talk_line_type::NoGoods2:
        return "No goods are selected.";
    case df::talk_line_type::FortNone:
    case df::talk_line_type::FortNone1:
    case df::talk_line_type::FortNone2:
        return "Select fortress goods to offer.";
    case df::talk_line_type::TraderNone:
    case df::talk_line_type::TraderNone1:
    case df::talk_line_type::TraderNone2:
        return "Select merchant goods to receive.";
    default:
        return "The merchants declined this exchange.";
    }
}

std::string build_trade_status_json(int32_t id, std::string* err) {
    std::ostringstream js;
    bool ok = run_depot_locked([&]() -> bool {
        auto depot = resolve_depot(id);
        if (!depot) { if (err) *err = "not a trade depot"; return false; }
        auto game = df::global::game;
        auto& tr = game->main_interface.trade;
        bool open = tr.open;
        js << "{\"ok\":true,\"id\":" << id
           << ",\"tradeScreenOpen\":" << (open ? "true" : "false");
        if (open) {
            std::string civ;
            if (tr.civ)
                civ = DFHack::Translation::translateName(&tr.civ->name, false);
            js << ",\"merchantCiv\":" << json_string(civ)
               << ",\"stillUnloading\":" << (int)tr.stillunloading
               << ",\"haveTalker\":" << (int)tr.havetalker
               << ",\"caravanGoods\":" << static_cast<int>(tr.good[0].size())
               << ",\"fortGoods\":" << static_cast<int>(tr.good[1].size());
        }
        // The barter CONFIRM (trade/offer/seize) is host-native -- see /depot-trade.
        js << ",\"confirmHostNative\":true}\n";
        return true;
    });
    if (!ok)
        return "";
    return js.str();
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------
// Mark or unmark one item for trade at this depot:
//   on=1, item already at depot (holder==depot) -> item.flags.in_building = true
//   on=1, otherwise -> clear forbid + Items::markForTrade(item, depot)  [creates BringItemToDepot]
//   on=0, item has a BringItemToDepot job -> Job::removeJob(job)
//   on=0, item at depot (in_building && holder==depot) -> item.flags.in_building = false
int do_depot_mark(int32_t id, int32_t item_id, bool on, std::string* err) {
    int result = 0; // 1 = marked, 2 = at-depot toggled on, -1 unmarked-job, -2 at-depot off, 0 noop
    bool ok = run_depot_locked([&]() -> bool {
        auto depot = resolve_depot(id);
        if (!depot) { if (err) *err = "not a trade depot"; return false; }
        auto item = df::item::find(item_id);
        if (!item) { if (err) *err = "item not found"; return false; }
        if (on) {
            if (DFHack::Items::getHolderBuilding(item) == depot) {
                item->flags.bits.in_building = true;
                result = 2;
                return true;
            }
            item->flags.bits.forbid = false;
            if (!DFHack::Items::markForTrade(item, depot)) {
                if (err) *err = "markForTrade failed (item not reachable or already assigned)";
                return false;
            }
            result = 1;
            return true;
        }
        // unmark
        if (df::job* job = find_bring_to_depot_job(item_id)) {
            DFHack::Job::removeJob(job);
            result = -1;
            return true;
        }
        if (item->flags.bits.in_building && DFHack::Items::getHolderBuilding(item) == depot) {
            item->flags.bits.in_building = false;
            result = -2;
            return true;
        }
        result = 0; // nothing to do
        return true;
    });
    if (!ok)
        return -100;
    return result;
}

// Toggle the depot's trade flags. Clearing trader_requested also removes any live TradeAtDepot
// job on the depot. The anyone parameter controls anyone_can_trade.
bool do_depot_broker(int32_t id, bool has_request, bool request, bool has_anyone, bool anyone,
                     std::string* err) {
    return run_depot_locked([&]() -> bool {
        auto depot = resolve_depot(id);
        if (!depot) { if (err) *err = "not a trade depot"; return false; }
        if (has_request) {
            depot->trade_flags.bits.trader_requested = request;
            if (!request) {
                // Recalling the broker removes the active TradeAtDepot job.
                for (auto* job : depot->jobs) {
                    if (job && job->job_type == df::job_type::TradeAtDepot) {
                        DFHack::Job::removeJob(job);
                        break;
                    }
                }
            }
        }
        if (has_anyone)
            depot->trade_flags.bits.anyone_can_trade = anyone;
        return true;
    });
}

} // namespace

void register_trade_depot_routes(httplib::Server& server) {
    // GET /depots -> every trade depot in the fort (id + name + built), so the panel can discover
    // one without a map click. Empty list (never an error) when none is built yet.
    server.Get("/depots", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        std::ostringstream body;
        bool ok = run_depot_locked([&]() -> bool {
            auto world = df::global::world;
            body << "{\"player\":" << json_string(player) << ",\"depots\":[";
            bool first = true;
            if (world) {
                for (auto b : world->buildings.all) {
                    auto depot = virtual_cast<df::building_tradedepotst>(b);
                    if (!depot) continue;
                    if (!first) body << ",";
                    first = false;
                    body << "{\"id\":" << depot->id
                         << ",\"name\":" << json_string(DFHack::Buildings::getName(depot))
                         << ",\"built\":" << (depot_built(depot) ? "true" : "false") << "}";
                }
            }
            body << "]}\n";
            return true;
        });
        if (!ok) { json_error(res, 503, "depot list unavailable"); return; }
        set_no_store_json(res, body.str());
    });

    // GET /depot-info?id= -> depot state, caravan roster, broker presence, goods counts.
    server.Get("/depot-info", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) { json_error(res, 400, "missing id"); return; }
        std::string err;
        std::string json = build_depot_info_json(id, &err);
        if (json.empty()) { json_error(res, 400, err.empty() ? "depot unavailable" : err); return; }
        set_no_store_json(res, json);
    });

    // GET /depot-goods?id=[&all=1] -> tradeable fort-item rows (capped 400 unless all=1).
    server.Get("/depot-goods", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) { json_error(res, 400, "missing id"); return; }
        bool all = req.get_param_value("all") == "1";
        std::string err;
        std::string json = build_depot_goods_json(id, all, &err);
        if (json.empty()) { json_error(res, 400, err.empty() ? "goods unavailable" : err); return; }
        set_no_store_json(res, json);
    });

    // POST /depot-mark?id=&item=&on=0|1 -> mark/unmark one item for trade.
    auto mark_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1, item = -1;
        if (!query_int(req, "id", id) || !query_int(req, "item", item)) {
            json_error(res, 400, "missing id/item"); return;
        }
        bool on = req.get_param_value("on") != "0";  // default = mark
        std::string err;
        int r = do_depot_mark(id, item, on, &err);
        if (r == -100) { json_error(res, 400, err.empty() ? "mark failed" : err); return; }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true,\"result\":" + std::to_string(r) + "}\n");
    };
    server.Get("/depot-mark", mark_handler);
    server.Post("/depot-mark", mark_handler);

    // POST /depot-broker?id=[&request=0|1][&anyone=0|1] -> toggle depot trade flags.
    auto broker_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) { json_error(res, 400, "missing id"); return; }
        bool has_request = req.has_param("request");
        bool request = has_request && req.get_param_value("request") != "0";
        bool has_anyone = req.has_param("anyone");
        bool anyone = has_anyone && req.get_param_value("anyone") != "0";
        if (!has_request && !has_anyone) { json_error(res, 400, "no flag specified"); return; }
        std::string err;
        if (!do_depot_broker(id, has_request, request, has_anyone, anyone, &err)) {
            json_error(res, 400, err.empty() ? "broker toggle failed" : err); return;
        }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/depot-broker", broker_handler);
    server.Post("/depot-broker", broker_handler);

    // GET /depot-trade-status?id= -> native trade-screen read (open? who? counts?).
    server.Get("/depot-trade-status", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) { json_error(res, 400, "missing id"); return; }
        std::string err;
        std::string json = build_trade_status_json(id, &err);
        if (json.empty()) { json_error(res, 400, err.empty() ? "trade status unavailable" : err); return; }
        set_no_store_json(res, json);
    });

    // GET /depot-trade?id=[&caravan=] -> live merchant + fortress barter inventories.
    // This builds a transient closed trade_interfacest. It never opens or drives the host UI.
    server.Get("/depot-trade", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1, caravan = -1;
        if (!query_int(req, "id", id)) { json_error(res, 400, "missing id"); return; }
        if (req.has_param("caravan") && !query_int(req, "caravan", caravan)) {
            json_error(res, 400, "invalid caravan"); return;
        }
        std::string err;
        std::string json = build_barter_json(id, caravan, &err);
        if (json.empty()) {
            json_error(res, 409, err.empty() ? "barter is not ready" : err);
            return;
        }
        set_no_store_json(res, json);
    });

    // POST /depot-trade
    //   action=trade  merchant=id:amount,... fort=id:amount,...
    //   action=accept token=<server-held-counteroffer-token>
    //   action=decline token=<server-held-counteroffer-token>
    //
    // Only the selection vectors are assembled here. Steam DF 53.15's standalone native barter
    // function owns counteroffer choice and the complete item/caravan/entity/history mutation.
    // Counteroffer acceptance is server-held so a client cannot forge the bypass flag.
    server.Post("/depot-trade", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1, caravan = -1;
        if (!query_int(req, "id", id)) { json_error(res, 400, "missing id"); return; }
        if (req.has_param("caravan") && !query_int(req, "caravan", caravan)) {
            json_error(res, 400, "invalid caravan"); return;
        }
        const std::string player = query_player(req);
        const std::string action = req.has_param("action")
            ? req.get_param_value("action") : "trade";
        const std::string token = req.has_param("token")
            ? req.get_param_value("token") : std::string();

        if (action == "decline") {
            bool removed = false;
            run_depot_locked([&]() -> bool {
                purge_expired_counteroffers();
                auto it = g_pending_counteroffers.find(token);
                if (it != g_pending_counteroffers.end() && it->second.player == player &&
                    it->second.depot_id == id) {
                    g_pending_counteroffers.erase(it);
                    removed = true;
                }
                return true;
            });
            if (!removed) { json_error(res, 404, "counteroffer expired or not found"); return; }
            set_no_store_json(res, "{\"ok\":true,\"declined\":true}\n");
            return;
        }

        std::vector<NativeTradeSelection> merchant, fortress;
        std::vector<int32_t> counter_ids;
        bool accepting = action == "accept";
        if (accepting) {
            std::string lookup_error;
            bool found = run_depot_locked([&]() -> bool {
                purge_expired_counteroffers();
                auto it = g_pending_counteroffers.find(token);
                if (it == g_pending_counteroffers.end() || it->second.player != player ||
                    it->second.depot_id != id) {
                    lookup_error = "counteroffer expired or not found";
                    return false;
                }
                caravan = it->second.caravan_index;
                merchant = it->second.merchant;
                fortress = it->second.fortress;
                counter_ids = it->second.counter_offer_ids;
                // Consume before commit. A lost HTTP response can never replay an atomic trade.
                g_pending_counteroffers.erase(it);
                return true;
            });
            if (!found) { json_error(res, 409, lookup_error); return; }
        } else if (action == "trade") {
            std::string parse_error;
            const std::string merchant_raw = req.has_param("merchant")
                ? req.get_param_value("merchant") : std::string();
            const std::string fort_raw = req.has_param("fort")
                ? req.get_param_value("fort") : std::string();
            if (!parse_trade_selections(merchant_raw, &merchant, &parse_error) ||
                !parse_trade_selections(fort_raw, &fortress, &parse_error)) {
                json_error(res, 400, parse_error); return;
            }
        } else {
            json_error(res, 400, "unknown trade action");
            return;
        }

        NativeTradeOfferResult result;
        std::string execute_error;
        std::string response;
        bool executed = run_depot_locked([&]() -> bool {
            auto* depot = resolve_depot(id);
            if (!depot) { execute_error = "not a trade depot"; return false; }
            if (!native_trade_execute(depot, caravan, merchant, fortress, counter_ids,
                                      accepting, &result, &execute_error))
                return false;

            std::string pending_token;
            if (result.counter_offer && !accepting) {
                PendingCounterOffer pending;
                pending.player = player;
                pending.depot_id = id;
                pending.caravan_index = caravan;
                pending.merchant = merchant;
                pending.fortress = fortress;
                pending.counter_offer_ids = result.counter_offer_ids;
                pending.expires = std::chrono::steady_clock::now() + std::chrono::minutes(5);
                pending_token = counteroffer_token();
                g_pending_counteroffers.emplace(pending_token, std::move(pending));
            }

            std::ostringstream js;
            js << "{\"ok\":true"
               << ",\"committed\":" << (result.committed ? "true" : "false")
               << ",\"counterOffer\":" << (result.counter_offer ? "true" : "false")
               << ",\"talkline\":" << result.talkline
               << ",\"message\":" << json_string(talkline_message(result.talkline))
               << ",\"merchantValue\":" << result.merchant_value
               << ",\"fortressValue\":" << result.fortress_value;
            if (!pending_token.empty())
                js << ",\"token\":" << json_string(pending_token);
            js << ",\"counterItems\":[";
            for (size_t i = 0; i < result.counter_offer_ids.size(); ++i) {
                if (i) js << ",";
                int32_t item_id = result.counter_offer_ids[i];
                auto* item = df::item::find(item_id);
                js << "{\"id\":" << item_id
                   << ",\"desc\":" << json_string(item
                        ? item_display_name(item, 0, true) : std::string("Unavailable item"))
                   << "}";
            }
            js << "]}\n";
            response = js.str();
            return true;
        });
        if (!executed) {
            json_error(res, 409, execute_error.empty() ? "trade could not be applied" : execute_error);
            return;
        }
        notify_player_input();
        set_no_store_json(res, response);
    });
}

} // namespace dfcapture
