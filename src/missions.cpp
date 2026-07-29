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

#include "missions.h"

#include "Core.h"
#include "diagnostics.h"
#include "json_util.h"
#include "lua_bridge.h"
#include "sdl_capture.h"

#include "modules/Military.h"
#include "modules/Translation.h"

#include "df/global_objects.h"
#include "df/army.h"
#include "df/army_controller.h"
#include "df/army_controller_goal_type.h"
#include "df/army_controller_goal_make_requestst.h"
#include "df/army_controller_goal_recover_artifactst.h"
#include "df/army_controller_goal_rescue_hfst.h"
#include "df/army_controller_goal_site_invasionst.h"
#include "df/army_flags.h"
#include "df/artifact_record.h"
#include "df/historical_entity.h"
#include "df/historical_figure.h"
#include "df/historical_figure_info.h"
#include "df/mission_report.h"
#include "df/plotinfost.h"
#include "df/squad.h"
#include "df/squad_position.h"
#include "df/state_profilest.h"
#include "df/world.h"
#include "df/world_data.h"
#include "df/world_site.h"
#include "df/world_site_type.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace DFHack;

namespace dfcapture {
namespace {

// Mission creation validates requests but remains native-only. There is no runtime path that
// enables a partial mission write.
constexpr bool kMissionCommitEnabled = false;

std::recursive_mutex g_missions_mutex;

template <typename Fn>
bool run_missions_locked(Fn&& fn) {
    std::lock_guard<std::recursive_mutex> missions_lock(g_missions_mutex);
    std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
    DFHack::CoreSuspender suspend;
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

// The refusal body every blocked create returns. One definition keeps the client copy and
// /missions capability advertisement consistent.
const char* kNativeOnlyReason =
    "Dwarf Fortress creates missions only inside its own world screen (viewscreen_worldst): the "
    "per-goal eligibility verdicts live in that screen's new_mission[] array and nowhere in world "
    "state, and the confirm itself allocates an army_controller + an army + one army_nemesis "
    "record per dwarf and hands them to DF's dwarf-mode departure. DFHack exposes no API for any "
    "of that, so writing it would mean guessing DF's own allocator -- and a wrong guess strands "
    "the squad or corrupts the save rather than failing loudly. The order below is fully "
    "validated and staged; only the commit is withheld.";

std::string site_name_of(df::world_site* site) {
    return site ? DFHack::Translation::translateName(&site->name, true) : std::string();
}

df::world_site* find_site(int32_t site_id) {
    auto world = df::global::world;
    if (!world || !world->world_data || site_id < 0) return nullptr;
    for (auto* site : world->world_data->sites)
        if (site && site->id == site_id) return site;
    return nullptr;
}

std::string entity_name_of(df::historical_entity* entity) {
    return entity ? DFHack::Translation::translateName(&entity->name, true) : std::string();
}

std::string hf_name_of(int32_t hfid) {
    auto* hf = df::historical_figure::find(hfid);
    return hf ? DFHack::Translation::translateName(&hf->name, true) : std::string();
}

std::string artifact_name_of(int32_t artifact_id) {
    auto* rec = df::artifact_record::find(artifact_id);
    return rec ? DFHack::Translation::translateName(&rec->name, true) : std::string();
}

int squad_member_count(df::squad* squad) {
    if (!squad) return 0;
    int n = 0;
    for (auto* pos : squad->positions)
        if (pos && pos->occupant != -1) ++n;
    return n;
}

// Resolve a squad's army through every living member because the first position may be vacant.
df::army* squad_army(df::squad* squad) {
    if (!squad) return nullptr;
    for (auto* pos : squad->positions) {
        if (!pos || pos->occupant == -1) continue;
        auto* hf = df::historical_figure::find(pos->occupant);
        if (!hf || !hf->info || !hf->info->whereabouts) continue;
        auto* army = df::army::find(hf->info->whereabouts->army_id);
        if (army) return army;
    }
    return nullptr;
}

// A nonzero controller id with no controller identifies a stranded army.
bool army_is_stuck(df::army* army) {
    return army && army->controller_id != 0 && !army->controller;
}

// Camping armies can hang from sub-controllers; the top controller points its master_id at itself.
df::army_controller* top_controller(df::army_controller* controller) {
    if (!controller) return nullptr;
    if (controller->master_id == controller->id) return controller;
    return df::army_controller::find(controller->master_id);
}

// Only these controller goals expose a usable homeward flag.
void army_valid_returning(df::army* army, bool& valid, bool& returning) {
    valid = false;
    returning = false;
    auto* c = top_controller(army ? army->controller : nullptr);
    if (!c) return;
    if (c->goal == df::army_controller_goal_type::SITE_INVASION && c->data.goal_site_invasion) {
        valid = true;
        returning = c->data.goal_site_invasion->flag.bits.RETURNING_HOME != 0;
    } else if (c->goal == df::army_controller_goal_type::MAKE_REQUEST && c->data.goal_make_request) {
        valid = true;
        returning = c->data.goal_make_request->flag.bits.RETURNING_HOME != 0;
    }
}

// Read the union member that `goal` selects -- and ONLY that one. df::army_controller::data is a
// true union, so reading goal_recover_artifact on a SITE_INVASION
// controller is reading an unrelated struct through a live pointer. `returning` is emitted as a
// tri-state (-1 = this goal has no homeward flag) rather than defaulting to false, so the client
// never claims "outbound" about a goal that does not track it.
struct GoalDetail {
    std::string target_kind;   // "" | "artifact" | "hf" | "invasion"
    int32_t target_id = -1;
    std::string target_name;
    std::string invasion_intent;
    int returning = -1;        // -1 unknown / not tracked, 0 outbound, 1 returning home
};

GoalDetail goal_detail(df::army_controller* c) {
    GoalDetail d;
    if (!c) return d;
    switch (c->goal) {
    case df::army_controller_goal_type::RECOVER_ARTIFACT:
        if (c->data.goal_recover_artifact) {
            d.target_kind = "artifact";
            d.target_id = c->data.goal_recover_artifact->artifact_id;
            d.target_name = artifact_name_of(d.target_id);
            d.returning = c->data.goal_recover_artifact->flag.bits.RETURNING ? 1 : 0;
        }
        break;
    case df::army_controller_goal_type::RESCUE_HF:
        if (c->data.goal_rescue_hf) {
            d.target_kind = "hf";
            d.target_id = c->data.goal_rescue_hf->hfid;
            d.target_name = hf_name_of(d.target_id);
            d.returning = c->data.goal_rescue_hf->flag.bits.RETURNING ? 1 : 0;
        }
        break;
    case df::army_controller_goal_type::SITE_INVASION:
        if (c->data.goal_site_invasion) {
            d.target_kind = "invasion";
            d.invasion_intent = DFHack::enum_item_key(c->data.goal_site_invasion->invasion_intent);
            d.returning = c->data.goal_site_invasion->flag.bits.RETURNING_HOME ? 1 : 0;
        }
        break;
    case df::army_controller_goal_type::MAKE_REQUEST:
        if (c->data.goal_make_request)
            d.returning = c->data.goal_make_request->flag.bits.RETURNING_HOME ? 1 : 0;
        break;
    default:
        break;
    }
    return d;
}

// The goal types DF's fortress mission screen can raise, in DF's own order of appearance. `needs`
// tells the client which extra target the goal takes beyond the site. This list is what a create
// would offer -- it is advertised as unavailable, with the reason, rather than hidden, so the
// screen is honest about what DF has and we do not.
struct MissionKind {
    const char* key;
    const char* label;
    const char* needs; // "site" | "artifact" | "hf"
};
const MissionKind kMissionKinds[] = {
    { "SITE_INVASION",    "Raid",              "site"     },
    { "RECOVER_ARTIFACT", "Recover artifact",  "artifact" },
    { "RESCUE_HF",        "Rescue prisoner",   "hf"       },
    { "MAKE_REQUEST",     "Request workers",   "site"     },
};

bool mission_kind_known(const std::string& key) {
    for (const auto& k : kMissionKinds)
        if (key == k.key) return true;
    return false;
}

// ---- the fort's own view of the mission domain -------------------------------------------------

struct FortView {
    df::historical_entity* group = nullptr;
    df::historical_entity* civ = nullptr;
    int32_t own_site = -1;
};

FortView fort_view() {
    FortView v;
    auto plotinfo = df::global::plotinfo;
    if (!plotinfo) return v;
    v.own_site = plotinfo->site_id;
    v.group = df::historical_entity::find(plotinfo->group_id);
    v.civ = df::historical_entity::find(plotinfo->civ_id);
    return v;
}

// The fort's squads, in the site government's own order (historical_entity::squads).
std::vector<df::squad*> fort_squads(const FortView& v) {
    std::vector<df::squad*> out;
    if (!v.group) return out;
    for (int32_t id : v.group->squads)
        if (auto* squad = df::squad::find(id)) out.push_back(squad);
    return out;
}

// Determine which army controllers belong to the fort (a controller is a
// fortress mission if any of its assigned squads is one of ours, or it belongs to our government /
// civ) -- kept identical on purpose so the world-map overlay and this screen can never disagree
// about what counts as an active mission.
bool is_fort_controller(df::army_controller* c, const FortView& v) {
    if (!c || c->assigned_squads.empty()) return false;
    if (v.group) {
        for (int32_t squad_id : c->assigned_squads)
            if (std::find(v.group->squads.begin(), v.group->squads.end(), squad_id) != v.group->squads.end())
                return true;
    }
    auto plotinfo = df::global::plotinfo;
    if (!plotinfo) return false;
    return c->entity_id == plotinfo->group_id || c->entity_id == plotinfo->civ_id;
}

// Candidate targets are every site the fort knows about. A fresh
// player site government's known_sites is EMPTY -- the civ carries them -- so both entities are
// unioned. Our own site is excluded (world_new_mission_type::OWN_SITE is DF's own refusal for it).
std::vector<df::world_site*> candidate_targets(const FortView& v) {
    std::set<int32_t> ids;
    for (auto* e : { v.group, v.civ })
        if (e)
            for (int32_t id : e->relations.known_sites) ids.insert(id);
    std::vector<df::world_site*> out;
    for (int32_t id : ids) {
        if (id == v.own_site) continue;
        if (auto* site = find_site(id)) out.push_back(site);
    }
    std::sort(out.begin(), out.end(), [](df::world_site* a, df::world_site* b) { return a->id < b->id; });
    return out;
}

// ---- GET /missions -----------------------------------------------------------------------------

std::string build_missions_json(const std::string& player, std::string* err) {
    std::ostringstream body;
    bool ok = run_missions_locked([&]() -> bool {
        auto world = df::global::world;
        if (!world || !world->world_data) { if (err) *err = "world data unavailable"; return false; }
        FortView v = fort_view();
        auto* own_site = find_site(v.own_site);

        body << "{\"player\":" << json_string(player)
             << ",\"ownSiteId\":" << v.own_site
             << ",\"ownSite\":" << json_string(site_name_of(own_site))
             << ",\"civ\":" << json_string(entity_name_of(v.civ));

        // --- squads belonging to the fort and not already committed -----------------------------
        auto squads = fort_squads(v);
        body << ",\"squads\":[";
        bool first = true;
        for (auto* squad : squads) {
            if (!first) body << ",";
            first = false;
            auto* army = squad_army(squad);
            bool busy = squad->assigned_army_controller_id != -1 || army != nullptr;
            std::string why;
            if (squad->assigned_army_controller_id != -1) why = "Already assigned to a mission";
            else if (army) why = "Away from the fortress";
            body << "{\"id\":" << squad->id
                 << ",\"name\":" << json_string(DFHack::Military::getSquadName(squad->id))
                 << ",\"memberCount\":" << squad_member_count(squad)
                 << ",\"busy\":" << (busy ? "true" : "false")
                 << ",\"busyReason\":" << json_string(why)
                 << ",\"armyId\":" << (army ? army->id : -1)
                 << ",\"stuck\":" << (army_is_stuck(army) ? "true" : "false")
                 << "}";
        }

        // --- active missions --------------------------------------------------------------------
        body << "],\"active\":[";
        first = true;
        for (auto* c : world->army_controllers.all) {
            if (!is_fort_controller(c, v)) continue;
            if (!first) body << ",";
            first = false;
            GoalDetail d = goal_detail(c);
            std::string goal = DFHack::enum_item_key(c->goal);
            body << "{\"id\":" << c->id
                 << ",\"goal\":" << json_string(goal.empty() ? "Unknown mission" : goal)
                 << ",\"targetSiteId\":" << c->site_id
                 << ",\"targetSite\":" << json_string(site_name_of(find_site(c->site_id)))
                 << ",\"year\":" << c->year
                 << ",\"yearTick\":" << c->year_tick
                 << ",\"reportTitle\":" << json_string(c->mission_report ? c->mission_report->title : "")
                 << ",\"targetKind\":" << json_string(d.target_kind)
                 << ",\"targetId\":" << d.target_id
                 << ",\"targetName\":" << json_string(d.target_name)
                 << ",\"invasionIntent\":" << json_string(d.invasion_intent)
                 << ",\"returning\":" << d.returning
                 << ",\"squads\":[";
            bool sfirst = true;
            bool any_stuck = false;
            for (int32_t squad_id : c->assigned_squads) {
                auto* squad = df::squad::find(squad_id);
                if (!sfirst) body << ",";
                sfirst = false;
                auto* army = squad_army(squad);
                if (army_is_stuck(army)) any_stuck = true;
                body << "{\"id\":" << squad_id
                     << ",\"name\":" << json_string(squad ? DFHack::Military::getSquadName(squad_id) : "")
                     << ",\"memberCount\":" << squad_member_count(squad) << "}";
            }
            body << "],\"stuck\":" << (any_stuck ? "true" : "false") << "}";
        }

        // --- candidate targets, straight off DF's known-sites relation ---------------------------
        body << "],\"targets\":[";
        first = true;
        for (auto* site : candidate_targets(v)) {
            if (!first) body << ",";
            first = false;
            auto* civ = df::historical_entity::find(site->civ_id);
            body << "{\"id\":" << site->id
                 << ",\"name\":" << json_string(site_name_of(site))
                 << ",\"type\":" << json_string(DFHack::enum_item_key(site->type))
                 << ",\"x\":" << site->pos.x
                 << ",\"y\":" << site->pos.y
                 << ",\"civId\":" << site->civ_id
                 << ",\"civ\":" << json_string(entity_name_of(civ))
                 << "}";
        }

        // --- the mission types DF offers, each advertised as native-only -------------------------
        body << "],\"missionTypes\":[";
        first = true;
        for (const auto& k : kMissionKinds) {
            if (!first) body << ",";
            first = false;
            body << "{\"key\":" << json_string(k.key)
                 << ",\"label\":" << json_string(k.label)
                 << ",\"needs\":" << json_string(k.needs)
                 << ",\"available\":" << (kMissionCommitEnabled ? "true" : "false")
                 << "}";
        }

        // --- stranded squads + whether DFHack's repair can actually run right now -----------------
        // Rescue only works when some army or
        // messenger is on its way HOME to carry the stranded members back. Mirrored here so the
        // button is only offered when the script would succeed.
        body << "],\"stuckSquads\":[";
        first = true;
        int stuck_count = 0;
        bool have_returning = false, have_outbound = false;
        for (auto* squad : squads) {
            auto* army = squad_army(squad);
            if (army_is_stuck(army)) {
                if (!first) body << ",";
                first = false;
                ++stuck_count;
                body << "{\"squadId\":" << squad->id
                     << ",\"squadName\":" << json_string(DFHack::Military::getSquadName(squad->id))
                     << ",\"armyId\":" << army->id << "}";
                continue;
            }
            bool valid = false, returning = false;
            army_valid_returning(army, valid, returning);
            if (!valid) continue;
            if (returning) have_returning = true; else have_outbound = true;
        }
        body << "]";

        std::string rescue_reason;
        bool rescue_available = false;
        if (stuck_count == 0) {
            rescue_reason = "No stranded squads.";
        } else if (have_returning) {
            rescue_available = true;
            rescue_reason = "A returning army can carry them home.";
        } else if (have_outbound) {
            rescue_reason = "A squad is out but still outbound -- DFHack can only rescue once "
                            "something is on its way home. Try again when they turn back.";
        } else {
            rescue_reason = "Nothing is returning to the fortress. Send a squad or a messenger on "
                            "a mission that comes home, then rescue once they are on the way back.";
        }
        body << ",\"rescue\":{\"available\":" << (rescue_available ? "true" : "false")
             << ",\"stuckCount\":" << stuck_count
             << ",\"reason\":" << json_string(rescue_reason) << "}";

        // --- the capability advertisement. The client renders THIS, not a hardcoded string. -------
        body << ",\"create\":{\"supported\":" << (kMissionCommitEnabled ? "true" : "false")
             << ",\"blocked\":" << json_string(kMissionCommitEnabled ? "" : "native-only")
             << ",\"reason\":" << json_string(kMissionCommitEnabled ? "" : kNativeOnlyReason)
             << "}}\n";
        return true;
    });
    if (!ok) return "";
    return body.str();
}

// ---- POST /mission-create ----------------------------------------------------------------------
//
// The route performs non-mutating eligibility checks and returns a native-only refusal after
// staging the validated request.

struct StagedOrder {
    std::string goal;
    int32_t site_id = -1;
    std::string site_name;
    std::vector<int32_t> squad_ids;
    std::vector<std::string> squad_names;
    int32_t target_id = -1;   // artifact / hf, when the goal needs one
};

bool do_mission_create(const std::string& goal, int32_t site_id, const std::vector<int32_t>& squad_ids,
                       int32_t target_id, StagedOrder& staged, std::string* err) {
    return run_missions_locked([&]() -> bool {
        if (!df::global::world || !df::global::plotinfo) {
            if (err) *err = "world unavailable";
            return false;
        }
        if (!mission_kind_known(goal)) {
            if (err) *err = "unknown mission type '" + goal + "'";
            return false;
        }
        FortView v = fort_view();
        if (!v.group) {
            if (err) *err = "no fortress government -- missions need a site government";
            return false;
        }

        // Target site: must be one DF says we know about (world_new_mission_type's NOT_IN_CONTACT /
        // OWN_SITE refusals, applied from the same known_sites relation DF reads).
        auto* site = find_site(site_id);
        if (!site) {
            if (err) *err = "no such site";
            return false;
        }
        if (site_id == v.own_site) {
            if (err) *err = "that is your own fortress";
            return false;
        }
        auto targets = candidate_targets(v);
        if (std::find(targets.begin(), targets.end(), site) == targets.end()) {
            if (err) *err = "your civilization has not heard of that site";
            return false;
        }

        // Squads: belonging to the fort, non-empty, and not already committed.
        if (squad_ids.empty()) {
            if (err) *err = "pick at least one squad";
            return false;
        }
        std::set<int32_t> seen;
        for (int32_t sid : squad_ids) {
            if (!seen.insert(sid).second) {
                if (err) *err = "squad listed twice";
                return false;
            }
            auto* squad = df::squad::find(sid);
            if (!squad || std::find(v.group->squads.begin(), v.group->squads.end(), sid) == v.group->squads.end()) {
                if (err) *err = "squad " + std::to_string(sid) + " is not one of your squads";
                return false;
            }
            if (squad->assigned_army_controller_id != -1 || squad_army(squad)) {
                if (err) *err = DFHack::Military::getSquadName(sid) + " is already away on a mission";
                return false;
            }
            if (squad_member_count(squad) == 0) {
                if (err) *err = DFHack::Military::getSquadName(sid) + " has no members";
                return false;
            }
            staged.squad_ids.push_back(sid);
            staged.squad_names.push_back(DFHack::Military::getSquadName(sid));
        }

        // Goal-specific target, validated against the real record (not merely non-negative).
        if (goal == "RECOVER_ARTIFACT") {
            if (!df::artifact_record::find(target_id)) {
                if (err) *err = "recover-artifact needs a real artifact";
                return false;
            }
        } else if (goal == "RESCUE_HF") {
            if (!df::historical_figure::find(target_id)) {
                if (err) *err = "rescue needs a real historical figure";
                return false;
            }
        }

        staged.goal = goal;
        staged.site_id = site_id;
        staged.site_name = site_name_of(site);
        staged.target_id = target_id;

        // Validation is complete, but no army-controller records are written. This hard stop
        // prevents partial controllers or dangling squad assignments.
        if (!kMissionCommitEnabled) {
            if (err) *err = kNativeOnlyReason;
            return false;
        }
        if (err) *err = "mission commit is enabled but unimplemented"; // unreachable; keeps the
        return false;                                                  // guard honest if flipped.
    });
}

std::string staged_json(const StagedOrder& s) {
    std::ostringstream body;
    body << "{\"goal\":" << json_string(s.goal)
         << ",\"targetSiteId\":" << s.site_id
         << ",\"targetSite\":" << json_string(s.site_name)
         << ",\"targetId\":" << s.target_id
         << ",\"squadIds\":[";
    for (size_t i = 0; i < s.squad_ids.size(); ++i) { if (i) body << ","; body << s.squad_ids[i]; }
    body << "],\"squadNames\":[";
    for (size_t i = 0; i < s.squad_names.size(); ++i) {
        if (i) body << ",";
        body << json_string(s.squad_names[i]);
    }
    body << "]}";
    return body.str();
}

// Parse a repeated/comma-joined `squad` parameter: /mission-create?squad=1&squad=2 or ?squad=1,2.
std::vector<int32_t> parse_squads(const httplib::Request& req) {
    std::vector<int32_t> out;
    auto range = req.params.equal_range("squad");
    for (auto it = range.first; it != range.second; ++it) {
        std::stringstream ss(it->second);
        std::string part;
        while (std::getline(ss, part, ',')) {
            try {
                if (!part.empty()) out.push_back(static_cast<int32_t>(std::stol(part)));
            } catch (...) {
                // A non-numeric squad token is dropped here and the order simply fails validation
                // (empty / short squad list) rather than silently resolving to squad 0.
            }
        }
    }
    return out;
}

} // namespace

void register_mission_routes(httplib::Server& server) {
    // GET /missions -> the whole domain: active missions, our squads, candidate targets, mission
    // types, stranded squads, and an honest capability block for the create + rescue writes.
    server.Get("/missions", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        std::string err;
        std::string json = build_missions_json(player, &err);
        if (json.empty()) { json_error(res, 503, err.empty() ? "missions unavailable" : err); return; }
        set_no_store_json(res, json);
    });

    // POST /mission-rescue runs the supported squad-recovery command through the Lua bridge and
    // returns its console result after validating the request.
    auto rescue_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        std::string err;
        std::string output;
        int rescued = 0;
        if (!mission_rescue_stuck_via_lua(rescued, output, &err)) {
            json_error(res, 400, err.empty() ? "rescue failed" : err);
            return;
        }
        diagnostics_log("missions: fix/stuck-squad by " + player + " -> rescued=" +
                        std::to_string(rescued));
        std::ostringstream body;
        body << "{\"ok\":true,\"rescued\":" << rescued
             << ",\"output\":" << json_string(output) << "}\n";
        set_no_store_json(res, body.str());
    };
    server.Get("/mission-rescue", rescue_handler);
    server.Post("/mission-rescue", rescue_handler);
}

} // namespace dfcapture
