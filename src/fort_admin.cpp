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

#include "fort_admin.h"

#include "Core.h"
#include "http_server.h"
#include "json_util.h"
#include "lua_bridge.h"
#include "save_barrier.h"
#include "sdl_capture.h"

#include "modules/Items.h"
#include "modules/Materials.h"
#include "modules/Translation.h"
#include "modules/Units.h"

#include "df/agreement.h"
#include "df/agreement_details.h"
#include "df/agreement_details_data_citizenship.h"
#include "df/agreement_details_data_residency.h"
#include "df/agreement_details_type.h"
#include "df/agreement_flag.h"
#include "df/agreement_party.h"
#include "df/building.h"
#include "df/building_civzonest.h"
#include "df/building_type.h"
#include "df/civzone_type.h"
#include "df/crime.h"
#include "df/crime_flag.h"
#include "df/crime_handlerst.h"
#include "df/crime_type.h"
#include "df/entity_position.h"
#include "df/entity_position_assignment.h"
#include "df/global_objects.h"
#include "df/histfig_entity_link_positionst.h"
#include "df/historical_entity.h"
#include "df/historical_figure.h"
#include "df/history_event_reason.h"
#include "df/mandate.h"
#include "df/mandate_handlerst.h"
#include "df/plotinfost.h"
// plotinfo->punishments stores df::punishment pointers.
#include "df/punishment.h"
#include "df/record_precision_level_type.h"
#include "df/squad.h"
#include "df/squad_position.h"
#include "df/unit.h"
#include "df/world.h"
#include "df/world_site.h"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace DFHack;

