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
#include "df/unit.h"
#include "df/world.h"

#include <mutex>
#include <sstream>
#include <stack>
#include <string>
#include <unordered_set>
#include <vector>

using namespace DFHack;

namespace dfcapture {
namespace {

std::recursive_mutex g_depot_mutex;

// Same lock discipline as fort_admin.cpp / squads.cpp: panel mutex -> capture-state mutex ->
// CoreSuspender. Reads and mutations share the guard so iterating caravans / jobs / items never
// races the sim.
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

// building_tradedepotst::accessible (original name: have_access) is caravan-maintained state,
// not the live answer shown by DF's depot-access check. In particular, it can remain false when
// no caravan is present. Mirror DFHack pathable's native-map wagon flood here, but seed it from
// only the requested depot instead of combining every depot in the fort.
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
// a trader is requested and a caravan is present). caravan.lua's `leave` reads/removes exactly
// this job.
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
// Caravans (plotinfo->caravans). Mirrors caravan.lua state/origin/day math.
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
// Tradeable-goods enumeration (mirrors movegoods.lua is_tradeable_item, default filters:
// group off / inside_containers off -> in_inventory items are skipped).
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
        {
            auto world = df::global::world;
            for (df::job_list_link* link = world->jobs.list.next; link; link = link->next) {
                df::job* job = link->item;
                if (job && job->job_type == df::job_type::BringItemToDepot)
                    ++pending;
            }
            // TEMP-role contained items are goods sitting at the depot for trade; PERM-role
            // items are the depot's own construction materials (excluded) -- movegoods parity.
            for (auto* ci : depot->contained_items) {
                if (ci && ci->item && ci->use_mode == df::building_item_role_type::TEMP)
                    ++at_depot;
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
           << ",\"caravans\":[";
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
// Mark / unmark ONE item for trade at this depot. Mirrors movegoods.lua onDismiss exactly:
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

// Toggle the depot's trade flags. request: set/clear trader_requested (the exact bit DF's own
// "Request trader at depot" checkbox writes). When clearing, also remove any live TradeAtDepot
// job on the depot (caravan.lua `leave` parity). anyone: set/clear anyone_can_trade.
bool do_depot_broker(int32_t id, bool has_request, bool request, bool has_anyone, bool anyone,
                     std::string* err) {
    return run_depot_locked([&]() -> bool {
        auto depot = resolve_depot(id);
        if (!depot) { if (err) *err = "not a trade depot"; return false; }
        if (has_request) {
            depot->trade_flags.bits.trader_requested = request;
            if (!request) {
                // Recall: drop the broker's TradeAtDepot job (mirrors caravan.lua leave).
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

    // /depot-trade requires an atomic transaction through DF's native trade state.
    //
    // The barter write-set includes ownership, trader flags, caravan counters, merchant state,
    // entity resources, and history events. It must not be hand-written. Selection, commit, offer,
    // seize, and counter-offer actions remain unavailable until they can be applied atomically
    // through DF's native transaction.
    // The barter transaction is not available until selection writes and the native commit can
    // be performed atomically and safely. Until then both routes return 501 so the frontend can
    // direct players to DF's own trade screen. The rest of the depot panel (goods, marking,
    // broker request, and trade status) remains fully available.
    auto trade_screen_stub = [](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        res.status = 501;
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":false,\"error\":\"barter remains native-only; goods marked here "
                        "reach the depot, but complete the trade in Dwarf Fortress\"}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/depot-trade", trade_screen_stub);
    server.Post("/depot-trade", trade_screen_stub);
}

} // namespace dfcapture