namespace dfcapture {
namespace {

std::recursive_mutex g_admin_mutex;

// Lock order: panel mutex -> capture-state mutex -> CoreSuspender. Reads and mutations use the
// same guard.
template <typename Fn>
bool run_admin_locked(Fn&& fn) {
    std::lock_guard<std::recursive_mutex> admin_lock(g_admin_mutex);
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

// Resolve a fort unit from a historical-figure id (nobles hold positions as
// histfigs; we want the live unit for a deep link).
df::unit* unit_for_histfig(int32_t hf_id) {
    if (hf_id < 0)
        return nullptr;
    auto world = df::global::world;
    if (!world)
        return nullptr;
    for (auto unit : world->units.active) {
        if (unit && unit->hist_figure_id == hf_id)
            return unit;
    }
    return nullptr;
}

std::string histfig_name(int32_t hf_id) {
    if (hf_id < 0)
        return "";
    auto hf = df::historical_figure::find(hf_id);
    if (!hf)
        return "";
    std::string name = DFHack::Translation::translateName(&hf->name, true);
    return name.empty() ? ("Figure " + std::to_string(hf_id)) : name;
}

// ---------------------------------------------------------------------------
// Nobles / administrators
// ---------------------------------------------------------------------------

std::string position_display_name(df::entity_position* position) {
    if (!position)
        return "";
    if (!position->name[0].empty())
        return position->name[0];
    if (!position->name_male[0].empty())
        return position->name_male[0];
    return "Position " + std::to_string(position->id);
}

std::string position_requirements(df::entity_position* position) {
    std::vector<std::string> reqs;
    if (position->required_office > 0) reqs.push_back("office");
    if (position->required_bedroom > 0) reqs.push_back("bedroom");
    if (position->required_dining > 0) reqs.push_back("dining room");
    if (position->required_tomb > 0) reqs.push_back("tomb");
    std::string out;
    for (size_t i = 0; i < reqs.size(); ++i) {
        if (i) out += ", ";
        out += reqs[i];
    }
    return out;
}

// Squad aliases override generated language names on militia rows.
std::string squad_name_for(int32_t squad_id) {
    if (squad_id < 0)
        return "";
    auto squad = df::squad::find(squad_id);
    if (!squad)
        return "";
    if (!squad->alias.empty())
        return squad->alias;
    return DFHack::Translation::translateName(&squad->name, true);
}

// Room-requirement values above zero mean the position requires that room type.
struct RoomReqs { int32_t office, bedroom, dining, tomb, box; };
RoomReqs position_room_reqs(df::entity_position* p) {
    return { p->required_office, p->required_bedroom, p->required_dining, p->required_tomb,
             p->required_boxes };
}

// Determine which room kinds the holder actually owns.
//
// Noble rooms are civzones, and their civzone type determines the room kind. This reports ownership
// of the required kind; value thresholds are not independently modeled.
struct RoomOwned { bool office = false, bedroom = false, dining = false, tomb = false; };
RoomOwned holder_owned_rooms(df::unit* holder) {
    RoomOwned owned;
    if (!holder)
        return owned;
    for (auto zone : holder->owned_buildings) {
        if (!zone)
            continue;
        switch (zone->type) {
            case df::civzone_type::Office:     owned.office  = true; break;
            case df::civzone_type::Bedroom:    owned.bedroom = true; break;
            case df::civzone_type::DiningHall: owned.dining  = true; break;
            case df::civzone_type::Tomb:       owned.tomb    = true; break;
            default: break;
        }
    }
    return owned;
}

// Create an additional position seat.
//
// Creating a position adds a vacant assignment for an existing entity position while respecting
// its holder limit. Explicitly initialize holder and squad ids to -1.

int32_t next_assignment_id(df::historical_entity* fort) {
    int32_t next_id = 0;
    for (auto a : fort->positions.assignments)
        if (a && a->id >= next_id)
            next_id = a->id + 1;
    return next_id;
}

df::entity_position_assignment* create_assignment(df::historical_entity* fort, int32_t position_id) {
    auto assignment = new df::entity_position_assignment();
    assignment->id = next_assignment_id(fort);
    assignment->position_id = position_id;
    assignment->histfig = -1;    // vacant seat (ctor would leave 0 == histfig id 0)
    assignment->histfig2 = -1;   // no previous holder
    assignment->squad_id = -1;   // leads no squad (ctor would leave 0 == squad id 0)
    fort->positions.assignments.push_back(assignment);
    return assignment;
}

// How many seats this position already has, and how many the raws allow (-1 = unlimited).
int count_assignments(df::historical_entity* fort, int32_t position_id) {
    int n = 0;
    for (auto a : fort->positions.assignments)
        if (a && a->position_id == position_id)
            ++n;
    return n;
}

// DF maintains the exact set of seats the fortress may currently appoint in
// historical_entity.positions.possible_appointable. This is downstream of the raws' population,
// market, replacement, and appointer rules. Do not duplicate those rules here: in particular,
// CAPTAIN_OF_THE_GUARD has both REQUIRES_POPULATION:50 and REQUIRES_MARKET in the vanilla raws.
bool position_is_possible_appointable(df::historical_entity* fort, int32_t position_id) {
    if (!fort)
        return false;
    for (auto assignment : fort->positions.possible_appointable)
        if (assignment && assignment->position_id == position_id)
            return true;
    return false;
}

// Native's squad creator can synthesize another AS_NEEDED squad office (vanilla
// MILITIA_CAPTAIN) after its appointing office is held. Such a not-yet-created assignment is not
// necessarily present in possible_appointable, which remains the correct gate for ordinary noble
// appointments. Keep this exception squad-only and preserve the raw population/market/appointer
// requirements.
bool position_is_as_needed_squad_appointable(df::historical_entity* fort,
                                             df::entity_position* position) {
    if (!fort || !position || position->squad_size <= 0 || position->number >= 0 ||
        position->flags.is_set(df::entity_position_flags::HAS_BEEN_REPLACED) ||
        (position->requires_population > 0 &&
         !position->flags.is_set(df::entity_position_flags::HAS_MET_POP_REQ)) ||
        (position->flags.is_set(df::entity_position_flags::REQUIRES_MARKET) &&
         !position->flags.is_set(df::entity_position_flags::HAS_MET_MARKET_REQ)))
        return false;
    if (position->appointed_by.empty())
        return false;
    for (auto appointer_id : position->appointed_by)
        for (auto assignment : fort->positions.assignments)
            if (assignment && assignment->position_id == appointer_id && assignment->histfig >= 0)
                return true;
    return false;
}

// A held position remains on the native nobles screen even though its occupied seat is no longer
// an appointment offer. A squad-linked seat is likewise already active DF state. Vacant positions
// appear only when DF itself puts them in possible_appointable.
bool position_is_noble_screen_visible(df::historical_entity* fort, int32_t position_id) {
    if (!fort)
        return false;
    df::entity_position* position = nullptr;
    for (auto candidate : fort->positions.own)
        if (candidate && candidate->id == position_id) {
            position = candidate;
            break;
        }
    if (!position || position->flags.is_set(df::entity_position_flags::HAS_BEEN_REPLACED) ||
        (position->requires_population > 0 &&
         !position->flags.is_set(df::entity_position_flags::HAS_MET_POP_REQ)))
        return false;
    for (auto assignment : fort->positions.assignments) {
        if (assignment && assignment->position_id == position_id &&
            (assignment->histfig >= 0 || assignment->squad_id >= 0))
            return true;
    }
    return position_is_possible_appointable(fort, position_id);
}

std::string build_nobles_json(const std::string& player, std::string* err) {
    std::ostringstream body;
    bool ok = run_admin_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        auto world = df::global::world;
        if (!plotinfo || !world) { if (err) *err = "world unavailable"; return false; }
        // Fort positions and assignments live on the group entity, not the civilization entity.
        auto fort = df::historical_entity::find(plotinfo->group_id);
        if (!fort) { if (err) *err = "fort entity unavailable"; return false; }

        std::vector<df::entity_position*> visible_positions;
        for (auto position : fort->positions.own)
            if (position && position_is_noble_screen_visible(fort, position->id))
                visible_positions.push_back(position);
        // Native orders the nobles screen by entity_position.precedence.
        std::stable_sort(visible_positions.begin(), visible_positions.end(),
                         [](df::entity_position* a, df::entity_position* b) {
                             return a->precedence < b->precedence;
                         });

        body << "{\"player\":" << json_string(player) << ",\"positions\":[";
        bool first = true;
        for (auto position : visible_positions) {
            // Find the assignment (holder) for this position.
            int32_t holder_hf = -1;
            int32_t assignment_id = -1;
            int32_t squad_id = -1;
            for (auto asn : fort->positions.assignments) {
                if (!asn || asn->position_id != position->id)
                    continue;
                assignment_id = asn->id;
                if (asn->squad_id >= 0)
                    squad_id = asn->squad_id;
                if (asn->histfig != -1) {
                    holder_hf = asn->histfig;
                    break;
                }
            }
            df::unit* holder = unit_for_histfig(holder_hf);
            std::string holder_name = holder ? DFHack::Units::getReadableName(holder)
                                             : histfig_name(holder_hf);
            RoomReqs reqs = position_room_reqs(position);
            RoomOwned owned = holder_owned_rooms(holder);
            const bool can_create = position_is_possible_appointable(fort, position->id) &&
                (position->number < 0 ||
                 count_assignments(fort, position->id) < position->number);
            if (!first) body << ",";
            first = false;
            body << "{\"name\":" << json_string(position_display_name(position))
                 << ",\"positionId\":" << position->id
                 << ",\"assignmentId\":" << assignment_id
                 << ",\"squadSize\":" << position->squad_size
                 << ",\"squadName\":" << json_string(squad_name_for(squad_id))
                 << ",\"precedence\":" << position->precedence
                 << ",\"requirements\":" << json_string(position_requirements(position))
                  // Per-room requirement levels and holder satisfaction.
                 << ",\"rooms\":{\"office\":" << reqs.office << ",\"bedroom\":" << reqs.bedroom
                 << ",\"dining\":" << reqs.dining << ",\"tomb\":" << reqs.tomb
                 << ",\"box\":" << reqs.box << "}"
                 << ",\"roomsSatisfied\":{\"office\":" << (owned.office ? "true" : "false")
                 << ",\"bedroom\":" << (owned.bedroom ? "true" : "false")
                 << ",\"dining\":" << (owned.dining ? "true" : "false")
                 << ",\"tomb\":" << (owned.tomb ? "true" : "false") << "}"
                 << ",\"filled\":" << (holder_hf != -1 ? "true" : "false")
                 << ",\"holder\":" << json_string(holder_name)
                 << ",\"unitId\":" << (holder ? holder->id : -1)
                 << ",\"profession\":" << json_string(holder ? DFHack::Units::getProfessionName(holder) : "")
                 << ",\"professionColor\":" << (holder ? static_cast<int>(DFHack::Units::getProfessionColor(holder)) : -1)
                 // Number of occupied seats compared with the raws-defined limit.
                 // (entity_position.number, -1 == AS_NEEDED == unlimited). `canCreate` is what the
                 // create-position chooser keys off -- and it is honest: it is the same bound
                 // /position-create enforces, so a chooser row can never 400.
                 << ",\"seats\":" << count_assignments(fort, position->id)
                 << ",\"maxSeats\":" << static_cast<int>(position->number)
                 << ",\"canCreate\":" << (can_create ? "true" : "false")
                 << "}";
        }
        // Bookkeeper precision goal (1-5 selector). plotinfo.nobles
        // .bookkeeper_settings is the record_precision_level_type goal (NONE=-1, nearest_10=0 ..
        // all_accurate=4); the native selector button N maps to enum N-1.
        body << "],\"bookkeeperPrecision\":" << static_cast<int>(plotinfo->nobles.bookkeeper_settings)
             << ",\"mandates\":[";
        first = true;
        for (auto mandate : world->mandates.all) {
            if (!mandate)
                continue;
            df::unit* unit = mandate->unit;
            // Item + material the mandate is about (Make X of material Y / ban on exporting
            // material Y). ItemTypeInfo/MaterialInfo both decode invalid ids gracefully to an
            // empty/"any" label, so no extra nil-guard is needed beyond the >=0 material check.
            DFHack::ItemTypeInfo iti(mandate->item_type, mandate->item_subtype);
            std::string item_label = iti.isValid() ? iti.toString() : "";
            std::string mat_label = mandate->mat_type >= 0
                ? DFHack::MaterialInfo(mandate->mat_type, mandate->mat_index).toString() : "";
            // Countdown: timeout_counter ticks once per 10 frames toward timeout_limit; DF runs
            // 1200 frames/day. A non-positive limit means "no deadline" (ongoing prohibition) ->
            // daysRemaining = -1 so the client renders "Ongoing" rather than a false "0 days".
            int days_remaining = -1;
            if (mandate->timeout_limit > 0) {
                long ticks_left = (long)mandate->timeout_limit - (long)mandate->timeout_counter;
                if (ticks_left < 0) ticks_left = 0;
                long frames_left = ticks_left * 10;
                days_remaining = (int)((frames_left + 1199) / 1200); // ceil
            }
            if (!first) body << ",";
            first = false;
            body << "{\"mode\":" << json_string(DFHack::enum_item_key(mandate->mode))
                 << ",\"item\":" << json_string(item_label)
                 << ",\"material\":" << json_string(mat_label)
                 << ",\"amountTotal\":" << mandate->amount_total
                 << ",\"amountRemaining\":" << mandate->amount_remaining
                 << ",\"timeoutCounter\":" << mandate->timeout_counter
                 << ",\"timeoutLimit\":" << mandate->timeout_limit
                 << ",\"daysRemaining\":" << days_remaining
                 << ",\"punishMultiple\":" << (mandate->punish_multiple ? "true" : "false")
                 << ",\"hammerstrikes\":" << mandate->punishment.hammerstrikes
                 << ",\"prisonTime\":" << mandate->punishment.prison_time
                 << ",\"unitId\":" << (unit ? unit->id : -1)
                 << ",\"by\":" << json_string(unit ? DFHack::Units::getReadableName(unit) : "")
                 << ",\"byProfessionColor\":" << (unit ? static_cast<int>(DFHack::Units::getProfessionColor(unit)) : -1)
                 << "}";
        }
        body << "]}\n";
        return true;
    });
    if (!ok)
        return "";
    return body.str();
}

// ---------------------------------------------------------------------------
// Noble assignment: /noble-assign and /noble-candidates.
// ---------------------------------------------------------------------------
// Noble assignment updates the fort's position-assignment slot and the historical figure's
// reciprocal position link. Squad creation for military positions remains in the squad domain.

df::entity_position* find_position(df::historical_entity* fort, int32_t position_id) {
    for (auto p : fort->positions.own)
        if (p && p->id == position_id)
            return p;
    return nullptr;
}

df::entity_position_assignment* find_assignment(df::historical_entity* fort, int32_t position_id) {
    for (auto a : fort->positions.assignments)
        if (a && a->position_id == position_id)
            return a;
    return nullptr;
}

int32_t assignment_index(df::historical_entity* fort, df::entity_position_assignment* assignment) {
    for (size_t i = 0; i < fort->positions.assignments.size(); ++i)
        if (fort->positions.assignments[i] == assignment)
            return static_cast<int32_t>(i);
    return -1;
}

// Drop the matching reciprocal position link from the historical figure.
void unlink_position_holder(int32_t old_hf_id, int32_t fort_id, int32_t assignment_id) {
    auto hf = df::historical_figure::find(old_hf_id);
    if (!hf)
        return;
    for (size_t i = 0; i < hf->entity_links.size(); ++i) {
        auto link = virtual_cast<df::histfig_entity_link_positionst>(hf->entity_links[i]);
        if (link && link->entity_id == fort_id && link->assignment_id == assignment_id) {
            delete hf->entity_links[i];
            hf->entity_links.erase(hf->entity_links.begin() + i);
            return;
        }
    }
}

// Noble candidates must be active, living, sane adult citizens. This excludes retained corpses,
// ghosts, long-term residents, children, and units unable to perform the office's duties.
bool is_assignable_citizen(df::unit* unit) {
    return unit && DFHack::Units::isCitizen(unit) && DFHack::Units::isActive(unit) &&
           !DFHack::Units::isDead(unit) && !DFHack::Units::isGhost(unit) &&
           !DFHack::Units::isBaby(unit) && !DFHack::Units::isChild(unit);
}

std::string build_noble_candidates_json(int32_t position_id, const std::string& player, std::string* err) {
    std::ostringstream body;
    bool ok = run_admin_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        auto world = df::global::world;
        if (!plotinfo || !world) { if (err) *err = "world unavailable"; return false; }
        auto fort = df::historical_entity::find(plotinfo->group_id);
        if (!fort) { if (err) *err = "fort entity unavailable"; return false; }
        auto position = find_position(fort, position_id);
        if (!position || !position_is_noble_screen_visible(fort, position_id)) {
            if (err) *err = "position is not currently offered by DF";
            return false;
        }
        auto assignment = find_assignment(fort, position_id);
        int32_t current_hf = assignment ? assignment->histfig : -1;

        body << "{\"player\":" << json_string(player)
             << ",\"positionId\":" << position_id
             << ",\"positionName\":" << json_string(position ? position_display_name(position) : "")
             << ",\"candidates\":[";
        bool first = true;
        for (auto unit : world->units.active) {
            if (!is_assignable_citizen(unit))
                continue;
            bool is_current = (unit->hist_figure_id >= 0 && unit->hist_figure_id == current_hf);
            if (!first) body << ",";
            first = false;
            body << "{\"unitId\":" << unit->id
                 << ",\"name\":" << json_string(DFHack::Units::getReadableName(unit))
                 << ",\"profession\":" << json_string(DFHack::Units::getProfessionName(unit))
                 << ",\"professionColor\":" << static_cast<int>(DFHack::Units::getProfessionColor(unit))
                 << ",\"current\":" << (is_current ? "true" : "false")
                 << "}";
        }
        body << "]}\n";
        return true;
    });
    if (!ok)
        return "";
    return body.str();
}

// unit_id < 0 unassigns (clears the holder); otherwise assigns that unit's historical figure to
// the position, creating the assignment slot if this position never had one yet (DF's "NEW"
// state, e.g. Messenger in 18-info-nobles.png).
bool do_noble_assign(int32_t position_id, int32_t unit_id, std::string* err) {
    return run_admin_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) { if (err) *err = "world unavailable"; return false; }
        auto fort = df::historical_entity::find(plotinfo->group_id);
        if (!fort) { if (err) *err = "fort entity unavailable"; return false; }
        auto position = find_position(fort, position_id);
        if (!position) { if (err) *err = "unknown position"; return false; }
        if (!position_is_noble_screen_visible(fort, position_id)) {
            if (err) *err = "position is not currently offered by DF";
            return false;
        }

        // Validate a named unit before creating or changing any position assignment. The unit can
        // disappear between the candidate snapshot and this click; find it again while suspended.
        df::unit* unit = nullptr;
        if (unit_id >= 0) {
            unit = df::unit::find(unit_id);
            if (!unit) { if (err) *err = "unit not found"; return false; }
            if (DFHack::Units::isBaby(unit) || DFHack::Units::isChild(unit)) {
                if (err) *err = "unit is a child";
                return false;
            }
            if (!is_assignable_citizen(unit)) { if (err) *err = "unit is not an assignable living citizen"; return false; }
            if (unit->hist_figure_id < 0) { if (err) *err = "unit has no historical figure"; return false; }
        }

        auto assignment = find_assignment(fort, position_id);
        if (!assignment) {
            // Use the shared constructor so sentinel fields, including squad_id, start at -1.
            assignment = create_assignment(fort, position_id);
        }
        int32_t idx = assignment_index(fort, assignment);

        if (unit_id < 0) {
            if (assignment->histfig != -1)
                unlink_position_holder(assignment->histfig, fort->id, assignment->id);
            assignment->histfig = -1;
            return true;
        }

        if (assignment->histfig != -1 && assignment->histfig != unit->hist_figure_id)
            unlink_position_holder(assignment->histfig, fort->id, assignment->id);
        assignment->histfig = unit->hist_figure_id;

        auto newfig = df::historical_figure::find(unit->hist_figure_id);
        if (newfig) {
            bool already_linked = false;
            for (auto link : newfig->entity_links) {
                auto pos_link = virtual_cast<df::histfig_entity_link_positionst>(link);
                if (pos_link && pos_link->entity_id == fort->id && pos_link->assignment_id == assignment->id) {
                    already_linked = true;
                    break;
                }
            }
            if (!already_linked) {
                auto link = df::allocate<df::histfig_entity_link_positionst>();
                link->entity_id = fort->id;
                link->link_strength = 100;
                link->assignment_id = assignment->id;
                link->assignment_vector_idx = idx;
                link->start_year = df::global::cur_year ? *df::global::cur_year : 0;
                newfig->entity_links.push_back(link);
            }
        }
        return true;
    });
}

// Creates one new vacant seat for `position_id`. Returns the new assignment id via out_id.
bool do_position_create(int32_t position_id, int32_t& out_id, std::string* err) {
    return run_admin_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) { if (err) *err = "world unavailable"; return false; }
        auto fort = df::historical_entity::find(plotinfo->group_id);
        if (!fort) { if (err) *err = "fort entity unavailable"; return false; }
        auto position = find_position(fort, position_id);
        if (!position) { if (err) *err = "unknown position"; return false; }
        if (!position_is_possible_appointable(fort, position_id) &&
            !position_is_as_needed_squad_appointable(fort, position)) {
            if (err) *err = "position is neither currently offered by DF nor eligible as an "
                            "AS_NEEDED squad office";
            return false;
        }
        const int held = count_assignments(fort, position_id);
        if (position->number >= 0 && held >= position->number) {
            if (err) *err = "this position allows no more holders (the raws cap it at " +
                            std::to_string(position->number) + ")";
            return false;
        }
        auto assignment = create_assignment(fort, position_id);
        out_id = assignment->id;
        return true;
    });
}

// Set the bookkeeper's precision goal. level is the enum value 0..4
// (nearest_10 .. all_accurate); the client sends button_index-1. Rejects out-of-range so a
// bad request never writes a garbage enum.
bool do_noble_precision(int level, std::string* err) {
    if (level < 0 || level > 4) { if (err) *err = "level out of range (0-4)"; return false; }
    return run_admin_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) { if (err) *err = "world unavailable"; return false; }
        plotinfo->nobles.bookkeeper_settings = static_cast<df::record_precision_level_type>(level);
        return true;
    });
}

// ---------------------------------------------------------------------------
// Justice (read-only)
// ---------------------------------------------------------------------------

std::string unit_name_or_blank(int32_t unit_id) {
    if (unit_id < 0)
        return "";
    auto unit = df::unit::find(unit_id);
    return unit ? DFHack::Units::getReadableName(unit) : "";
}

// True classification bits DF's own crime record exposes (sentenced/discovered/needs_trial --
// see df::crime_flag, only 3 bits). DF's own Open/Closed/Cold split is UI-side categorization
// over those bits (not a stored 4th flag) -- this is the same best-effort derivation: Cold =
// never discovered (no witnesses/evidence surfaced yet), Closed = sentenced (a verdict already
// landed), Open = discovered and not yet sentenced (whether or not it needs_trial).
std::string crime_case_state(df::crime* crime) {
    if (!crime) return "";
    if (!crime->flags.bits.discovered) return "cold";
    if (crime->flags.bits.sentenced) return "closed";
    return "open";
}

void append_crime_json(std::ostringstream& body, df::crime* crime) {
    df::unit* accused = df::unit::find(crime->accused);
    df::unit* criminal = df::unit::find(crime->criminal);
    df::unit* victim = df::unit::find(crime->victim);
    body << "{\"id\":" << crime->id
         << ",\"mode\":" << json_string(DFHack::enum_item_key(crime->mode))
         << ",\"sentenced\":" << (crime->flags.bits.sentenced ? "true" : "false")
         << ",\"discovered\":" << (crime->flags.bits.discovered ? "true" : "false")
         << ",\"needsTrial\":" << (crime->flags.bits.needs_trial ? "true" : "false")
         << ",\"year\":" << crime->event_year
         << ",\"prisonTime\":" << crime->punishment.prison_time
         << ",\"hammerstrikes\":" << crime->punishment.hammerstrikes
         << ",\"witnessCount\":" << static_cast<int>(crime->witnesses.size())
         << ",\"accusedId\":" << (accused ? accused->id : -1)
         << ",\"accused\":" << json_string(accused ? DFHack::Units::getReadableName(accused)
                                                   : unit_name_or_blank(crime->accused))
         << ",\"accusedProfessionColor\":" << (accused ? static_cast<int>(DFHack::Units::getProfessionColor(accused)) : -1)
         << ",\"criminalId\":" << (criminal ? criminal->id : -1)
         << ",\"criminal\":" << json_string(criminal ? DFHack::Units::getReadableName(criminal) : "")
         << ",\"criminalProfessionColor\":" << (criminal ? static_cast<int>(DFHack::Units::getProfessionColor(criminal)) : -1)
         << ",\"victimId\":" << (victim ? victim->id : -1)
         << ",\"victim\":" << json_string(victim ? DFHack::Units::getReadableName(victim) : "")
         << ",\"victimProfessionColor\":" << (victim ? static_cast<int>(DFHack::Units::getProfessionColor(victim)) : -1)
         << "}";
}

// Fortress guard sub-tab: DF's justice screen's guard roster is the squad attached to the
// Captain of the Guard (or Sheriff, before a Captain exists) position -- the same squad-bearing
// position rows /nobles already exposes (squadSize>0). Reads that position's assignment ->
// squad_id -> squad_position.occupant list; empty + unsupported when no guard squad has been
// formed yet (honest empty state, no fabricated roster).
void append_guard_json(std::ostringstream& body, df::historical_entity* fort) {
    int32_t squad_id = -1;
    if (fort) {
        for (auto position : fort->positions.own) {
            if (!position) continue;
            if (position->code != "CAPTAIN_OF_THE_GUARD" && position->code != "SHERIFF")
                continue;
            auto assignment = find_assignment(fort, position->id);
            if (assignment && assignment->squad_id >= 0) {
                squad_id = assignment->squad_id;
                if (position->code == "CAPTAIN_OF_THE_GUARD")
                    break; // prefer the Captain's squad over the Sheriff's if both exist
            }
        }
    }
    auto squad = squad_id >= 0 ? df::squad::find(squad_id) : nullptr;
    // "Desired metal cages and chains in dungeons: N of M" is a derived UI metric
    // (available dungeon-zone restraints vs prisoners) with no clean backing field
    // (only total_death_cage_number / cage_spring_* -- unrelated). Ship null so the web renders
    // nothing rather than fabricating a value.
    body << "\"guard\":{\"squadId\":" << (squad ? squad->id : -1)
         << ",\"desiredCagesChains\":null"
         << ",\"unsupported\":" << (squad ? "false" : "true") << ",\"members\":[";
    bool first = true;
    if (squad) {
        for (auto pos : squad->positions) {
            if (!pos || pos->occupant < 0) continue;
            auto hf = df::historical_figure::find(pos->occupant);
            df::unit* unit = nullptr;
            if (hf) {
                for (auto u : df::global::world->units.active)
                    if (u && u->hist_figure_id == hf->id) { unit = u; break; }
            }
            if (!first) body << ","; first = false;
            body << "{\"unitId\":" << (unit ? unit->id : -1)
                 << ",\"name\":" << json_string(unit ? DFHack::Units::getReadableName(unit) : histfig_name(pos->occupant))
                 << ",\"profession\":" << json_string(unit ? DFHack::Units::getProfessionName(unit) : "")
                 << ",\"professionColor\":" << (unit ? static_cast<int>(DFHack::Units::getProfessionColor(unit)) : -1)
                 << ",\"portraitTexpos\":" << (unit ? unit->portrait_texpos : -1)
                 << "}";
        }
    }
    body << "]}";
}

// Convicts sub-tab: units carrying an active sentence (crime.flags.bits.sentenced, keyed off
// the accused/criminal unit -- real DF data, one row per sentenced crime).
void append_convicts_json(std::ostringstream& body, df::world* world) {
    body << "\"convicts\":[";
    bool first = true;
    int count = 0;
    for (auto crime : world->crimes.all) {
        if (!crime || !crime->flags.bits.sentenced)
            continue;
        int32_t unit_id = crime->accused >= 0 ? crime->accused : crime->criminal;
        df::unit* unit = df::unit::find(unit_id);
        // Injured-party join for the convict detail pane. Same crime->victim
        // read as append_crime_json; web omits the line when victimId<0.
        df::unit* victim = df::unit::find(crime->victim);
        if (!first) body << ","; first = false;
        // Native's convict row is a unit row: portrait tile, then a
        // semantically-coloured `name, profession` second line. All fields are plain reads from the
        // unit already resolved above.
        //   * `portraitTexpos` -- df::unit::portrait_texpos, used by the existing portrait
        //     chain unchanged.
        //   * `profession` / `professionColor` -- DFHack Units::getProfessionName /
        //     the live profession name and color.
        body << "{\"crimeId\":" << crime->id
             << ",\"unitId\":" << (unit ? unit->id : -1)
             << ",\"name\":" << json_string(unit ? DFHack::Units::getReadableName(unit) : unit_name_or_blank(unit_id))
             << ",\"profession\":" << json_string(unit ? DFHack::Units::getProfessionName(unit) : "")
             << ",\"professionColor\":" << (unit ? static_cast<int>(DFHack::Units::getProfessionColor(unit)) : -1)
             << ",\"portraitTexpos\":" << (unit ? unit->portrait_texpos : -1)
             << ",\"mode\":" << json_string(DFHack::enum_item_key(crime->mode))
             << ",\"prisonTime\":" << crime->punishment.prison_time
             << ",\"hammerstrikes\":" << crime->punishment.hammerstrikes
             << ",\"victimId\":" << (victim ? victim->id : -1)
             << ",\"victim\":" << json_string(victim ? DFHack::Units::getReadableName(victim) : "")
             << ",\"victimProfessionColor\":" << (victim ? static_cast<int>(DFHack::Units::getProfessionColor(victim)) : -1)
             << "}";
        if (++count >= 200) break;
    }
    body << "]";
}

std::string build_justice_json(const std::string& player, const std::string& mode, std::string* err) {
    std::ostringstream body;
    bool ok = run_admin_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        auto world = df::global::world;
        if (!world) { if (err) *err = "world unavailable"; return false; }

        body << "{\"player\":" << json_string(player)
             << ",\"justiceActive\":" << (plotinfo && plotinfo->justice_active ? "true" : "false");

        if (mode.empty()) {
            // An omitted mode returns the complete crime list.
            body << ",\"crimes\":[";
            bool first = true;
            int count = 0;
            for (auto crime : world->crimes.all) {
                if (!crime) continue;
                if (!first) body << ","; first = false;
                append_crime_json(body, crime);
                if (++count >= 200) break;
            }
            body << "]}\n";
            return true;
        }

        body << ",\"mode\":" << json_string(mode);
        if (mode == "open" || mode == "closed" || mode == "cold") {
            body << ",\"crimes\":[";
            bool first = true;
            int count = 0;
            for (auto crime : world->crimes.all) {
                if (!crime || crime_case_state(crime) != mode)
                    continue;
                if (!first) body << ","; first = false;
                append_crime_json(body, crime);
                if (++count >= 200) break;
            }
            body << "]}\n";
        } else if (mode == "guard") {
            auto fort = plotinfo ? df::historical_entity::find(plotinfo->group_id) : nullptr;
            body << ",";
            append_guard_json(body, fort);
            body << "}\n";
        } else if (mode == "convicts") {
            body << ",";
            append_convicts_json(body, world);
            body << "}\n";
        } else if (mode == "counterintel") {
            // Interrogation/scheme reports aren't trivially readable through dfhack's exposed
            // structures (they're derived from history-event report strings, not a plain list)
            // Return an honest empty state instead of fabricating a roster.
            body << ",\"counterintel\":[],\"unsupported\":true}\n";
        } else {
            if (err) *err = "unknown justice mode";
            return false;
        }
        return true;
    });
    if (!ok)
        return "";
    return body.str();
}

// ---------------------------------------------------------------------------
// Petitions / agreements
// ---------------------------------------------------------------------------

std::string agreement_detail_summary(df::agreement* agreement) {
    if (!agreement || agreement->details.empty())
        return "Agreement";
    std::vector<std::string> parts;
    for (auto detail : agreement->details) {
        if (!detail)
            continue;
        parts.push_back(DFHack::enum_item_key(detail->type));
        if (parts.size() >= 3)
            break;
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += ", ";
        out += parts[i];
    }
    return out.empty() ? "Agreement" : out;
}

std::string agreement_petitioner(df::agreement* agreement) {
    if (!agreement)
        return "";
    for (auto party : agreement->parties) {
        if (party && !party->histfig_ids.empty())
            return histfig_name(party->histfig_ids[0]);
    }
    return "";
}

struct PetitionDetail {
    std::string site;
    std::string purpose;
    uint8_t* policy = nullptr;
};

std::string enum_words(const std::string& key) {
    std::string out = key;
    std::replace(out.begin(), out.end(), '_', ' ');
    return out;
}

PetitionDetail petition_detail(df::agreement* agreement) {
    PetitionDetail out;
    if (!agreement)
        return out;
    for (auto detail : agreement->details) {
        if (!detail)
            continue;
        int32_t site_id = -1;
        if (detail->type == df::agreement_details_type::Citizenship && detail->data.Citizenship) {
            site_id = detail->data.Citizenship->site;
            out.purpose = enum_words(DFHack::enum_item_key(detail->type));
            out.policy = df::global::standing_orders_petition_citizenship;
        } else if (detail->type == df::agreement_details_type::Residency && detail->data.Residency) {
            auto reason = detail->data.Residency->reason;
            site_id = detail->data.Residency->site;
            out.purpose = enum_words(DFHack::enum_item_key(reason));
            switch (reason) {
            case df::history_event_reason::entertain_people:
            case df::history_event_reason::hire_on_as_performer:
                out.policy = df::global::standing_orders_petition_resident_performer;
                break;
            case df::history_event_reason::eradicate_beasts:
                out.policy = df::global::standing_orders_petition_resident_monster_hunter;
                break;
            case df::history_event_reason::make_a_living_as_a_warrior:
            case df::history_event_reason::hire_on_as_mercenary:
                out.policy = df::global::standing_orders_petition_resident_mercenary;
                break;
            case df::history_event_reason::study:
            case df::history_event_reason::scholarship:
            case df::history_event_reason::hire_on_as_scholar:
                out.policy = df::global::standing_orders_petition_resident_scholar;
                break;
            case df::history_event_reason::seek_sanctuary:
                out.policy = df::global::standing_orders_petition_resident_sanctuary;
                break;
            default:
                break;
            }
        } else {
            continue;
        }
        if (auto site = df::world_site::find(site_id))
            out.site = DFHack::Translation::translateName(&site->name, true);
        break;
    }
    return out;
}

const char* petition_policy_name(uint8_t value) {
    static const char* names[] = {"prompt", "accept", "reject"};
    return value < 3 ? names[value] : "";
}

std::string build_petitions_json(const std::string& player, std::string* err) {
    std::ostringstream body;
    bool ok = run_admin_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) { if (err) *err = "world unavailable"; return false; }

        // plotinfo->petitions contains only
        // unapproved_agreement_id. Native keeps accepted/READY fort obligations in the sibling
        // continuing_agreement_id vector. Union those two fort-owned lists (not world.agreements,
        // which also contains unrelated intrigue/parley agreements) and retain invalid ids as
        // valid:false diagnostics. The vectors contain ids, so this adds no pointer walk.
        struct FortAgreementRef {
            int32_t id;
            bool pending_list;
            bool continuing_list;
        };
        std::vector<FortAgreementRef> agreement_refs;
        auto append_ids = [&](const std::vector<int32_t>& ids, bool pending_list) {
            for (int32_t id : ids) {
                auto found = std::find_if(agreement_refs.begin(), agreement_refs.end(),
                    [id](const FortAgreementRef& ref) { return ref.id == id; });
                if (found == agreement_refs.end()) {
                    agreement_refs.push_back({id, pending_list, !pending_list});
                } else if (pending_list) {
                    found->pending_list = true;
                } else {
                    found->continuing_list = true;
                }
            }
        };
        append_ids(plotinfo->petitions, true);
        append_ids(plotinfo->continuing_agreement_id, false);

        body << "{\"player\":" << json_string(player)
             << ",\"agreementCoverage\":\"pending+continuing\",\"petitions\":[";
        bool first = true;
        for (const auto& ref : agreement_refs) {
            auto agreement = df::agreement::find(ref.id);
            if (!first) body << ",";
            first = false;
            bool pending = agreement && agreement->flags.bits.petition_not_accepted;
            PetitionDetail detail = petition_detail(agreement);
            body << "{\"id\":" << ref.id
                 << ",\"summary\":" << json_string(agreement_detail_summary(agreement))
                 << ",\"petitioner\":" << json_string(agreement_petitioner(agreement))
                 << ",\"site\":" << json_string(detail.site)
                 << ",\"purpose\":" << json_string(detail.purpose)
                 << ",\"futurePolicy\":" << json_string(detail.policy ? petition_policy_name(*detail.policy) : "")
                 << ",\"inPendingList\":" << (ref.pending_list ? "true" : "false")
                 << ",\"inContinuingList\":" << (ref.continuing_list ? "true" : "false")
                 << ",\"pending\":" << (pending ? "true" : "false")
                 << ",\"valid\":" << (agreement ? "true" : "false")
                 << "}";
        }
        body << "]}\n";
        return true;
    });
    if (!ok)
        return "";
    return body.str();
}

bool do_petition_policy(int32_t agreement_id, int value, std::string* err) {
    return run_admin_locked([&]() -> bool {
        if (value < 0 || value > 2) { if (err) *err = "petition value must be 0, 1 or 2"; return false; }
        PetitionDetail detail = petition_detail(df::agreement::find(agreement_id));
        if (!detail.policy) { if (err) *err = "petition policy unavailable"; return false; }
        *detail.policy = static_cast<uint8_t>(value);
        return true;
    });
}

// Per-petition accept and deny are native-only and fail closed. Removing the petition record or
// toggling agreement flags does not reproduce DF's residency, citizenship, location-obligation,
// and decision bookkeeping. The route validates the target and returns 501 without mutating it.
// /petition-policy remains available for native automatic handling of future petitions.
constexpr const char* kPetitionNativeOnlyReason =
    "Approving or denying a petition is a native-only action. The plugin cannot grant the "
    "petitioner residency (on accept) or record the decision the way DF does, so a plugin write "
    "would only hide the row without actually resolving it. Decide it on the host, in the Steam "
    "client (the petition notification / Agreements screen). To auto-handle future petitions of "
    "this kind from the browser, set the standing-orders response below.";

// Validate the id before returning the native-only refusal.
bool validate_pending_petition(int32_t agreement_id, std::string* err) {
    return run_admin_locked([&]() -> bool {
        auto agreement = df::agreement::find(agreement_id);
        if (!agreement) { if (err) *err = "agreement not found"; return false; }
        if (!agreement->flags.bits.petition_not_accepted) {
            if (err) *err = "not a pending petition";
            return false;
        }
        return true;
    });
}

// ---------------------------------------------------------------------------
// Justice write actions.
// ---------------------------------------------------------------------------
// Pardon commutes active punishment counters without altering the crime record or history.
// Returns the number of punishment rows commuted, or -1 on a hard failure.
int do_justice_pardon(int32_t unit_id, std::string* err) {
    int commuted = 0;
    bool ok = run_admin_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) { if (err) *err = "world unavailable"; return false; }
        for (auto punishment : plotinfo->punishments) {
            if (!punishment || punishment->criminal != unit_id)
                continue;
            punishment->prison_counter = 0;
            punishment->beating = 0;
            punishment->hammer_strikes = 0;
            ++commuted;
        }
        return true;
    });
    if (!ok)
        return -1;
    if (commuted == 0 && err)
        *err = "unit is not currently serving a sentence";
    return commuted;
}

// Recenter hotkey locations (plotinfo->main.hotkeys[16]).
// DF's F1-F8-style saved map locations. Each df::ui_hotkey has {name, cmd, x, y, z}; a slot is a
// live LOCATION when cmd == Zoom and x >= 0 (DF uses -30000 as the empty sentinel). We expose the
// 16 slots read-only, and set/clear/rename them. "Set to current camera" takes the x/y/z from the
// client (its live viewport centre) rather than reading a per-player camera here, so no camera
// coupling. Global fort state (one shared list, exactly like the native game).
constexpr int kHotkeyEmpty = -30000;

std::string build_hotkeys_json() {
    std::ostringstream js;
    js << "{\"hotkeys\":[";
    bool ok = run_admin_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) return false;
        for (int i = 0; i < 16; ++i) {
            df::ui_hotkey& hk = plotinfo->main.hotkeys[i];
            bool set = (hk.cmd == df::hotkey_type::Zoom) && hk.x >= 0;
            if (i) js << ",";
            js << "{\"slot\":" << i
               << ",\"name\":" << json_string(hk.name)
               << ",\"cmd\":" << (int)hk.cmd
               << ",\"set\":" << (set ? "true" : "false")
               << ",\"x\":" << hk.x << ",\"y\":" << hk.y << ",\"z\":" << hk.z << "}";
        }
        return true;
    });
    if (!ok) return std::string();
    js << "]}\n";
    return js.str();
}

bool do_hotkey_action(int slot, const std::string& action, bool has_xyz,
                      int x, int y, int z, const std::string& name, std::string* err) {
    if (slot < 0 || slot >= 16) { if (err) *err = "slot out of range (0-15)"; return false; }
    return run_admin_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) { if (err) *err = "plotinfo unavailable"; return false; }
        df::ui_hotkey& hk = plotinfo->main.hotkeys[slot];
        if (action == "set") {
            if (!has_xyz) { if (err) *err = "set requires x/y/z"; return false; }
            hk.cmd = df::hotkey_type::Zoom;
            hk.x = x; hk.y = y; hk.z = z;
            std::string nm = name;
            if (nm.size() > 128) nm.resize(128);
            if (nm.empty() && hk.name.empty())
                nm = "Location " + std::to_string(slot + 1);
            if (!nm.empty()) hk.name = nm;
            return true;
        }
        if (action == "clear") {
            hk.cmd = df::hotkey_type::None;
            hk.name.clear();
            hk.x = hk.y = hk.z = kHotkeyEmpty;
            return true;
        }
        if (action == "rename") {
            std::string nm = name;
            if (nm.size() > 128) nm.resize(128);
            hk.name = nm;
            return true;
        }
        if (err) *err = "unknown action: " + action;
        return false;
    });
}

} // namespace

void register_fort_admin_routes(httplib::Server& server) {
    // GET /nobles -> positions, holders (unit deep links), requirements, mandates.
    server.Get("/nobles", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        std::string err;
        std::string json = build_nobles_json(player, &err);
        if (json.empty()) { json_error(res, 503, err.empty() ? "nobles unavailable" : err); return; }
        set_no_store_json(res, json);
    });

    // GET /noble-candidates?position= -> eligible fort citizens for that position (best-effort:
    // all resident citizens, NOT DF's own suitability-scored appointment_candidatest list --
    // no dfhack module exposes that computation, see the recipe note above build_noble_candidates_json).
    server.Get("/noble-candidates", [](const httplib::Request& req, httplib::Response& res) {
        int position = -1;
        if (!query_int(req, "position", position)) { json_error(res, 400, "missing position"); return; }
        std::string player = query_player(req);
        std::string err;
        std::string json = build_noble_candidates_json(position, player, &err);
        if (json.empty()) { json_error(res, 503, err.empty() ? "candidates unavailable" : err); return; }
        set_no_store_json(res, json);
    });

    // POST /noble-assign?position=&unit= (unit=-1 unassigns) -> set/clear the position holder.
    auto noble_assign_handler = [](const httplib::Request& req, httplib::Response& res) {
        int position = -1;
        if (!query_int(req, "position", position)) { json_error(res, 400, "missing position"); return; }
        int unit = -1;
        query_int(req, "unit", unit);
        std::string err;
        if (!do_noble_assign(position, unit, &err)) { json_error(res, 400, err); return; }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/noble-assign", noble_assign_handler);
    server.Post("/noble-assign", noble_assign_handler);

    // POST /position-create?position=<entity_position id> creates one vacant seat
    // (df::entity_position_assignment) for a position the raws allow more of
    // (entity_position.number, -1 = AS_NEEDED). This is what native's create-squad chooser does
    // when it offers "a new militia captain": DF makes the captain seat, then the squad under it.
    // Returns the new assignment id so the caller can hand it straight to /squad-create?position=.
    auto position_create_handler = [](const httplib::Request& req, httplib::Response& res) {
        int position = -1;
        if (!query_int(req, "position", position)) { json_error(res, 400, "missing position"); return; }
        int32_t assignment_id = -1;
        std::string err;
        if (!do_position_create(position, assignment_id, &err)) { json_error(res, 400, err); return; }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true,\"assignmentId\":" + std::to_string(assignment_id) + "}\n");
    };
    server.Get("/position-create", position_create_handler);
    server.Post("/position-create", position_create_handler);

    // POST /noble-precision?level=0..4 -> set the bookkeeper's record-precision goal (the 1-5
    // selector on the Bookkeeper row; button N sends level=N-1).
    auto noble_precision_handler = [](const httplib::Request& req, httplib::Response& res) {
        int level = -1;
        if (!query_int(req, "level", level)) { json_error(res, 400, "missing level"); return; }
        std::string err;
        if (!do_noble_precision(level, &err)) { json_error(res, 400, err); return; }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/noble-precision", noble_precision_handler);
    server.Post("/noble-precision", noble_precision_handler);

    // GET /justice[?mode=] -> open/closed crimes, convictions, witness counts.
    // mode selects one of DF's six Justice sub-tabs; omitting it returns the complete crime list.
    server.Get("/justice", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "";
        std::string err;
        std::string json = build_justice_json(player, mode, &err);
        if (json.empty()) { json_error(res, 503, err.empty() ? "justice unavailable" : err); return; }
        set_no_store_json(res, json);
    });

    // POST /justice-pardon?unit= commutes that unit's active sentence. Returns 400 when the unit
    // is not serving one and 503 when the world is unavailable.
    auto pardon_handler = [](const httplib::Request& req, httplib::Response& res) {
        int unit = -1;
        if (!query_int(req, "unit", unit) || unit < 0) { json_error(res, 400, "missing unit"); return; }
        std::string err;
        int commuted = do_justice_pardon(unit, &err);
        if (commuted < 0) { json_error(res, 503, err.empty() ? "pardon unavailable" : err); return; }
        if (commuted == 0) { json_error(res, 400, err); return; }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true,\"commuted\":" + std::to_string(commuted) + "}\n");
    };
    server.Get("/justice-pardon", pardon_handler);
    server.Post("/justice-pardon", pardon_handler);

    // /justice-convict and /justice-interrogate require DF's native justice transaction.
    //
    // Native conviction creates punishment and history records; interrogation changes the case's
    // native queue. These operations are unavailable until they can be committed atomically without
    // exposing partially mutated justice state. Case reads and pardons remain available.
    auto justice_drive_stub = [](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        res.status = 501;
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":false,\"error\":\"convict/interrogate actions remain native-only; "
                        "use Dwarf Fortress for those actions; pardons work from here\"}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/justice-convict", justice_drive_stub);
    server.Post("/justice-convict", justice_drive_stub);
    server.Get("/justice-interrogate", justice_drive_stub);
    server.Post("/justice-interrogate", justice_drive_stub);

    // GET /petitions -> pending + accepted agreements.
    server.Get("/petitions", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        std::string err;
        std::string json = build_petitions_json(player, &err);
        if (json.empty()) { json_error(res, 503, err.empty() ? "petitions unavailable" : err); return; }
        set_no_store_json(res, json);
    });

    // POST /petition-accept?id= and /petition-deny?id= validate the target, then fail closed because
    // native petition decisions have additional side effects that cannot be reproduced safely.
    auto native_only_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) { json_error(res, 400, "missing id"); return; }
        std::string err;
        if (!validate_pending_petition(id, &err)) { json_error(res, 400, err); return; }
        res.status = 501;
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":false,\"blocked\":\"native-only\",\"error\":" +
                            json_string(kPetitionNativeOnlyReason) + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/petition-accept", native_only_handler);
    server.Post("/petition-accept", native_only_handler);
    server.Get("/petition-deny", native_only_handler);
    server.Post("/petition-deny", native_only_handler);

    // POST /petition-policy?id=&value=0|1|2 -> prompt/accept/reject for this petition category.
    server.Post("/petition-policy", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1, value = -1;
        if (!query_int(req, "id", id)) { json_error(res, 400, "missing id"); return; }
        if (!query_int(req, "value", value)) { json_error(res, 400, "missing value"); return; }
        std::string err;
        if (!do_petition_policy(id, value, &err)) { json_error(res, 400, err); return; }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true}\n");
    });

    // Recenter hotkey locations. GET lists the 16 slots; POST
    // /hotkey-action?slot=N&action=set|clear|rename[&x=&y=&z=&name=] mutates one slot. "Set" takes
    // the client's current viewport centre as x/y/z; the client recenters to a slot via the
    // existing /camera route (no server "zoom-to" needed).
    server.Get("/hotkeys", [](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        std::string json = build_hotkeys_json();
        if (json.empty()) { json_error(res, 503, "hotkeys unavailable"); return; }
        set_no_store_json(res, json);
    });
    auto hotkey_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int slot = -1;
        if (!query_int(req, "slot", slot)) { json_error(res, 400, "missing slot"); return; }
        std::string action = req.has_param("action") ? req.get_param_value("action") : "";
        int x = 0, y = 0, z = 0;
        bool has_xyz = query_int(req, "x", x) & query_int(req, "y", y) & query_int(req, "z", z);
        std::string name = req.has_param("name") ? req.get_param_value("name") : "";
        std::string err;
        if (!do_hotkey_action(slot, action, has_xyz, x, y, z, name, &err)) {
            json_error(res, 400, err.empty() ? "hotkey action failed" : err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/hotkey-action", hotkey_action_handler);
    server.Post("/hotkey-action", hotkey_action_handler);
}

} // namespace dfcapture
