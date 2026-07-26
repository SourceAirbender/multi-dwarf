// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// Copyright (C) 2026 Reynaldo Reyes
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

#include "squads.h"

#include "Core.h"
#include "client_state.h"
#include "http_server.h"
#include "json_util.h"
#include "save_barrier.h"
#include "sdl_capture.h"

#include "modules/Gui.h"
#include "modules/Military.h"
#include "modules/Materials.h"
#include "modules/Units.h"

#include "df/alert_state_infost.h"
#include "df/building.h"
#include "df/building_civzonest.h"
#include "df/building_squad_infost.h"
#include "df/burrow.h"
#include "df/descriptor_color.h"
#include "df/entity_material_category.h"
#include "df/entity_position.h"
#include "df/entity_position_assignment.h"
#include "df/entity_uniform.h"
#include "df/entity_uniform_item.h"
#include "df/entity_uniform_type.h"
#include "df/equipment_update.h"
#include "df/gamest.h"
#include "df/global_objects.h"
#include "df/main_interface.h"
#include "df/historical_entity.h"
#include "df/historical_figure.h"
#include "df/interface_squad_modest.h"
#include "df/main_interface.h"
#include "df/histfig_entity_link_positionst.h"
#include "df/histfig_entity_link_squadst.h"
#include "df/item_type.h"
#include "df/itemdef_ammost.h"
#include "df/itemdef_armorst.h"
#include "df/itemdef_glovesst.h"
#include "df/itemdef_handlerst.h"
#include "df/itemdef_helmst.h"
#include "df/itemdef_pantsst.h"
#include "df/itemdef_shieldst.h"
#include "df/itemdef_shoesst.h"
#include "df/itemdef_weaponst.h"
#include "df/job_skill.h"
#include "df/military_routinest.h"
#include "df/plotinfost.h"
#include "df/pointst.h"
#include "df/routest.h"
#include "df/squad.h"
#include "df/squad_ammo_spec.h"
#include "df/squad_barracks_infost.h"
#include "df/squad_equipment_ammo_flag.h"
#include "df/squad_equipmentst.h"
#include "df/squad_infost.h"
#include "df/squad_month_positionst.h"
#include "df/squad_order_defend_burrowsst.h"
#include "df/squad_order_kill_listst.h"
#include "df/squad_order_movest.h"
#include "df/squad_order_patrol_routest.h"
#include "df/squad_order_trainst.h"
#include "df/squad_order_type.h"
#include "df/squad_position.h"
#include "df/squad_position_equipmentst.h"
#include "df/squad_routine_schedulest.h"
#include "df/squad_schedule_order.h"
#include "df/squad_suppliesst.h"
#include "df/squad_water_level_type.h"
#include "df/squad_uniform_spec.h"
#include "df/uniform_category.h"
#include "df/uniform_flags.h"
#include "df/uniform_indiv_choice.h"
#include "df/unit.h"
#include "df/viewscreen_worldst.h"
#include "df/viewunit_interfacest.h"
#include "df/world.h"

#include <algorithm>
#include <cstdint>
#include <array>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace DFHack;

namespace dfcapture {
namespace {

std::recursive_mutex g_squad_mutex;

// Squad/unit/entity reads and mutations touch stable sim structures (like labor.cpp),
// so we serialize them the same way the labor panel does: panel mutex -> capture-state
// mutex -> CoreSuspender. This matches lock ordering with the /frame.jpg render path and
// keeps every squad operation crash-safe. (Reads run under the same guard as mutations
// because iterating world->units.active / squad->positions must not race the sim.)
template <typename Fn>
bool run_squad_locked(Fn&& fn) {
    std::lock_guard<std::recursive_mutex> squad_lock(g_squad_mutex);
    std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
    DFHack::CoreSuspender suspend;
    if (save_barrier_active())
        return false;
    return fn();
}

// ---------------------------------------------------------------------------
// Provider state (plain structs; no df pointers escape run_squad_locked).
// ---------------------------------------------------------------------------

struct PositionUniformSpec {
    int cat = 0;
    int index = 0;
    int item_type = -1;
    int subtype = -1;
    std::string item_name;
    int material_class = -1;
    std::string material_class_name;
    int mattype = -1;
    int matindex = -1;
    std::string material_name;
    int color = -1;
    std::string color_name;
    unsigned int choice = 0;
    int assigned_count = 0;
};

struct SquadMember {
    int idx = 0;
    int32_t unit_id = -1;
    std::string name;
    std::string profession;
    int8_t profession_color = -1;
    std::vector<std::string> top_skills;
    int uniform_items = 0;
    bool filled = false;
    int32_t portrait_texpos = -1;   // -1 when unavailable
    std::vector<PositionUniformSpec> uniform_details; // detail response only (native 5.5)
};

struct SquadOrderInfo {
    int index = 0;
    std::string type;
    std::string description;
};

// One month's entry in the active schedule routine (squad->schedule.routine[
// squad->cur_routine_idx]->month[i]) -- the per-month Sleep/Uniform template DF's own squad
// schedule screen edits. Recurring per-month scheduled orders (squad_schedule_entry::orders,
// e.g. "train months 3-5") are exposed as a read-only count. This editor changes the sleep and
// uniform grid but not individual recurring order assignments.
struct SquadScheduleMonth {
    int month = 0;
    std::string name;
    std::string sleep;
    std::string uniform;
    int order_count = 0;
    // Compact summary of this month's scheduled orders. DF's monthly grid shows
    // the first order's label ("Train"/"Off duty"/…); has_train + min_count drive the training
    // editor's toggle. Per-member order_assignments are not editable here.
    bool has_train = false;
    int min_count = 0;
    std::string order_label; // "No orders" | "Train" | first order's description
};

// One squad's full per-routine schedule, including every
// routine's 12 months, not just the active one). schedule.routine is parallel to the fort-wide
// alerts.routines at the same index. Served detail-only.
struct SquadRoutineSchedule {
    int idx = 0;
    std::string name;
    std::vector<SquadScheduleMonth> months;
};

// One squad_ammo_spec entry rendered for the ammunition editor.
struct SquadAmmoSpec {
    int index = 0;
    int item_subtype = -1;    // ammo itemdef id (0=bolt, 1=arrow, ...)
    std::string ammo_name;    // resolved from the itemdef catalog
    int material_class = -1;
    std::string material_name;
    int mattype = -1;
    int matindex = -1;
    int amount = 0;
    bool use_combat = false;
    bool use_training = false;
};

// Emblem: DF's per-squad badge (df.squad.xml:342-350 on the `squad` struct). symbol is a 0..22
// index into the graphics-mode symbol sheet; fg/bg are the RGB of the coloured glyph. Purely
// cosmetic (graphics mode only) -- served READ on /squads + written via /squad-emblem.
struct SquadEmblem {
    int symbol = 0;
    int fg_r = 0, fg_g = 0, fg_b = 0;
    int bg_r = 0, bg_g = 0, bg_b = 0;
};

struct SquadInfo {
    int32_t id = -1;
    std::string name;
    std::string alias;
    int routine_idx = 0;
    std::string routine_name;
    SquadEmblem emblem;
    // Supplies (native 5.4): squad->supplies. carry_food 0..3 ("No food".."3 food"); carry_water
    // is a name ("none"|"nowater"|"water"|"drink").
    int carry_food = 0;
    std::string carry_water = "none";
    std::vector<SquadMember> positions;
    std::vector<SquadOrderInfo> orders;   // current non-scheduled squad orders
    std::vector<SquadScheduleMonth> schedule;   // active routine's 12 months
    std::vector<SquadRoutineSchedule> routine_schedules; // all routines and their 12 months
    std::vector<SquadAmmoSpec> ammo; // squad->ammo.ammunition[] specs
};

std::string squad_sleep_mode_name(df::squad_sleep_option_type mode) {
    switch (mode) {
        case df::squad_sleep_option_type::AnywhereAtWill: return "anywhere";
        case df::squad_sleep_option_type::InBarracksAtWill: return "barracks-will";
        case df::squad_sleep_option_type::InBarracksAtNeed: return "barracks-need";
        default: return "none";
    }
}

bool parse_squad_sleep_mode(const std::string& s, df::squad_sleep_option_type& out) {
    if (s == "anywhere") { out = df::squad_sleep_option_type::AnywhereAtWill; return true; }
    if (s == "barracks-will") { out = df::squad_sleep_option_type::InBarracksAtWill; return true; }
    if (s == "barracks-need") { out = df::squad_sleep_option_type::InBarracksAtNeed; return true; }
    if (s == "none") { out = df::squad_sleep_option_type::None; return true; }
    return false;
}

std::string squad_uniform_mode_name(df::squad_civilian_uniform_type mode) {
    switch (mode) {
        case df::squad_civilian_uniform_type::Regular: return "regular";
        case df::squad_civilian_uniform_type::Civilian: return "civilian";
        default: return "none";
    }
}

bool parse_squad_uniform_mode(const std::string& s, df::squad_civilian_uniform_type& out) {
    if (s == "regular") { out = df::squad_civilian_uniform_type::Regular; return true; }
    if (s == "civilian") { out = df::squad_civilian_uniform_type::Civilian; return true; }
    if (s == "none") { out = df::squad_civilian_uniform_type::None; return true; }
    return false;
}

// Supplies (native 5.4): squad->supplies.carry_water is a small enum; carry_food is a plain
// 0..3 count ("No food".."3 food"). Names below match the client's radio values.
std::string squad_water_level_name(df::squad_water_level_type w) {
    switch (w) {
        case df::squad_water_level_type::AnyDrink: return "drink";
        case df::squad_water_level_type::Water: return "water";
        case df::squad_water_level_type::NoWater: return "nowater";
        default: return "none";
    }
}

bool parse_squad_water_level(const std::string& s, df::squad_water_level_type& out) {
    if (s == "drink") { out = df::squad_water_level_type::AnyDrink; return true; }
    if (s == "water") { out = df::squad_water_level_type::Water; return true; }
    if (s == "nowater") { out = df::squad_water_level_type::NoWater; return true; }
    if (s == "none") { out = df::squad_water_level_type::None; return true; }
    return false;
}

std::string squad_order_type_name(df::squad_order_type type) {
    switch (type) {
        case df::squad_order_type::MOVE: return "move";
        case df::squad_order_type::KILL_LIST: return "kill";
        case df::squad_order_type::DEFEND_BURROWS: return "defend-burrow";
        case df::squad_order_type::PATROL_ROUTE: return "patrol";
        case df::squad_order_type::TRAIN: return "train";
        case df::squad_order_type::DRIVE_ENTITY_OFF_SITE: return "drive-entity-off-site";
        case df::squad_order_type::CAUSE_TROUBLE_FOR_ENTITY: return "cause-trouble";
        case df::squad_order_type::KILL_HF: return "kill-hf";
        case df::squad_order_type::DRIVE_ARMIES_FROM_SITE: return "drive-armies-off-site";
        case df::squad_order_type::RETRIEVE_ARTIFACT: return "retrieve-artifact";
        case df::squad_order_type::RAID_SITE: return "raid-site";
        case df::squad_order_type::RESCUE_HF: return "rescue-hf";
        default: return "unknown";
    }
}

struct SquadCandidate {
    int32_t unit_id = -1;
    std::string name;
    std::string profession;
    int8_t profession_color = -1;
    std::vector<std::string> top_skills;
    int best_skill_level = 0;     // DF-sourced ordering proxy; never presented as native suitability
    int32_t portrait_texpos = -1;   // -1 when unavailable
};

struct SquadFreePosition {
    int32_t assignment_id = -1;
    int32_t position_id = -1;
    std::string title;
    std::string holder_name;
    std::string appoint_label;
    int squad_size = 0;
};

// A squad-capable position whose raws still allow another holder, matching native's
// create-squad chooser would offer as "a new militia captain" (entity_position.number, -1 =
// AS_NEEDED = unlimited; MILITIA_CAPTAIN is AS_NEEDED in vanilla_entities/objects/
// entity_default.txt:546). No seat exists for it yet, so it has no assignment id: the client first
// POSTs /position-create?position=<positionId> (fort_admin.cpp) to make the seat, then hands the
// returned assignment id to /squad-create?position=. That two-step IS what DF does internally.
struct SquadCreatablePosition {
    int32_t position_id = -1;
    std::string title;
    int squad_size = 0;
    int seats = 0;       // seats that already exist
    int max_seats = -1;  // raws' NUMBER (-1 = unlimited)
};

// Ammunition item-definition catalog entry.
struct AmmoDef {
    int subtype = -1;
    std::string name;
    std::string ammo_class;
};

struct SquadState {
    std::vector<SquadInfo> squads;
    std::vector<std::pair<int, std::string>> routines;       // idx -> name
    std::vector<std::pair<int32_t, std::string>> uniforms;   // id  -> name
    std::vector<SquadCandidate> candidates;
    std::vector<SquadFreePosition> free_positions;           // squad-capable entity assignments
    std::vector<SquadCreatablePosition> creatable_positions;   // seats the raws still allow
    std::vector<AmmoDef> ammo_defs;                          // ammunition catalog
    bool has_free_position = false;
    std::vector<std::string> messages;
};

// Uniform-template details and authoring catalogs served by GET /uniforms.
struct UniformItemDetail {
    int cat = 0;
    int item_type = -1;
    std::string item_type_name;
    int subtype = -1;
    int material_class = -1;
    std::string material_name;
    int mattype = -1;
    int matindex = -1;
    int color = -1;
    unsigned int choice = 0; // uniform_indiv_choice.whole
};

struct UniformTemplateDetail {
    int32_t id = -1;
    std::string name;
    int type = -1;
    bool replace_clothing = false;
    bool exact_matches = false;
    std::vector<UniformItemDetail> items;
};

struct ItemDefEntry {
    int subtype = -1;
    std::string name;
};

struct UniformCatalog {
    std::vector<UniformTemplateDetail> templates;
    std::vector<ItemDefEntry> subtypes[7];                       // per uniform_category 0..6
    std::vector<std::pair<int, std::string>> material_classes;   // value -> name
    struct MaterialChoice { int mattype = -1; int matindex = -1; std::string name; };
    struct ColorChoice { int value = -1; std::string name; };
    std::vector<MaterialChoice> materials;
    std::vector<ColorChoice> colors;
};

std::vector<std::string> top_military_skills(df::unit* unit, size_t cap = 3) {
    using df::job_skill;
    static const job_skill kSkills[] = {
        job_skill::AXE, job_skill::SWORD, job_skill::MACE, job_skill::HAMMER,
        job_skill::SPEAR, job_skill::CROSSBOW, job_skill::WRESTLING,
        job_skill::DODGING, job_skill::SHIELD, job_skill::ARMOR,
        job_skill::DISCIPLINE,
    };
    std::vector<std::pair<int, std::string>> ranked;
    for (auto skill : kSkills) {
        int level = DFHack::Units::getEffectiveSkill(unit, skill);
        if (level <= 0)
            continue;
        auto caption = df::enum_traits<df::job_skill>::attrs(skill).caption_noun;
        ranked.emplace_back(level,
                            std::string(caption ? caption : "?") + " " + std::to_string(level));
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (const auto& e : ranked) {
        out.push_back(e.second);
        if (out.size() >= cap)
            break;
    }
    return out;
}

int best_military_skill_level(df::unit* unit) {
    using df::job_skill;
    static const job_skill kSkills[] = {
        job_skill::AXE, job_skill::SWORD, job_skill::MACE, job_skill::HAMMER,
        job_skill::SPEAR, job_skill::CROSSBOW, job_skill::WRESTLING,
        job_skill::DODGING, job_skill::SHIELD, job_skill::ARMOR,
        job_skill::DISCIPLINE,
    };
    int best = 0;
    for (auto skill : kSkills)
        best = std::max(best, DFHack::Units::getEffectiveSkill(unit, skill));
    return best;
}

// Summarise one schedule month's Sleep/Uniform + scheduled orders into a flat SquadScheduleMonth
// (shared by the active-routine read and the full routine_schedules read). A month with a TRAIN
// order surfaces has_train plus its min_count ("At least N / Train"); otherwise the
// label is the first order's description, or "No orders".
void fill_schedule_month(SquadScheduleMonth& sm, int month, const df::squad_schedule_entry& entry) {
    sm.month = month;
    sm.name = entry.name;
    sm.sleep = squad_sleep_mode_name(entry.sleep_mode);
    sm.uniform = squad_uniform_mode_name(entry.uniform_mode);
    sm.order_count = static_cast<int>(entry.orders.size());
    sm.has_train = false;
    sm.min_count = 0;
    sm.order_label = "No orders";
    for (auto so : entry.orders) {
        if (!so || !so->order) continue;
        if (so->order->getType() == df::squad_order_type::TRAIN) {
            sm.has_train = true;
            sm.min_count = so->min_count;
            sm.order_label = "Train";
            break;
        }
        if (sm.order_label == "No orders") {
            std::string desc;
            so->order->getDescription(&desc);
            sm.order_label = desc.empty() ? squad_order_type_name(so->order->getType()) : desc;
        }
    }
}

int count_position_uniform_items(df::squad_position* pos) {
    if (!pos)
        return 0;
    int total = 0;
    for (int cat = 0; cat <= df::enum_traits<df::uniform_category>::last_item_value; ++cat)
        total += static_cast<int>(pos->equipment.uniform[cat].size());
    return total;
}

// ---------------------------------------------------------------------------
// Uniform and squad-ammunition authoring. Existing fort templates can be applied or edited.
// and ammunition. All the helpers/structs/routes below run under the same
// run_squad_locked -> CoreSuspender discipline as the rest of this file.
// ---------------------------------------------------------------------------

// Canonical uniform_category -> item_type map. Each category carries exactly
// one item_type; the author chooses SUBTYPE (specific itemdef or -1=any) + material + color.
int uniform_item_type_for_category(int cat) {
    switch (cat) {
        case df::uniform_category::body:   return df::item_type::ARMOR;
        case df::uniform_category::head:   return df::item_type::HELM;
        case df::uniform_category::pants:  return df::item_type::PANTS;
        case df::uniform_category::gloves: return df::item_type::GLOVES;
        case df::uniform_category::shoes:  return df::item_type::SHOES;
        case df::uniform_category::shield: return df::item_type::SHIELD;
        case df::uniform_category::weapon: return df::item_type::WEAPON;
        default: return -1;
    }
}

const char* uniform_category_item_name(int cat) {
    switch (cat) {
        case df::uniform_category::body:   return "Body armor";
        case df::uniform_category::head:   return "Helm";
        case df::uniform_category::pants:  return "Legwear";
        case df::uniform_category::gloves: return "Gloves";
        case df::uniform_category::shoes:  return "Footwear";
        case df::uniform_category::shield: return "Shield";
        case df::uniform_category::weapon: return "Weapon";
        default: return "?";
    }
}

const char* material_class_name(int v) {
    if (df::enum_traits<df::entity_material_category>::is_valid(v))
        return df::enum_traits<df::entity_material_category>::key_table[v + 1];
    return "any";
}

std::string uniform_subtype_name(df::world* world, int cat, int subtype) {
    if (!world || subtype < 0) return "any";
    auto pick = [subtype](const auto& defs) -> std::string {
        for (auto* def : defs)
            if (def && def->subtype == subtype) return def->name;
        return "";
    };
    std::string name;
    auto& defs = world->raws.itemdefs;
    switch (cat) {
        case df::uniform_category::body: name = pick(defs.armor); break;
        case df::uniform_category::head: name = pick(defs.helms); break;
        case df::uniform_category::pants: name = pick(defs.pants); break;
        case df::uniform_category::gloves: name = pick(defs.gloves); break;
        case df::uniform_category::shoes: name = pick(defs.shoes); break;
        case df::uniform_category::shield: name = pick(defs.shields); break;
        case df::uniform_category::weapon: name = pick(defs.weapons); break;
    }
    return name.empty() ? ("subtype " + std::to_string(subtype)) : name;
}

// Look up a fort uniform template by id (caller already under run_squad_locked).
df::entity_uniform* find_fort_uniform(int32_t id, std::string* err) {
    auto plotinfo = df::global::plotinfo;
    if (!plotinfo) { if (err) *err = "world unavailable"; return nullptr; }
    auto fort = df::historical_entity::find(plotinfo->group_id);
    if (!fort) { if (err) *err = "fort entity unavailable"; return nullptr; }
    for (auto u : fort->uniforms)
        if (u && u->id == id) return u;
    if (err) *err = "uniform template not found";
    return nullptr;
}

// Nudge the sim to re-evaluate ammunition after an ammo spec change (same discipline the
// uniform-apply used for the uniform categories).
void nudge_squad_ammo(df::squad* squad) {
    if (!squad) return;
    squad->ammo.update.whole |= df::equipment_update::mask_ammo;
    if (df::global::plotinfo)
        df::global::plotinfo->equipment.update.whole |= df::equipment_update::mask_ammo;
}

std::string entity_position_title(df::entity_position* pos) {
    if (!pos) return "Squad";
    if (!pos->squad[0].empty()) return pos->squad[0];
    if (!pos->name[0].empty()) return pos->name[0] + "'s squad";
    return "Squad position " + std::to_string(pos->id);
}

std::string entity_position_name(df::entity_position* pos) {
    if (!pos) return "position";
    if (!pos->name[0].empty()) return pos->name[0];
    return "position " + std::to_string(pos->id);
}

df::entity_position* find_entity_position(df::historical_entity* fort, int32_t position_id) {
    if (!fort) return nullptr;
    for (auto pos : fort->positions.own)
        if (pos && pos->id == position_id) return pos;
    return nullptr;
}

// DF's own current appointment offers. This vector already incorporates population, market,
// replacement, and appointer requirements; squad creation must not infer availability from
// entity_position.number alone.
bool position_is_possible_appointable(df::historical_entity* fort, int32_t position_id) {
    if (!fort) return false;
    for (auto assignment : fort->positions.possible_appointable)
        if (assignment && assignment->position_id == position_id)
            return true;
    return false;
}

// `possible_appointable` is the right source for the Nobles screen, but it is not the complete
// source for native's Create Squad chooser. AS_NEEDED squad offices (vanilla MILITIA_CAPTAIN)
// are synthesized by that chooser once their appointing office is held, even when DF has not put
// another vacant assignment in possible_appointable yet.
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

bool squad_leader_seat_is_available(df::historical_entity* fort,
                                    df::entity_position_assignment* assignment) {
    auto position = fort && assignment
        ? find_entity_position(fort, assignment->position_id) : nullptr;
    const bool eligible = position &&
        !position->flags.is_set(df::entity_position_flags::HAS_BEEN_REPLACED) &&
        (position->requires_population <= 0 ||
         position->flags.is_set(df::entity_position_flags::HAS_MET_POP_REQ));
    return eligible && assignment->squad_id == -1 &&
           (assignment->histfig >= 0 ||
            position_is_possible_appointable(fort, assignment->position_id) ||
            position_is_as_needed_squad_appointable(fort, position));
}

std::string assignment_holder_name(df::entity_position_assignment* asn) {
    if (!asn || asn->histfig < 0) return "";
    auto hf = df::historical_figure::find(asn->histfig);
    if (!hf || hf->unit_id < 0) return "";
    auto unit = df::unit::find(hf->unit_id);
    return unit ? DFHack::Units::getReadableName(unit) : std::string();
}

std::string appoint_label_for_position(df::historical_entity* fort, df::entity_position* pos) {
    if (!fort || !pos || pos->appointed_by.empty()) return "";
    auto appointing = find_entity_position(fort, pos->appointed_by[0]);
    return appointing ? ("appointed by " + entity_position_name(appointing)) : std::string();
}

bool build_squad_state(SquadState& state, std::string* err) {
    auto plotinfo = df::global::plotinfo;
    auto world = df::global::world;
    if (!plotinfo || !world) {
        if (err) *err = "world unavailable";
        return false;
    }
    auto fort = df::historical_entity::find(plotinfo->group_id);
    if (!fort) {
        if (err) *err = "fort entity unavailable";
        return false;
    }

    state = SquadState{};

    for (size_t i = 0; i < plotinfo->alerts.routines.size(); ++i) {
        auto routine = plotinfo->alerts.routines[i];
        state.routines.emplace_back(static_cast<int>(i), routine ? routine->name : std::string());
    }
    for (auto uniform : fort->uniforms)
        if (uniform)
            state.uniforms.emplace_back(uniform->id, uniform->name);

    // Ammunition item-definition catalog for the editor.
    for (auto def : world->raws.itemdefs.ammo) {
        if (!def) continue;
        AmmoDef ad;
        ad.subtype = def->subtype;
        ad.name = def->name;
        ad.ammo_class = def->ammo_class;
        state.ammo_defs.push_back(std::move(ad));
    }

    for (int32_t squad_id : fort->squads) {
        auto squad = df::squad::find(squad_id);
        if (!squad)
            continue;
        SquadInfo row;
        row.id = squad->id;
        row.name = DFHack::Military::getSquadName(squad->id);
        row.alias = squad->alias;
        row.routine_idx = squad->cur_routine_idx;
        if (row.routine_idx >= 0 && row.routine_idx < static_cast<int>(state.routines.size()))
            row.routine_name = state.routines[row.routine_idx].second;
        // Emblem (df.squad.xml:342-350): graphics-mode badge symbol + fg/bg RGB.
        row.emblem.symbol = squad->symbol;
        row.emblem.fg_r = squad->foreground_r;
        row.emblem.fg_g = squad->foreground_g;
        row.emblem.fg_b = squad->foreground_b;
        row.emblem.bg_r = squad->background_r;
        row.emblem.bg_g = squad->background_g;
        row.emblem.bg_b = squad->background_b;
        // Supplies (native 5.4): squad->supplies carry_food (0..3) + carry_water enum.
        row.carry_food = squad->supplies.carry_food;
        row.carry_water = squad_water_level_name(squad->supplies.carry_water);
        // schedule.routine is parallel to alerts.routines (makeSquad allocates one
        // routine entry per fort-wide named routine, same index) -- read the ACTIVE one's 12
        // months for the quick schedule editor, AND every routine's 12 months for the monthly
        // monthly grid and training editor.
        for (int ri = 0; ri < static_cast<int>(squad->schedule.routine.size()); ++ri) {
            auto* routine = squad->schedule.routine[ri];
            if (!routine) continue;
            SquadRoutineSchedule rs;
            rs.idx = ri;
            if (ri < static_cast<int>(state.routines.size())) rs.name = state.routines[ri].second;
            for (int m = 0; m < 12; ++m) {
                SquadScheduleMonth sm;
                fill_schedule_month(sm, m, routine->month[m]);
                if (ri == row.routine_idx) row.schedule.push_back(sm);
                rs.months.push_back(std::move(sm));
            }
            row.routine_schedules.push_back(std::move(rs));
        }
        for (size_t o = 0; o < squad->orders.size(); ++o) {
            auto order = squad->orders[o];
            if (!order)
                continue;
            SquadOrderInfo info;
            info.index = static_cast<int>(o);
            info.type = squad_order_type_name(order->getType());
            order->getDescription(&info.description);
            row.orders.push_back(std::move(info));
        }
        // Squad ammunition specifications.
        for (size_t a = 0; a < squad->ammo.ammunition.size(); ++a) {
            auto spec = squad->ammo.ammunition[a];
            if (!spec) continue;
            SquadAmmoSpec info;
            info.index = static_cast<int>(a);
            info.item_subtype = spec->item_subtype;
            info.material_class = spec->material_class;
            info.material_name = material_class_name(spec->material_class);
            info.mattype = spec->mattype;
            info.matindex = spec->matindex;
            info.amount = spec->amount;
            info.use_combat = spec->flags.bits.use_combat;
            info.use_training = spec->flags.bits.use_training;
            for (const auto& ad : state.ammo_defs)
                if (ad.subtype == spec->item_subtype) { info.ammo_name = ad.name; break; }
            row.ammo.push_back(std::move(info));
        }
        for (size_t p = 0; p < squad->positions.size(); ++p) {
            auto pos = squad->positions[p];
            SquadMember member;
            member.idx = static_cast<int>(p);
            member.unit_id = -1;
            member.filled = false;
            if (pos && pos->occupant != -1) {
                if (auto hf = df::historical_figure::find(pos->occupant)) {
                    if (auto unit = df::unit::find(hf->unit_id)) {
                        member.unit_id = unit->id;
                        member.name = DFHack::Units::getReadableName(unit);
                        member.profession = DFHack::Units::getProfessionName(unit);
                        member.profession_color = DFHack::Units::getProfessionColor(unit);
                        member.top_skills = top_military_skills(unit);
                        member.portrait_texpos = unit->portrait_texpos;
                        member.filled = true;
                    }
                }
            }
            member.uniform_items = count_position_uniform_items(pos);
            if (pos) {
                for (int cat = 0; cat <= df::enum_traits<df::uniform_category>::last_item_value; ++cat) {
                    const auto& specs = pos->equipment.uniform[cat];
                    for (size_t i = 0; i < specs.size(); ++i) {
                        auto* spec = specs[i];
                        if (!spec) continue;
                        PositionUniformSpec detail;
                        detail.cat = cat;
                        detail.index = static_cast<int>(i);
                        detail.item_type = spec->item_type;
                        detail.subtype = spec->item_subtype;
                        detail.item_name = uniform_subtype_name(world, cat, spec->item_subtype);
                        detail.material_class = spec->material_class;
                        detail.material_class_name = material_class_name(spec->material_class);
                        detail.mattype = spec->mattype;
                        detail.matindex = spec->matindex;
                        if (spec->mattype >= 0) {
                            DFHack::MaterialInfo material(spec->mattype, spec->matindex);
                            if (material.isValid()) detail.material_name = material.toString();
                        }
                        detail.color = spec->color;
                        if (auto* color = df::descriptor_color::find(spec->color)) detail.color_name = color->name;
                        detail.choice = spec->indiv_choice.whole;
                        detail.assigned_count = static_cast<int>(spec->assigned.size());
                        member.uniform_details.push_back(std::move(detail));
                    }
                }
            }
            row.positions.push_back(std::move(member));
        }
        state.squads.push_back(std::move(row));
    }

    for (auto unit : world->units.active) {
        // world->units.active retains corpses and real ghosts (isDead covers
        // flags2.killed + flags3.ghostly); a dead soldier's squad_id can clear to -1, so
        // isCitizen + squad_id alone would list the deceased as an assignable candidate.
        // Native also excludes baby and child professions from squad assignment.
        if (!unit || !DFHack::Units::isCitizen(unit) || !DFHack::Units::isActive(unit) ||
            DFHack::Units::isDead(unit) || DFHack::Units::isGhost(unit) ||
            DFHack::Units::isBaby(unit) || DFHack::Units::isChild(unit) ||
            unit->military.squad_id != -1)
            continue;
        SquadCandidate cand;
        cand.unit_id = unit->id;
        cand.name = DFHack::Units::getReadableName(unit);
        cand.profession = DFHack::Units::getProfessionName(unit);
        cand.profession_color = DFHack::Units::getProfessionColor(unit);
        cand.top_skills = top_military_skills(unit);
        cand.best_skill_level = best_military_skill_level(unit);
        cand.portrait_texpos = unit->portrait_texpos;
        state.candidates.push_back(std::move(cand));
    }
    // The native SQUAD_FILL_POSITION selector does not expose appointment_candidatest.value.
    // Keep its rows deterministic with a DF-sourced proxy: strongest effective military skill
    // first, then readable name. Do not serialize or label this as an exact suitability score.
    std::sort(state.candidates.begin(), state.candidates.end(),
              [](const SquadCandidate& a, const SquadCandidate& b) {
                  if (a.best_skill_level != b.best_skill_level)
                      return a.best_skill_level > b.best_skill_level;
                  return a.name < b.name;
              });

    // Native create flow lists every free squad-capable entity assignment (captain of the guard,
    // militia captain, etc.) instead of silently taking the first one.
    for (auto asn : fort->positions.assignments) {
        if (!squad_leader_seat_is_available(fort, asn))
            continue;
        auto pos = find_entity_position(fort, asn->position_id);
        if (!pos || pos->squad_size <= 0)
            continue;
        SquadFreePosition fp;
        fp.assignment_id = asn->id;
        fp.position_id = pos->id;
        fp.title = entity_position_title(pos);
        fp.holder_name = assignment_holder_name(asn);
        fp.appoint_label = appoint_label_for_position(fort, pos);
        fp.squad_size = pos->squad_size;
        state.free_positions.push_back(std::move(fp));
    }
    state.has_free_position = !state.free_positions.empty();

    // Create-position half of native's chooser. A squad-capable position whose
    // raws allow another holder can have a brand-new seat made for it, which is how native keeps
    // offering "a new militia captain" after every existing captain already leads a squad. Bound:
    // entity_position.number (df.entity.xml:977; -1 = AS_NEEDED = unlimited) -- the SAME bound
    // /position-create enforces, so a chooser row here can never be rejected by the write.
    for (auto pos : fort->positions.own) {
        if (!pos || pos->squad_size <= 0 ||
            (!position_is_possible_appointable(fort, pos->id) &&
             !position_is_as_needed_squad_appointable(fort, pos)))
            continue;
        int seats = 0;
        for (auto asn : fort->positions.assignments)
            if (asn && asn->position_id == pos->id)
                ++seats;
        if (pos->number >= 0 && seats >= pos->number)
            continue;
        SquadCreatablePosition cp;
        cp.position_id = pos->id;
        cp.title = entity_position_title(pos);
        cp.squad_size = pos->squad_size;
        cp.seats = seats;
        cp.max_seats = static_cast<int>(pos->number);
        state.creatable_positions.push_back(std::move(cp));
    }

    if (state.squads.empty())
        state.messages.push_back("No squads. Create one (requires a free militia-captain position).");

    return true;
}

bool squads_snapshot(SquadState& state, std::string* err) {
    std::string local_err;
    bool ok = run_squad_locked([&]() { return build_squad_state(state, &local_err); });
    if (!ok && err)
        *err = local_err;
    return ok;
}

// Uniform-template details and authoring catalogs.
bool build_uniform_catalog(UniformCatalog& cat, std::string* err) {
    auto plotinfo = df::global::plotinfo;
    auto world = df::global::world;
    if (!plotinfo || !world) { if (err) *err = "world unavailable"; return false; }
    auto fort = df::historical_entity::find(plotinfo->group_id);
    if (!fort) { if (err) *err = "fort entity unavailable"; return false; }
    cat = UniformCatalog{};

    for (auto u : fort->uniforms) {
        if (!u) continue;
        UniformTemplateDetail t;
        t.id = u->id;
        t.name = u->name;
        t.type = u->type;
        t.replace_clothing = u->flags.bits.replace_clothing;
        t.exact_matches = u->flags.bits.exact_matches;
        for (int c = 0; c <= df::enum_traits<df::uniform_category>::last_item_value; ++c) {
            const auto& types = u->uniform_item_types[c];
            const auto& subs = u->uniform_item_subtypes[c];
            const auto& infos = u->uniform_item_info[c];
            for (size_t i = 0; i < types.size(); ++i) {
                UniformItemDetail d;
                d.cat = c;
                d.item_type = types[i];
                d.item_type_name = uniform_category_item_name(c);
                d.subtype = (i < subs.size()) ? subs[i] : -1;
                if (i < infos.size() && infos[i]) {
                    d.material_class = infos[i]->material_class;
                    d.material_name = material_class_name(infos[i]->material_class);
                    d.mattype = infos[i]->mattype;
                    d.matindex = infos[i]->matindex;
                    d.color = infos[i]->item_color;
                    d.choice = infos[i]->indiv_choice.whole;
                }
                t.items.push_back(std::move(d));
            }
        }
        cat.templates.push_back(std::move(t));
    }

    auto& idf = world->raws.itemdefs;
    auto fill = [](std::vector<ItemDefEntry>& out, const auto& vec) {
        for (auto def : vec) {
            if (!def) continue;
            ItemDefEntry e;
            e.subtype = def->subtype;
            e.name = def->name;
            out.push_back(std::move(e));
        }
    };
    fill(cat.subtypes[df::uniform_category::body], idf.armor);
    fill(cat.subtypes[df::uniform_category::head], idf.helms);
    fill(cat.subtypes[df::uniform_category::pants], idf.pants);
    fill(cat.subtypes[df::uniform_category::gloves], idf.gloves);
    fill(cat.subtypes[df::uniform_category::shoes], idf.shoes);
    fill(cat.subtypes[df::uniform_category::shield], idf.shields);
    fill(cat.subtypes[df::uniform_category::weapon], idf.weapons);

    for (int v = -1; v <= df::enum_traits<df::entity_material_category>::last_item_value; ++v)
        cat.material_classes.emplace_back(v, material_class_name(v));

    // Native's material picker lists broad classes first, then concrete materials.
    for (size_t i = 0; i < world->raws.inorganics.all.size(); ++i) {
        DFHack::MaterialInfo material(0, static_cast<int32_t>(i));
        if (!material.isValid()) continue;
        UniformCatalog::MaterialChoice choice;
        choice.mattype = material.type;
        choice.matindex = material.index;
        choice.name = material.toString();
        cat.materials.push_back(std::move(choice));
    }
    for (size_t i = 0; i < world->raws.descriptors.colors.size(); ++i) {
        auto* color = world->raws.descriptors.colors[i];
        if (!color) continue;
        UniformCatalog::ColorChoice choice;
        choice.value = static_cast<int>(i);
        choice.name = color->name;
        cat.colors.push_back(std::move(choice));
    }

    return true;
}

bool uniform_catalog_snapshot(UniformCatalog& cat, std::string* err) {
    std::string local_err;
    bool ok = run_squad_locked([&]() { return build_uniform_catalog(cat, &local_err); });
    if (!ok && err)
        *err = local_err;
    return ok;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

void append_skills(std::ostringstream& body, const std::vector<std::string>& skills) {
    body << "[";
    for (size_t i = 0; i < skills.size(); ++i) {
        if (i) body << ",";
        body << json_string(skills[i]);
    }
    body << "]";
}

std::string position_name(int idx) {
    return idx == 0 ? std::string("Leader") : std::string("Member");
}

void append_member(std::ostringstream& body, const SquadMember& m, bool include_uniform_details) {
    body << "{\"idx\":" << m.idx
         << ",\"unitId\":" << m.unit_id
         << ",\"name\":" << json_string(m.name)
         << ",\"position\":" << m.idx
         << ",\"positionName\":" << json_string(position_name(m.idx))
         << ",\"profession\":" << json_string(m.profession)
         << ",\"professionColor\":" << static_cast<int>(m.profession_color)
         << ",\"uniformItems\":" << m.uniform_items
         << ",\"filled\":" << (m.filled ? "true" : "false")
         << ",\"portraitTexpos\":" << m.portrait_texpos
         << ",\"topSkills\":";
    append_skills(body, m.top_skills);
    if (include_uniform_details) {
        body << ",\"uniformDetails\":[";
        for (size_t i = 0; i < m.uniform_details.size(); ++i) {
            if (i) body << ",";
            const auto& d = m.uniform_details[i];
            body << "{\"cat\":" << d.cat << ",\"index\":" << d.index
                 << ",\"itemType\":" << d.item_type << ",\"subtype\":" << d.subtype
                 << ",\"itemName\":" << json_string(d.item_name)
                 << ",\"materialClass\":" << d.material_class
                 << ",\"materialClassName\":" << json_string(d.material_class_name)
                 << ",\"mattype\":" << d.mattype << ",\"matindex\":" << d.matindex
                 << ",\"materialName\":" << json_string(d.material_name)
                 << ",\"color\":" << d.color << ",\"colorName\":" << json_string(d.color_name)
                 << ",\"choice\":" << d.choice << ",\"assignedCount\":" << d.assigned_count << "}";
        }
        body << "]";
    }
    body << "}";
}

void append_orders(std::ostringstream& body, const std::vector<SquadOrderInfo>& orders) {
    body << "[";
    for (size_t i = 0; i < orders.size(); ++i) {
        if (i) body << ",";
        const auto& o = orders[i];
        body << "{\"index\":" << o.index
             << ",\"type\":" << json_string(o.type)
             << ",\"description\":" << json_string(o.description)
             << "}";
    }
    body << "]";
}

void append_squad(std::ostringstream& body, const SquadInfo& s, bool include_uniform_details = false) {
    int member_count = 0;
    for (const auto& m : s.positions)
        if (m.filled) ++member_count;
    body << "{\"id\":" << s.id
         << ",\"name\":" << json_string(s.name)
         << ",\"alias\":" << json_string(s.alias)
         << ",\"routineIdx\":" << s.routine_idx
         << ",\"routineName\":" << json_string(s.routine_name)
         << ",\"memberCount\":" << member_count
         << ",\"positionCount\":" << s.positions.size()
         << ",\"emblem\":{\"symbol\":" << s.emblem.symbol
         << ",\"fg\":{\"r\":" << s.emblem.fg_r << ",\"g\":" << s.emblem.fg_g
         << ",\"b\":" << s.emblem.fg_b << "}"
         << ",\"bg\":{\"r\":" << s.emblem.bg_r << ",\"g\":" << s.emblem.bg_g
         << ",\"b\":" << s.emblem.bg_b << "}}"
         << ",\"members\":[";
    for (size_t i = 0; i < s.positions.size(); ++i) {
        if (i) body << ",";
        append_member(body, s.positions[i], include_uniform_details);
    }
    body << "],\"orders\":";
    append_orders(body, s.orders);
    body << "}";
}

void append_messages(std::ostringstream& body, const std::vector<std::string>& messages) {
    body << "\"messages\":[";
    for (size_t i = 0; i < messages.size(); ++i) {
        if (i) body << ",";
        body << json_string(messages[i]);
    }
    body << "]";
}

void append_creatable_positions(std::ostringstream& body,
                                const std::vector<SquadCreatablePosition>& positions) {
    body << "\"creatablePositions\":[";
    for (size_t i = 0; i < positions.size(); ++i) {
        if (i) body << ",";
        const auto& p = positions[i];
        body << "{\"positionId\":" << p.position_id
             << ",\"title\":" << json_string(p.title)
             << ",\"category\":\"new\""
             << ",\"squadSize\":" << p.squad_size
             << ",\"seats\":" << p.seats
             << ",\"maxSeats\":" << p.max_seats
             << "}";
    }
    body << "]";
}

void append_free_positions(std::ostringstream& body, const std::vector<SquadFreePosition>& positions) {
    body << "\"freePositions\":[";
    for (size_t i = 0; i < positions.size(); ++i) {
        if (i) body << ",";
        const auto& p = positions[i];
        body << "{\"assignmentId\":" << p.assignment_id
             << ",\"positionId\":" << p.position_id
             << ",\"title\":" << json_string(p.title)
             << ",\"holderName\":" << json_string(p.holder_name)
             << ",\"appointLabel\":" << json_string(p.appoint_label)
             << ",\"category\":" << json_string(p.holder_name.empty() ? "appoint" : "existing")
             << ",\"squadSize\":" << p.squad_size
             << "}";
    }
    body << "]";
}

std::string squads_list_json(const std::string& player, const SquadState& state) {
    std::ostringstream body;
    body << "{\"player\":" << json_string(player)
         << ",\"hasFreePosition\":" << (state.has_free_position ? "true" : "false")
         << ",";
    append_free_positions(body, state.free_positions);
    body << ",";
    append_creatable_positions(body, state.creatable_positions);
    body << ",\"squads\":[";
    for (size_t i = 0; i < state.squads.size(); ++i) {
        if (i) body << ",";
        append_squad(body, state.squads[i]);
    }
    body << "],";
    append_messages(body, state.messages);
    body << "}\n";
    return body.str();
}

std::string squad_detail_json(const std::string& player, const SquadState& state,
                              const SquadInfo& squad) {
    std::ostringstream body;
    body << "{\"player\":" << json_string(player)
         << ",\"hasFreePosition\":" << (state.has_free_position ? "true" : "false")
         << ",";
    append_free_positions(body, state.free_positions);
    body << ",";
    append_creatable_positions(body, state.creatable_positions);
    body << ",\"squad\":";
    append_squad(body, squad, true);
    body << ",\"routines\":[";
    for (size_t i = 0; i < state.routines.size(); ++i) {
        if (i) body << ",";
        body << "{\"idx\":" << state.routines[i].first
             << ",\"name\":" << json_string(state.routines[i].second) << "}";
    }
    body << "],\"uniforms\":[";
    for (size_t i = 0; i < state.uniforms.size(); ++i) {
        if (i) body << ",";
        body << "{\"id\":" << state.uniforms[i].first
             << ",\"name\":" << json_string(state.uniforms[i].second) << "}";
    }
    // Active routine's 12-month schedule. This is detail-only and does not alter /squads.
    body << "],\"supplies\":{\"food\":" << squad.carry_food
         << ",\"water\":" << json_string(squad.carry_water) << "}";
    body << ",\"schedule\":[";
    for (size_t i = 0; i < squad.schedule.size(); ++i) {
        if (i) body << ",";
        const auto& sm = squad.schedule[i];
        body << "{\"month\":" << sm.month
             << ",\"name\":" << json_string(sm.name)
             << ",\"sleep\":" << json_string(sm.sleep)
             << ",\"uniform\":" << json_string(sm.uniform)
             << ",\"orderCount\":" << sm.order_count
             << ",\"orderLabel\":" << json_string(sm.order_label)
             << ",\"hasTrain\":" << (sm.has_train ? "true" : "false")
             << ",\"minCount\":" << sm.min_count
             << "}";
    }
    // Every routine's full 12-month schedule for this squad.
    body << "],\"routineSchedules\":[";
    for (size_t r = 0; r < squad.routine_schedules.size(); ++r) {
        if (r) body << ",";
        const auto& rs = squad.routine_schedules[r];
        body << "{\"idx\":" << rs.idx << ",\"name\":" << json_string(rs.name) << ",\"months\":[";
        for (size_t i = 0; i < rs.months.size(); ++i) {
            if (i) body << ",";
            const auto& sm = rs.months[i];
            body << "{\"month\":" << sm.month
                 << ",\"sleep\":" << json_string(sm.sleep)
                 << ",\"uniform\":" << json_string(sm.uniform)
                 << ",\"orderCount\":" << sm.order_count
                 << ",\"orderLabel\":" << json_string(sm.order_label)
                 << ",\"hasTrain\":" << (sm.has_train ? "true" : "false")
                 << ",\"minCount\":" << sm.min_count
                 << "}";
        }
        body << "]}";
    }
    // Squad ammunition specs are detail-only and not part of the /squads list shape.
    body << "],\"ammo\":[";
    for (size_t i = 0; i < squad.ammo.size(); ++i) {
        if (i) body << ",";
        const auto& a = squad.ammo[i];
        body << "{\"index\":" << a.index
             << ",\"subtype\":" << a.item_subtype
             << ",\"ammoName\":" << json_string(a.ammo_name)
             << ",\"materialClass\":" << a.material_class
             << ",\"materialName\":" << json_string(a.material_name)
             << ",\"mattype\":" << a.mattype
             << ",\"matindex\":" << a.matindex
             << ",\"amount\":" << a.amount
             << ",\"combat\":" << (a.use_combat ? "true" : "false")
             << ",\"training\":" << (a.use_training ? "true" : "false")
             << "}";
    }
    body << "],\"ammoDefs\":[";
    for (size_t i = 0; i < state.ammo_defs.size(); ++i) {
        if (i) body << ",";
        const auto& ad = state.ammo_defs[i];
        body << "{\"subtype\":" << ad.subtype
             << ",\"name\":" << json_string(ad.name)
             << ",\"ammoClass\":" << json_string(ad.ammo_class)
             << "}";
    }
    body << "],\"candidates\":[";
    for (size_t i = 0; i < state.candidates.size(); ++i) {
        if (i) body << ",";
        const auto& c = state.candidates[i];
        body << "{\"unitId\":" << c.unit_id
             << ",\"name\":" << json_string(c.name)
             << ",\"profession\":" << json_string(c.profession)
             << ",\"professionColor\":" << static_cast<int>(c.profession_color)
             << ",\"portraitTexpos\":" << c.portrait_texpos
             << ",\"topSkills\":";
        append_skills(body, c.top_skills);
        body << "}";
    }
    body << "],";
    append_messages(body, state.messages);
    body << "}\n";
    return body.str();
}

// Full uniform-authoring catalog.
std::string uniform_catalog_json(const std::string& player, const UniformCatalog& cat) {
    std::ostringstream body;
    body << "{\"player\":" << json_string(player) << ",\"uniforms\":[";
    for (size_t t = 0; t < cat.templates.size(); ++t) {
        if (t) body << ",";
        const auto& u = cat.templates[t];
        body << "{\"id\":" << u.id
             << ",\"name\":" << json_string(u.name)
             << ",\"type\":" << u.type
             << ",\"replaceClothing\":" << (u.replace_clothing ? "true" : "false")
             << ",\"exactMatches\":" << (u.exact_matches ? "true" : "false")
             << ",\"items\":[";
        for (size_t i = 0; i < u.items.size(); ++i) {
            if (i) body << ",";
            const auto& it = u.items[i];
            body << "{\"cat\":" << it.cat
                 << ",\"itemType\":" << it.item_type
                 << ",\"itemTypeName\":" << json_string(it.item_type_name)
                 << ",\"subtype\":" << it.subtype
                 << ",\"materialClass\":" << it.material_class
                 << ",\"materialName\":" << json_string(it.material_name)
                 << ",\"mattype\":" << it.mattype
                 << ",\"matindex\":" << it.matindex
                 << ",\"color\":" << it.color
                 << ",\"choice\":" << it.choice
                 << "}";
        }
        body << "]}";
    }
    body << "],\"subtypes\":{";
    for (int c = 0; c <= df::enum_traits<df::uniform_category>::last_item_value; ++c) {
        if (c) body << ",";
        body << "\"" << c << "\":[";
        const auto& list = cat.subtypes[c];
        for (size_t i = 0; i < list.size(); ++i) {
            if (i) body << ",";
            body << "{\"subtype\":" << list[i].subtype
                 << ",\"name\":" << json_string(list[i].name) << "}";
        }
        body << "]";
    }
    body << "},\"materialClasses\":[";
    for (size_t i = 0; i < cat.material_classes.size(); ++i) {
        if (i) body << ",";
        body << "{\"value\":" << cat.material_classes[i].first
             << ",\"name\":" << json_string(cat.material_classes[i].second) << "}";
    }
    body << "],\"materials\":[";
    for (size_t i = 0; i < cat.materials.size(); ++i) {
        if (i) body << ",";
        body << "{\"mattype\":" << cat.materials[i].mattype
             << ",\"matindex\":" << cat.materials[i].matindex
             << ",\"name\":" << json_string(cat.materials[i].name) << "}";
    }
    body << "],\"colors\":[";
    for (size_t i = 0; i < cat.colors.size(); ++i) {
        if (i) body << ",";
        body << "{\"value\":" << cat.colors[i].value
             << ",\"name\":" << json_string(cat.colors[i].name) << "}";
    }
    body << "]}\n";
    return body.str();
}

// ---------------------------------------------------------------------------
// Mutations (all run under run_squad_locked -> CoreSuspender)
// ---------------------------------------------------------------------------

// Leader-seat helpers, forward-declared so do_squad_create
// can seat the commander at position 0 the moment the squad is made).
df::entity_position_assignment* squad_leader_assignment(df::squad* squad);
df::unit* squad_leader_unit(df::squad* squad);
bool seat_leader_at_pos0(df::squad* squad, df::unit* unit, std::string* err);
bool apply_uniform_to_position_locked(df::squad* squad, int32_t pos_idx,
                                      df::entity_uniform* tmpl, std::string* err);

// status: 0 = created, 1 = no free position / requested position unavailable (409),
// 2 = other failure (400). requested_assignment_id < 0 selects the first available position.
int do_squad_create(int32_t requested_assignment_id, int32_t requested_uniform_id,
                    int32_t& new_id, std::string* err) {
    int status = 2;
    run_squad_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) { if (err) *err = "world unavailable"; return false; }
        auto fort = df::historical_entity::find(plotinfo->group_id);
        if (!fort) { if (err) *err = "fort entity unavailable"; return false; }
        df::entity_uniform* uniform = nullptr;
        if (requested_uniform_id >= 0) {
            for (auto candidate : fort->uniforms)
                if (candidate && candidate->id == requested_uniform_id) { uniform = candidate; break; }
            if (!uniform) { if (err) *err = "uniform template not found"; return false; }
        }
        for (auto asn : fort->positions.assignments) {
            if (!squad_leader_seat_is_available(fort, asn))
                continue;
            if (requested_assignment_id >= 0 && asn->id != requested_assignment_id)
                continue;
            auto position = find_entity_position(fort, asn->position_id);
            if (!position || position->squad_size <= 0)
                continue;
            auto squad = DFHack::Military::makeSquad(asn->id);
            if (!squad) { if (err) *err = "makeSquad failed"; status = 2; return false; }
            new_id = squad->id;
            status = 0;
            // Native creation's second screen chooses one template for the new squad. Apply the
            // selected template to every position before returning the new squad to the client.
            if (uniform) {
                for (int32_t pos = 0; pos < static_cast<int32_t>(squad->positions.size()); ++pos) {
                    if (!apply_uniform_to_position_locked(squad, pos, uniform, err)) {
                        status = 2;
                        return false;
                    }
                }
            }
            // Native DF seats the commanding militia captain or commander at position zero when
            // moment the squad is created ("they immediately are the leader / position 0").
            // makeSquad only records leader_position/leader_assignment; it leaves positions[0]
            // unoccupied, which is why position 0 then looked empty and rejected assignment.
            // Seat the assignment holder now. A vacant command position is a no-op.
            if (auto leader = squad_leader_unit(squad)) {
                if (leader->military.squad_id == -1)
                    seat_leader_at_pos0(squad, leader, nullptr);
            }
            return true;
        }
        if (err) {
            *err = requested_assignment_id >= 0
                ? "squad position is no longer available"
                : "no free squad position -- appoint a militia captain first";
        }
        status = 1;
        return false;
    });
    return status;
}

bool do_squad_rename(int32_t squad_id, const std::string& name, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        squad->alias = name;
        return true;
    });
}

// Position zero is the squad leader. Filling it appoints its occupant as the squad's militia
// commander or captain; an empty squad's position zero must remain assignable. The relevant
// DFHack behavior is:
//
//  * `Military::removeFromSquad` (library/modules/Military.cpp:478) is explicitly "based on
//    unitst::remove_squad_info" -- DF's OWN routine -- and at :528 it branches
//    `if (squad_pos == 0) remove_officer_entity_link(hf, squad); else remove_soldier_entity_link(...)`.
//    `remove_officer_entity_link` (:345) finds the noble assignment whose `squad_id == squad->id`
//    and sets `assignment->histfig = -1; assignment->histfig2 = -1;`, drops the hf's
//    histfig_entity_link_positionst and files a former-position link + history event.
//    So: removing the dwarf at squad position 0 VACATES the squad's militia commander/captain seat.
//    The occupant of position 0 therefore HOLDS that seat -- the appointment follows the slot.
//  * `Military::addToSquad` (:426) refuses `squad_pos == 0` for exactly one stated reason:
//    "this function cannot (currently) change the squad commander". A DFHack TODO, not a DF rule.
//  * The noble seat (df::entity_position_assignment, original name entity_position_profilest) and
//    the squad slot (df::squad_position.occupant, original name hfid) are different objects; DF's
//    own fill-position UI is the generic unit selector
//    (df::unit_selector_interfacest{squad_id, squad_position}, context SQUAD_FILL_POSITION) with no
//    slot-0 special case in its candidate set.
//
// Hence: pos 0 accepts any assignable citizen; seating them also performs the appointment (histfig
// + POSITION entity link, displacing any previous holder), which is the exact inverse of what DF's
// remove path undoes. Positions 1..9 are unchanged and still go through addToSquad.

// The fort entity's assignment (noble seat) that commands this squad, or nullptr.
df::entity_position_assignment* squad_leader_assignment(df::squad* squad) {
    if (!squad) return nullptr;
    auto plotinfo = df::global::plotinfo;
    if (!plotinfo) return nullptr;
    auto fort = df::historical_entity::find(plotinfo->group_id);
    if (!fort) return nullptr;
    for (auto asn : fort->positions.assignments)
        if (asn && asn->id == squad->leader_assignment)
            return asn;
    return nullptr;
}

// Resolve the unit currently appointed to the squad's commanding (leader) entity-position
// assignment. Returns nullptr if that assignment is vacant or its holder has no live unit.
df::unit* squad_leader_unit(df::squad* squad) {
    auto asn = squad_leader_assignment(squad);
    if (!asn || asn->histfig < 0) return nullptr;
    auto hf = df::historical_figure::find(asn->histfig);
    if (!hf) return nullptr;
    return df::unit::find(hf->unit_id);
}

// Drop the noble POSITION link for <fort entity, assignment> from a histfig. Same shape as
// fort_admin.cpp's unlink_position_holder (the make-monarch.lua unlink-before-relink recipe) and
// as the link Military.cpp's remove_officer_entity_link removes.
void unlink_leader_position(int32_t hf_id, int32_t entity_id, int32_t assignment_id) {
    auto hf = df::historical_figure::find(hf_id);
    if (!hf) return;
    for (size_t i = 0; i < hf->entity_links.size(); ++i) {
        auto link = virtual_cast<df::histfig_entity_link_positionst>(hf->entity_links[i]);
        if (link && link->entity_id == entity_id && link->assignment_id == assignment_id) {
            delete hf->entity_links[i];
            hf->entity_links.erase(hf->entity_links.begin() + i);
            return;
        }
    }
}

// Appoint `hf` to the squad's commanding noble seat: the write that DF's remove path (see the
// note above) undoes. Idempotent. Caller holds the squad lock.
bool appoint_squad_leader_position(df::squad* squad, df::historical_figure* hf, std::string* err) {
    auto plotinfo = df::global::plotinfo;
    auto fort = plotinfo ? df::historical_entity::find(plotinfo->group_id) : nullptr;
    auto asn = squad_leader_assignment(squad);
    if (!fort || !asn) {
        if (err) *err = "this squad has no commanding position seat (militia commander/captain)";
        return false;
    }
    if (asn->histfig != -1 && asn->histfig != hf->id) {
        unlink_leader_position(asn->histfig, squad->entity_id, asn->id);
        asn->histfig2 = asn->histfig;   // last holder, as DF records it
    }
    asn->histfig = hf->id;
    if (asn->squad_id != squad->id) asn->squad_id = squad->id;  // keep the seat<->squad link honest
    int32_t idx = -1;
    for (size_t i = 0; i < fort->positions.assignments.size(); ++i)
        if (fort->positions.assignments[i] == asn) { idx = static_cast<int32_t>(i); break; }
    for (auto link : hf->entity_links) {
        auto pos_link = virtual_cast<df::histfig_entity_link_positionst>(link);
        if (pos_link && pos_link->entity_id == squad->entity_id &&
            pos_link->assignment_id == asn->id)
            return true;                                        // already appointed
    }
    auto link = df::allocate<df::histfig_entity_link_positionst>();
    if (!link) { if (err) *err = "allocation failed"; return false; }
    link->entity_id = squad->entity_id;
    link->link_strength = 100;
    link->assignment_id = asn->id;
    link->assignment_vector_idx = idx;
    link->start_year = df::global::cur_year ? *df::global::cur_year : 0;
    hf->entity_links.push_back(link);
    return true;
}

// Seat `unit` at squad position 0 (the leader/commander slot) and appoint them to the squad's
// commanding noble seat. Mirrors the bookkeeping Military::addToSquad performs for a normal member
// (occupant, unit->military, equipment-update flags) -- the pieces its pos-0 early-return skips --
// but files the OFFICER (histfig_entity_link_positionst) link rather than the soldier squad link,
// because that is the one DF's remove path takes back off a pos-0 occupant (Military.cpp:528).
// Idempotent: a no-op success if `unit` already leads this squad. Caller holds the squad lock.
bool seat_leader_at_pos0(df::squad* squad, df::unit* unit, std::string* err) {
    if (!squad || squad->positions.empty()) { if (err) *err = "squad has no positions"; return false; }
    auto pos0 = squad->positions[0];
    if (!pos0) { if (err) *err = "leader position missing"; return false; }
    auto hf = df::historical_figure::find(unit->hist_figure_id);
    if (!hf) { if (err) *err = "unit has no historical figure"; return false; }
    if (pos0->occupant == hf->id)                                    // already leader (idempotent)
        return appoint_squad_leader_position(squad, hf, err);
    if (pos0->occupant != -1) { if (err) *err = "leader position is already filled"; return false; }
    if (unit->military.squad_id != -1) { if (err) *err = "unit is already assigned to a squad"; return false; }
    if (!appoint_squad_leader_position(squad, hf, err)) return false;
    pos0->occupant = hf->id;
    unit->military.squad_id = squad->id;
    unit->military.squad_position = 0;
    // Nudge the sim to equip the new leader (same bits addToSquad sets when a member joins).
    #define DFCAPTURE_SEAT_FLAG(flag) df::equipment_update::mask_##flag
    auto update_flags = DFCAPTURE_SEAT_FLAG(weapon) | DFCAPTURE_SEAT_FLAG(armor) |
                        DFCAPTURE_SEAT_FLAG(shoes) | DFCAPTURE_SEAT_FLAG(shield) |
                        DFCAPTURE_SEAT_FLAG(helm) | DFCAPTURE_SEAT_FLAG(gloves) |
                        DFCAPTURE_SEAT_FLAG(ammo) | DFCAPTURE_SEAT_FLAG(pants) |
                        DFCAPTURE_SEAT_FLAG(backpack) | DFCAPTURE_SEAT_FLAG(quiver) |
                        DFCAPTURE_SEAT_FLAG(flask);
    #undef DFCAPTURE_SEAT_FLAG
    squad->ammo.update.whole |= update_flags;
    if (df::global::plotinfo) df::global::plotinfo->equipment.update.whole |= update_flags;
    return true;
}

bool do_squad_assign(int32_t squad_id, int32_t unit_id, int32_t squad_pos, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        auto unit = df::unit::find(unit_id);
        if (!unit) { if (err) *err = "unit not found"; return false; }
        if (!DFHack::Units::isCitizen(unit) || !DFHack::Units::isActive(unit) ||
            DFHack::Units::isDead(unit) || DFHack::Units::isGhost(unit)) {
            if (err) *err = "unit is not an assignable living citizen";
            return false;
        }
        // DFHack 53.15-r1 exposes both predicates (modules/Units.h); checking the freshly
        // resolved unit under CoreSuspender closes the stale/crafted-request path too.
        if (DFHack::Units::isBaby(unit) || DFHack::Units::isChild(unit)) {
            if (err) *err = "unit is a child";
            return false;
        }
        if (unit->military.squad_id != -1) { if (err) *err = "unit is already assigned to a squad"; return false; }
        // Position 0 is an explicit, guarded commander appointment. Automatic placement starts at
        // the first rank seat accepted by Military::addToSquad.
        if (squad_pos < 0) {
            for (size_t i = 1; i < squad->positions.size(); ++i) {
                auto position = squad->positions[i];
                if (!position || position->occupant == -1) {
                    squad_pos = static_cast<int32_t>(i);
                    break;
                }
            }
            if (squad_pos < 0) {
                if (err) *err = "squad has no free non-commander position";
                return false;
            }
        }
        if (squad_pos >= static_cast<int32_t>(squad->positions.size())) {
            if (err) *err = "squad position is out of range";
            return false;
        }
        // Position zero is the leader or commander slot. addToSquad refuses it ("cannot change the
        // squad commander"), so seat the leader ourselves -- ANY assignable citizen, exactly as
        // native does. Seating them IS the appointment: seat_leader_at_pos0 writes the squad's
        // militia commander/captain seat, which is precisely what DF's own remove path vacates
        // when a pos-0 occupant leaves. See the note above for the citations.
        if (squad_pos == 0) {
            return seat_leader_at_pos0(squad, unit, err);
        }
        if (!DFHack::Military::addToSquad(unit_id, squad_id, squad_pos)) {
            if (err) *err = "assign failed (position is occupied / invalid)";
            return false;
        }
        return true;
    });
}

bool do_squad_remove(int32_t unit_id, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto unit = df::unit::find(unit_id);
        if (!unit) { if (err) *err = "unit not found"; return false; }
        if (unit->military.squad_id == -1) { if (err) *err = "unit is not in a squad"; return false; }
        const int32_t was_squad = unit->military.squad_id;
        const int32_t was_pos = unit->military.squad_position;
        const int32_t hf_id = unit->hist_figure_id;
        if (!DFHack::Military::removeFromSquad(unit_id)) { if (err) *err = "remove failed"; return false; }
        // removeFromSquad removes the officer link from a position-zero occupant but does not
        // inspect soldier links. Remove any such link so the unit is fully detached.
        if (was_pos == 0 && hf_id >= 0) {
            if (auto hf = df::historical_figure::find(hf_id)) {
                for (size_t i = 0; i < hf->entity_links.size(); ++i) {
                    auto link = virtual_cast<df::histfig_entity_link_squadst>(hf->entity_links[i]);
                    if (link && link->squad_id == was_squad) {
                        delete hf->entity_links[i];
                        hf->entity_links.erase(hf->entity_links.begin() + i);
                        break;
                    }
                }
            }
        }
        return true;
    });
}

// ---------------------------------------------------------------------------
// Squad orders (move, kill, train, and cancel). Uses the allocation recipe Military.cpp uses
// for the squad-creation default train order (df::allocate<T>(), push onto a live order
// vector, stamp year/year_tick from the world clock) applied to squad->orders -- the
// *immediate* order queue the in-game squad screen's Move/Kill/Train buttons push onto
// This is distinct from squad->schedule, the per-month routine template.
// Patrol and defend-burrow use the same queue. Patrol additionally creates a persistent route in
// plotinfo->waypoints (point_infost.points/routes), which is the canonical store referenced by
// squad_order_patrol_routest.route_id.
// ---------------------------------------------------------------------------

bool do_squad_order_move(int32_t squad_id, int32_t x, int32_t y, int32_t z, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        auto order = df::allocate<df::squad_order_movest>();
        if (!order) { if (err) *err = "allocation failed"; return false; }
        order->year = df::global::cur_year ? *df::global::cur_year : 0;
        order->year_tick = df::global::cur_year_tick ? *df::global::cur_year_tick : 0;
        order->pos = df::coord(x, y, z);
        order->point_id = -1;
        squad->orders.push_back(order);
        return true;
    });
}

// A kill order natively carries a list of targets (squad_order_kill_listst.units): one
// order, many victims -- so multi-select maps onto a single order with several units, not several
// orders. Every id is validated + de-duped before the order is built; the title reads "Kill X"
// for one target and "Kill X +N more" for a set. The single-id overload below keeps the original
// (scalar) call shape working unchanged.
bool do_squad_order_kill(int32_t squad_id, const std::vector<int32_t>& target_unit_ids,
                         std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        if (target_unit_ids.empty()) { if (err) *err = "no target units"; return false; }
        std::vector<int32_t> valid;
        for (int32_t tid : target_unit_ids) {
            if (std::find(valid.begin(), valid.end(), tid) != valid.end())
                continue;  // de-dupe
            if (!df::unit::find(tid)) {
                if (err) *err = "target unit " + std::to_string(tid) + " not found";
                return false;
            }
            valid.push_back(tid);
        }
        auto order = df::allocate<df::squad_order_kill_listst>();
        if (!order) { if (err) *err = "allocation failed"; return false; }
        order->year = df::global::cur_year ? *df::global::cur_year : 0;
        order->year_tick = df::global::cur_year_tick ? *df::global::cur_year_tick : 0;
        std::string first_name;
        for (size_t i = 0; i < valid.size(); ++i) {
            auto target = df::unit::find(valid[i]);
            order->units.push_back(valid[i]);
            if (target->hist_figure_id != -1)
                order->histfigs.push_back(target->hist_figure_id);
            if (i == 0) first_name = DFHack::Units::getReadableName(target);
        }
        order->title = valid.size() == 1
            ? ("Kill " + first_name)
            : ("Kill " + first_name + " +" + std::to_string(valid.size() - 1) + " more");
        squad->orders.push_back(order);
        return true;
    });
}

// Single-target compatibility overload.
bool do_squad_order_kill(int32_t squad_id, int32_t target_unit_id, std::string* err) {
    return do_squad_order_kill(squad_id, std::vector<int32_t>{ target_unit_id }, err);
}

bool do_squad_order_train(int32_t squad_id, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        auto order = df::allocate<df::squad_order_trainst>();
        if (!order) { if (err) *err = "allocation failed"; return false; }
        order->year = df::global::cur_year ? *df::global::cur_year : 0;
        order->year_tick = df::global::cur_year_tick ? *df::global::cur_year_tick : 0;
        squad->orders.push_back(order);
        return true;
    });
}

// index < 0 cancels every current order (matches the in-game "clear orders" affordance).
bool do_squad_order_cancel(int32_t squad_id, int index, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        if (index < 0) {
            for (auto order : squad->orders) delete order;
            squad->orders.clear();
            return true;
        }
        if (index >= static_cast<int>(squad->orders.size())) {
            if (err) *err = "order index out of range";
            return false;
        }
        delete squad->orders[index];
        squad->orders.erase(squad->orders.begin() + index);
        return true;
    });
}

// Native patrol authoring stores each clicked world tile as a persistent waypoint, stores their
// ids on one persistent route, then queues a patrol order that references that route id. Build
// every heap object before touching the live vectors so allocation failure cannot leave a
// half-route behind. Consecutive duplicate clicks are ignored and at least two distinct points
// are required, matching the useful minimum for a route.
bool do_squad_order_patrol(int32_t squad_id, const std::string& requested_name,
                           const std::vector<df::coord>& requested_points,
                           int32_t* created_route_id, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        auto plotinfo = df::global::plotinfo;
        auto world = df::global::world;
        if (!plotinfo || !world) { if (err) *err = "world unavailable"; return false; }

        std::vector<df::coord> points;
        for (const auto& pos : requested_points) {
            if (pos.x < 0 || pos.y < 0 || pos.z < 0 ||
                    pos.x >= world->map.x_count || pos.y >= world->map.y_count ||
                    pos.z >= world->map.z_count) {
                if (err) *err = "patrol point is outside the fortress map";
                return false;
            }
            if (points.empty() || points.back() != pos)
                points.push_back(pos);
        }
        bool has_distinct_pair = false;
        for (size_t i = 1; i < points.size(); ++i)
            if (points[i] != points[0]) { has_distinct_pair = true; break; }
        if (!has_distinct_pair) {
            if (err) *err = "patrol route needs at least two distinct points";
            return false;
        }

        auto* route = new df::routest();
        route->id = plotinfo->waypoints.next_route_id;
        route->name = requested_name.empty()
            ? ("Route " + std::to_string(route->id + 1))
            : requested_name.substr(0, 80);

        std::vector<df::pointst*> waypoints;
        waypoints.reserve(points.size());
        for (size_t i = 0; i < points.size(); ++i) {
            auto* waypoint = new df::pointst();
            waypoint->id = plotinfo->waypoints.next_point_id + static_cast<int32_t>(i);
            waypoint->name = route->name + " " + std::to_string(i + 1);
            waypoint->pos = points[i];
            waypoints.push_back(waypoint);
            route->points.push_back(waypoint->id);
        }

        auto* order = df::allocate<df::squad_order_patrol_routest>();
        if (!order) {
            for (auto* waypoint : waypoints) delete waypoint;
            delete route;
            if (err) *err = "allocation failed";
            return false;
        }
        order->year = df::global::cur_year ? *df::global::cur_year : 0;
        order->year_tick = df::global::cur_year_tick ? *df::global::cur_year_tick : 0;
        order->route_id = route->id;

        for (auto* waypoint : waypoints)
            plotinfo->waypoints.points.push_back(waypoint);
        plotinfo->waypoints.routes.push_back(route);
        plotinfo->waypoints.next_point_id += static_cast<int32_t>(waypoints.size());
        ++plotinfo->waypoints.next_route_id;
        squad->orders.push_back(order);
        if (created_route_id) *created_route_id = route->id;
        return true;
    });
}

// Defend-burrow order. squad_order_defend_burrowsst
// carries exactly one field -- a vector of burrow ids -- so it follows the same allocate/stamp/
// push recipe as move/kill/train above. Every requested id is validated against the live
// plotinfo->burrows.list before the order is built (an invalid id would produce an order the sim
// can never satisfy).
bool do_squad_order_defend_burrow(int32_t squad_id, const std::vector<int32_t>& burrow_ids,
                                  std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) { if (err) *err = "world unavailable"; return false; }
        // De-dupe + validate against live burrows.
        std::vector<int32_t> valid;
        for (int32_t bid : burrow_ids) {
            bool exists = false;
            for (auto b : plotinfo->burrows.list)
                if (b && b->id == bid) { exists = true; break; }
            if (!exists) { if (err) *err = "burrow id " + std::to_string(bid) + " not found"; return false; }
            if (std::find(valid.begin(), valid.end(), bid) == valid.end())
                valid.push_back(bid);
        }
        if (valid.empty()) { if (err) *err = "no valid burrow ids"; return false; }
        auto order = df::allocate<df::squad_order_defend_burrowsst>();
        if (!order) { if (err) *err = "allocation failed"; return false; }
        order->year = df::global::cur_year ? *df::global::cur_year : 0;
        order->year_tick = df::global::cur_year_tick ? *df::global::cur_year_tick : 0;
        for (int32_t bid : valid)
            order->burrows.push_back(bid);
        squad->orders.push_back(order);
        return true;
    });
}

// Squad emblem write. The six colour bytes and symbol index are graphics-mode
// cosmetics (df.squad.xml:342-350) with no sim invariant, so a clamped in-place write is safe.
// -1 sentinels mean "leave unchanged" (partial update): the caller passes -1 for any field the
// client is not editing so toggling one never clobbers the others.
bool do_squad_emblem(int32_t squad_id, int symbol, int fg_r, int fg_g, int fg_b,
                     int bg_r, int bg_g, int bg_b, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
        if (symbol >= 0) squad->symbol = clamp(symbol, 0, 22);
        if (fg_r >= 0) squad->foreground_r = static_cast<uint8_t>(clamp(fg_r, 0, 255));
        if (fg_g >= 0) squad->foreground_g = static_cast<uint8_t>(clamp(fg_g, 0, 255));
        if (fg_b >= 0) squad->foreground_b = static_cast<uint8_t>(clamp(fg_b, 0, 255));
        if (bg_r >= 0) squad->background_r = static_cast<uint8_t>(clamp(bg_r, 0, 255));
        if (bg_g >= 0) squad->background_g = static_cast<uint8_t>(clamp(bg_g, 0, 255));
        if (bg_b >= 0) squad->background_b = static_cast<uint8_t>(clamp(bg_b, 0, 255));
        return true;
    });
}

// Supplies (native 5.4): squad->supplies.carry_food (0..3) + carry_water enum. Both are simple
// scalars on the squad with no sim invariant (they just tell haulers what each member carries),
// so a clamped in-place write is safe. food < 0 or an empty water string leaves that field alone.
bool do_squad_supplies(int32_t squad_id, int food, const std::string& water_str, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        if (food >= 0) {
            if (food > 3) food = 3;
            squad->supplies.carry_food = static_cast<int16_t>(food);
        }
        if (!water_str.empty()) {
            df::squad_water_level_type water;
            if (!parse_squad_water_level(water_str, water)) {
                if (err) *err = "invalid water level (none|nowater|water|drink)";
                return false;
            }
            squad->supplies.carry_water = water;
        }
        return true;
    });
}

// ---------------------------------------------------------------------------
// Routine authoring. Routines are fort-global
// (plotinfo->alerts.routines) but every squad carries a PARALLEL schedule.routine entry at the
// same index (Military.cpp's makeSquad allocates one squad_routine_schedulest per fort routine).
// So create/delete must keep BOTH sides in lockstep across every squad, exactly as DF's own
// add/remove-routine does -- otherwise cur_routine_idx or a monthly read would index out of range.
// ---------------------------------------------------------------------------

// Build one squad's schedule entry for a NEW routine, replicating makeSquad's per-name defaults
// (Off duty / Staggered training / Constant training / Ready / generic). squad_size = number of
// positions. Caller already holds run_squad_locked.
df::squad_routine_schedulest* make_routine_schedule_for_squad(df::squad* squad,
                                                              const std::string& name) {
    int squad_size = static_cast<int>(squad->positions.size());
    auto* schedule = new df::squad_routine_schedulest();
    auto& asched = schedule->month;
    for (int kk = 0; kk < 12; ++kk) {
        for (int jj = 0; jj < squad_size; ++jj) {
            auto* oa = new df::squad_month_positionst();
            oa->assigned_order_idx = -1;
            asched[kk].order_assignments.push_back(oa);
        }
    }
    auto insert_training_order = [&](int month) {
        auto* order = new df::squad_schedule_order();
        order->min_count = squad_size;
        order->positions.resize(squad_size);
        auto* s_order = df::allocate<df::squad_order_trainst>();
        s_order->year = df::global::cur_year ? *df::global::cur_year : 0;
        s_order->year_tick = df::global::cur_year_tick ? *df::global::cur_year_tick : 0;
        order->order = s_order;
        asched[month].orders.push_back(order);
        asched[month].uniform_mode = df::squad_civilian_uniform_type::Regular;
    };
    if (name == "Off duty") {
        for (int i = 0; i < 12; ++i) {
            asched[i].sleep_mode = df::squad_sleep_option_type::AnywhereAtWill;
            asched[i].uniform_mode = df::squad_civilian_uniform_type::Civilian;
        }
    } else if (name == "Staggered training") {
        std::array<int, 6> indices = (squad->id & 1)
            ? std::array<int, 6>{3, 4, 5, 9, 10, 11} : std::array<int, 6>{0, 1, 2, 6, 7, 8};
        for (int index : indices) {
            insert_training_order(index);
            asched[index].sleep_mode = df::squad_sleep_option_type::AnywhereAtWill;
        }
    } else if (name == "Constant training") {
        for (int i = 0; i < 12; ++i) {
            insert_training_order(i);
            asched[i].sleep_mode = df::squad_sleep_option_type::AnywhereAtWill;
        }
    } else if (name == "Ready") {
        for (int i = 0; i < 12; ++i) {
            asched[i].sleep_mode = df::squad_sleep_option_type::InBarracksAtNeed;
            asched[i].uniform_mode = df::squad_civilian_uniform_type::Regular;
        }
    } else {
        for (int i = 0; i < 12; ++i) {
            asched[i].sleep_mode = df::squad_sleep_option_type::AnywhereAtWill;
            asched[i].uniform_mode = df::squad_civilian_uniform_type::Regular;
        }
    }
    return schedule;
}

// Deep-delete one squad_routine_schedulest (every month's scheduled orders + per-position markers),
// mirroring do_squad_delete's schedule-tree teardown.
void free_routine_schedule(df::squad_routine_schedulest* routine) {
    if (!routine) return;
    for (int m = 0; m < 12; ++m) {
        auto& entry = routine->month[m];
        for (auto so : entry.orders) {
            if (so) { delete so->order; delete so; }
        }
        entry.orders.clear();
        for (auto pa : entry.order_assignments) delete pa;
        entry.order_assignments.clear();
    }
    delete routine;
}

bool do_routine_create(int32_t& new_id, const std::string& name, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        auto world = df::global::world;
        if (!plotinfo || !world) { if (err) *err = "world unavailable"; return false; }
        auto fort = df::historical_entity::find(plotinfo->group_id);
        if (!fort) { if (err) *err = "fort entity unavailable"; return false; }
        auto* routine = new df::military_routinest();
        routine->id = plotinfo->alerts.next_routine_id++;
        routine->name = name;
        plotinfo->alerts.routines.push_back(routine);
        // Append a parallel schedule entry to EVERY squad so schedule.routine stays index-aligned.
        for (int32_t sid : fort->squads) {
            auto squad = df::squad::find(sid);
            if (!squad) continue;
            squad->schedule.routine.push_back(make_routine_schedule_for_squad(squad, name));
        }
        new_id = routine->id;
        return true;
    });
}

bool do_routine_rename(int routine_idx, const std::string& name, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) { if (err) *err = "world unavailable"; return false; }
        auto& routines = plotinfo->alerts.routines;
        if (routine_idx < 0 || routine_idx >= static_cast<int>(routines.size()) || !routines[routine_idx]) {
            if (err) *err = "routine index out of range";
            return false;
        }
        routines[routine_idx]->name = name;
        return true;
    });
}

bool do_routine_delete(int routine_idx, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) { if (err) *err = "world unavailable"; return false; }
        auto fort = df::historical_entity::find(plotinfo->group_id);
        if (!fort) { if (err) *err = "fort entity unavailable"; return false; }
        auto& routines = plotinfo->alerts.routines;
        if (routine_idx < 0 || routine_idx >= static_cast<int>(routines.size())) {
            if (err) *err = "routine index out of range";
            return false;
        }
        if (routines.size() <= 1) {
            if (err) *err = "cannot delete the last remaining routine";
            return false;
        }
        // Drop the fort-global routine.
        delete routines[routine_idx];
        routines.erase(routines.begin() + routine_idx);
        // Drop the parallel entry from every squad + repair cur_routine_idx.
        for (int32_t sid : fort->squads) {
            auto squad = df::squad::find(sid);
            if (!squad) continue;
            auto& sroutines = squad->schedule.routine;
            if (routine_idx < static_cast<int>(sroutines.size())) {
                free_routine_schedule(sroutines[routine_idx]);
                sroutines.erase(sroutines.begin() + routine_idx);
            }
            if (squad->cur_routine_idx == routine_idx) squad->cur_routine_idx = 0;
            else if (squad->cur_routine_idx > routine_idx) squad->cur_routine_idx--;
        }
        return true;
    });
}

// ---------------------------------------------------------------------------
// Schedule (squad.schedule months x routines). schedule.routine is parallel to
// plotinfo->alerts.routines (Military.cpp's makeSquad allocates one routine entry per
// fort-wide named routine, at the same index) -- switching cur_routine_idx picks which named
// routine is active (matches DF's own schedule-screen routine-name selector); set-month writes
// the Sleep/Uniform fields of one month of the CURRENTLY active routine (matches the sleep/
// uniform grid under it). Per-month scheduled orders remain read-only through order_count.
// ---------------------------------------------------------------------------

bool do_squad_set_routine(int32_t squad_id, int routine_idx, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        if (routine_idx < 0 || routine_idx >= static_cast<int>(squad->schedule.routine.size())) {
            if (err) *err = "routine index out of range";
            return false;
        }
        squad->cur_routine_idx = routine_idx;
        return true;
    });
}

// routine_idx < 0 selects the squad's active routine.
bool do_squad_schedule_set_month(int32_t squad_id, int routine_idx, int month,
                                  const std::string& sleep_str, const std::string& uniform_str,
                                  const std::string* name, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        if (month < 0 || month > 11) { if (err) *err = "month out of range (0-11)"; return false; }
        int idx = routine_idx >= 0 ? routine_idx : squad->cur_routine_idx;
        if (idx < 0 || idx >= static_cast<int>(squad->schedule.routine.size())) {
            if (err) *err = "squad has no such schedule routine";
            return false;
        }
        auto* active_routine = squad->schedule.routine[idx];
        if (!active_routine) { if (err) *err = "routine schedule unavailable"; return false; }
        df::squad_sleep_option_type sleep_mode;
        if (!parse_squad_sleep_mode(sleep_str, sleep_mode)) {
            if (err) *err = "invalid sleep mode (anywhere|barracks-will|barracks-need|none)";
            return false;
        }
        df::squad_civilian_uniform_type uniform_mode;
        if (!parse_squad_uniform_mode(uniform_str, uniform_mode)) {
            if (err) *err = "invalid uniform mode (regular|civilian|none)";
            return false;
        }
        auto& entry = active_routine->month[month];
        entry.sleep_mode = sleep_mode;
        entry.uniform_mode = uniform_mode;
        if (name) entry.name = *name;
        return true;
    });
}

// Set one routine-month's scheduled order to a single train order with a
// minimum-soldier count -- native "At least N / Train") or clear it ("No orders"). Existing
// orders in that month are deep-deleted first; per-position order_assignments are reset to
// unassigned (-1) since they index into the now-replaced order list. Editing individual member
// assignments are not represented here.
bool do_squad_schedule_set_month_order(int32_t squad_id, int routine_idx, int month,
                                       const std::string& order, int min_count, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        if (month < 0 || month > 11) { if (err) *err = "month out of range (0-11)"; return false; }
        int idx = routine_idx >= 0 ? routine_idx : squad->cur_routine_idx;
        if (idx < 0 || idx >= static_cast<int>(squad->schedule.routine.size())) {
            if (err) *err = "squad has no such schedule routine";
            return false;
        }
        auto* routine = squad->schedule.routine[idx];
        if (!routine) { if (err) *err = "routine schedule unavailable"; return false; }
        if (order != "train" && order != "none") {
            if (err) *err = "invalid order (train|none)";
            return false;
        }
        auto& entry = routine->month[month];
        int squad_size = static_cast<int>(squad->positions.size());
        // Deep-delete the month's existing scheduled orders.
        for (auto so : entry.orders) {
            if (so) { delete so->order; delete so; }
        }
        entry.orders.clear();
        // Any per-position markers now point at nothing -> reset to unassigned.
        for (auto pa : entry.order_assignments) if (pa) pa->assigned_order_idx = -1;
        if (order == "train") {
            int mc = min_count;
            if (mc < 0) mc = 0;
            if (mc > squad_size) mc = squad_size;
            auto* sched_order = new df::squad_schedule_order();
            sched_order->min_count = mc;
            sched_order->positions.resize(squad_size);
            auto* s_order = df::allocate<df::squad_order_trainst>();
            s_order->year = df::global::cur_year ? *df::global::cur_year : 0;
            s_order->year_tick = df::global::cur_year_tick ? *df::global::cur_year_tick : 0;
            sched_order->order = s_order;
            entry.orders.push_back(sched_order);
        }
        return true;
    });
}

// ---------------------------------------------------------------------------
// Uniform assignment applies an existing fort uniform template (fort->uniforms,
// authored via DF's own military Uniforms page) onto one squad position's per-category
// equipment spec vectors. This applies existing templates and does not author new ones. It mirrors
// the template's item_type/subtype/
// material fields into fresh squad_uniform_spec allocations (item id left unset, same as a
// freshly-applied uniform in-game: specific items get matched in by the sim afterward), and
// copies the template's replace-clothing/exact-match flags (same bitfield type on both sides).
// ---------------------------------------------------------------------------

bool apply_uniform_to_position_locked(df::squad* squad, int32_t pos_idx,
                                      df::entity_uniform* tmpl, std::string* err) {
    if (!squad) { if (err) *err = "squad not found"; return false; }
    if (pos_idx < 0 || pos_idx >= static_cast<int32_t>(squad->positions.size())) {
        if (err) *err = "position index out of range";
        return false;
    }
    auto pos = squad->positions[pos_idx];
    if (!pos) { if (err) *err = "position slot is empty"; return false; }
    if (!tmpl) { if (err) *err = "uniform template not found"; return false; }

    for (int cat = 0; cat <= df::enum_traits<df::uniform_category>::last_item_value; ++cat) {
        for (auto spec : pos->equipment.uniform[cat]) delete spec;
        pos->equipment.uniform[cat].clear();
        const auto& types = tmpl->uniform_item_types[cat];
        const auto& subtypes = tmpl->uniform_item_subtypes[cat];
        const auto& infos = tmpl->uniform_item_info[cat];
        for (size_t i = 0; i < types.size(); ++i) {
            auto spec = new df::squad_uniform_spec();
            spec->item_type = types[i];
            spec->item_subtype = (i < subtypes.size()) ? subtypes[i] : static_cast<int16_t>(-1);
            if (i < infos.size() && infos[i]) {
                spec->material_class = infos[i]->material_class;
                spec->mattype = infos[i]->mattype;
                spec->matindex = infos[i]->matindex;
            }
            pos->equipment.uniform[cat].push_back(spec);
        }
    }
    pos->equipment.flags = tmpl->flags;

    #define DFCAPTURE_EQUIP_FLAG(flag) df::equipment_update::mask_##flag
    auto constexpr update_flags = DFCAPTURE_EQUIP_FLAG(weapon) | DFCAPTURE_EQUIP_FLAG(armor) |
                                   DFCAPTURE_EQUIP_FLAG(shoes) | DFCAPTURE_EQUIP_FLAG(shield) |
                                   DFCAPTURE_EQUIP_FLAG(helm) | DFCAPTURE_EQUIP_FLAG(gloves) |
                                   DFCAPTURE_EQUIP_FLAG(ammo) | DFCAPTURE_EQUIP_FLAG(pants) |
                                   DFCAPTURE_EQUIP_FLAG(backpack) | DFCAPTURE_EQUIP_FLAG(quiver) |
                                   DFCAPTURE_EQUIP_FLAG(flask);
    #undef DFCAPTURE_EQUIP_FLAG
    squad->ammo.update.whole |= update_flags;
    if (df::global::plotinfo) df::global::plotinfo->equipment.update.whole |= update_flags;
    return true;
}

bool do_squad_uniform_apply(int32_t squad_id, int32_t pos_idx, int32_t uniform_id, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) { if (err) *err = "world unavailable"; return false; }
        auto fort = df::historical_entity::find(plotinfo->group_id);
        if (!fort) { if (err) *err = "fort entity unavailable"; return false; }
        df::entity_uniform* tmpl = nullptr;
        for (auto u : fort->uniforms)
            if (u && u->id == uniform_id) { tmpl = u; break; }
        return apply_uniform_to_position_locked(squad, pos_idx, tmpl, err);
    });
}

bool do_squad_uniform_clear(int32_t squad_id, int32_t pos_idx, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        if (pos_idx < 0 || pos_idx >= static_cast<int32_t>(squad->positions.size())) {
            if (err) *err = "position index out of range";
            return false;
        }
        auto pos = squad->positions[pos_idx];
        if (!pos) { if (err) *err = "position slot is empty"; return false; }
        for (int cat = 0; cat <= df::enum_traits<df::uniform_category>::last_item_value; ++cat) {
            for (auto spec : pos->equipment.uniform[cat]) delete spec;
            pos->equipment.uniform[cat].clear();
        }
        return true;
    });
}

void mark_squad_equipment_dirty(df::squad* squad) {
    if (!squad) return;
    #define DFCAPTURE_DETAIL_FLAG(flag) df::equipment_update::mask_##flag
    auto constexpr flags = DFCAPTURE_DETAIL_FLAG(weapon) | DFCAPTURE_DETAIL_FLAG(armor) |
                           DFCAPTURE_DETAIL_FLAG(shoes) | DFCAPTURE_DETAIL_FLAG(shield) |
                           DFCAPTURE_DETAIL_FLAG(helm) | DFCAPTURE_DETAIL_FLAG(gloves) |
                           DFCAPTURE_DETAIL_FLAG(pants) | DFCAPTURE_DETAIL_FLAG(backpack) |
                           DFCAPTURE_DETAIL_FLAG(quiver) | DFCAPTURE_DETAIL_FLAG(flask);
    #undef DFCAPTURE_DETAIL_FLAG
    squad->ammo.update.whole |= flags;
    if (df::global::plotinfo) df::global::plotinfo->equipment.update.whole |= flags;
}

bool validate_equipment_filter(int matclass, int mattype, int matindex, int color,
                               std::string* err) {
    if (matclass < -1 || (matclass >= 0 &&
            !df::enum_traits<df::entity_material_category>::is_valid(matclass))) {
        if (err) *err = "invalid material class";
        return false;
    }
    if (mattype >= 0) {
        DFHack::MaterialInfo material(static_cast<int16_t>(mattype), matindex);
        if (!material.isValid()) { if (err) *err = "invalid specific material"; return false; }
    }
    if (color >= 0 && !df::descriptor_color::find(color)) {
        if (err) *err = "invalid color";
        return false;
    }
    return true;
}

bool do_squad_equipment_change(int32_t squad_id, int32_t pos_idx, const std::string& action,
                               int cat, int index, int subtype, int matclass, int mattype,
                               int matindex, int color, unsigned int choice, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto* squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        if (pos_idx < 0 || pos_idx >= static_cast<int>(squad->positions.size()) ||
                !squad->positions[pos_idx]) {
            if (err) *err = "position index out of range";
            return false;
        }
        if (cat < 0 || cat > df::enum_traits<df::uniform_category>::last_item_value) {
            if (err) *err = "uniform category out of range";
            return false;
        }
        auto& specs = squad->positions[pos_idx]->equipment.uniform[cat];
        if (action == "remove") {
            if (index < 0 || index >= static_cast<int>(specs.size())) {
                if (err) *err = "equipment item index out of range";
                return false;
            }
            delete specs[index];
            specs.erase(specs.begin() + index);
            mark_squad_equipment_dirty(squad);
            return true;
        }
        if (!validate_equipment_filter(matclass, mattype, matindex, color, err)) return false;
        df::squad_uniform_spec* spec = nullptr;
        if (action == "add") {
            spec = new df::squad_uniform_spec();
            specs.push_back(spec);
        } else if (action == "update") {
            if (index < 0 || index >= static_cast<int>(specs.size()) || !specs[index]) {
                if (err) *err = "equipment item index out of range";
                return false;
            }
            spec = specs[index];
        } else {
            if (err) *err = "unknown squad-equipment action";
            return false;
        }
        spec->item = -1;
        spec->item_type = static_cast<df::item_type>(uniform_item_type_for_category(cat));
        spec->item_subtype = static_cast<int16_t>(subtype);
        spec->material_class = static_cast<df::entity_material_category>(matclass);
        spec->mattype = static_cast<int16_t>(mattype);
        spec->matindex = matindex;
        spec->color = color;
        spec->indiv_choice.whole = choice;
        spec->assigned.clear();
        mark_squad_equipment_dirty(squad);
        return true;
    });
}

// ---------------------------------------------------------------------------
// Squad deletion. DFHack::Military exposes no single disband call, so this tears the squad down
// way DF's own disband would: release every occupied position (Military::removeFromSquad --
// no position-0 restriction there, unlike addToSquad), free the leader position's assignment
// slot for reuse, deep-delete every heap object the squad exclusively owns (current orders,
// per-position orders + uniform specs, the whole schedule.routine tree, barracks room links
// incl. the building-side backref -- mirrors Military::updateRoomAssignments's own removal
// branch), then unlink the id from fort->squads / world->squads.all and free the squad object.
// ---------------------------------------------------------------------------

// Null the dying squad out of a single raw-pointer cache slot.
inline void null_if_squad(df::squad*& slot, const df::squad* dying) {
    if (slot == dying) slot = nullptr;
}
// Null EVERY occurrence of the dying squad in a raw-pointer cache vector, IN PLACE -- never erase.
// Erasing would shift the parallel metadata vectors these screens index in lockstep
// (viewscreen_worldst.squad_flag, squads.squad_id/sel_squads) out of sync and turn a
// use-after-free into an out-of-bounds read. Nulling keeps every length and selection index intact
// and simply defuses the freed pointer. Used for the surfaces where null entries are tolerable
// (DF marks plotinfo.squads.list has-bad-pointers); the main_interface squad-screen lists get the
// stronger clear-the-parallel-family treatment below instead (see purge_ui_caches_for_squad).
inline void null_squad_in(std::vector<df::squad*>& vec, const df::squad* dying) {
    for (auto*& p : vec) if (p == dying) p = nullptr;
}
// True when the dying squad appears anywhere in a squad* cache vector.
inline bool contains_squad(const std::vector<df::squad*>& vec, const df::squad* dying) {
    for (auto* p : vec) if (p == dying) return true;
    return false;
}

// A df::squad is not a df::building, so squad pointer caches require a parallel purge helper.
// This must run under the caller's CoreSuspender.
//
// Freeing a df::squad while any native squad screen still holds a RAW POINTER to it is a
// use-after-free when a freed object remains
// live in a game.main_interface UI cache, walked on the next frame. df-structures enumerates every
// fort-mode screen that caches a squad*; this nulls the dying squad out of each BEFORE the free.
// Conservative: a slot is touched only when it points at THIS squad; ids are never rewritten
// (main_interface.squad_selector caches squad_id[] -- integers, safe by construction).
//
// Cache sites (df.d_interface.xml / df.plotinfo.xml / df.squad.xml, DFHack 53.15-r1):
//   game.main_interface.view.squad_list_sq   (d_interface:520)  unit view-sheet "assign to squad"
//   game.main_interface.view.name_squad      (d_interface:533)  squad being renamed
//   game.main_interface.barracks_squad       (d_interface:5528) barracks assignment screen
//   game.main_interface.ap_squad             (d_interface:5534) assign-position (single)
//   game.main_interface.ap_squad_list        (d_interface:5538) assign-position-squad list
//   plotinfo.squads.list                     (plotinfo:517)     the 's' military squad-mode list
//   plotinfo.squads.nearest_squad            (plotinfo:531)     hover cache
//   viewscreen_worldst.squad                 (d_interface:7029) world/mission screen's send-on-a-
//     mission squad picker (sibling of squad_flag / civlist / army_controller / messenger_epp).
//     This is a live viewscreen, NOT a main_interface field: it holds pointers to FORT squads
//     (missions.cpp opens it from fort mode to dispatch raids), so a squad freed while it is
//     on the stack would dangle. Walk the whole gview stack and null it there too.
//   world.squads.order_load                  (squad:356)        DF-marked has-bad-pointers; a
//     load-time reconstruction buffer DF does not walk during play (sibling of world.squads.all,
//     which do_squad_delete already erases from). Documented-safe, but nulled here too as cheap,
//     in-scope defense-in-depth so no freed pointer survives anywhere.
// NOT purged: main_interface.squad_selector caches squad_id[] -- integers, safe by construction.
//
// Main-interface squad caches are consumed through widget state. Dismiss each dependent surface
// before clearing its subject. Parallel vectors are cleared together and their selection index is
// reset; single-pointer subjects are nulled and their open or mode flag is closed.
void purge_ui_caches_for_squad(df::squad* squad) {
    if (!squad) return;
    if (auto* game = df::global::game) {
        auto& mi = game->main_interface;
        auto& vu = mi.view;
        // Unit sheet "assign to squad" tab: squad_list_sq is one of FIVE parallel vectors
        // (sq/ep/epp/has_subord_pos/add_index) indexed in lockstep. Clear the whole family and
        // reset the selection, as the sheet's list builder would on rebuild.
        if (contains_squad(vu.squad_list_sq, squad)) {
            vu.squad_list_sq.clear();
            vu.squad_list_ep.clear();
            vu.squad_list_epp.clear();
            vu.squad_list_has_subord_pos.clear();
            vu.squad_list_add_index.clear();
            vu.selected_squad = 0;
        }
        // Squad rename box: name_squad is its subject; naming_squad is its open flag (adjacent in
        // viewunit_interfacest). Nulling the subject without closing the box is the exact
        // Close the dependent location selector as well.
        if (vu.name_squad == squad) {
            vu.name_squad = nullptr;
            vu.naming_squad = false;
        }
        // Barracks assignment list: barracks_squad / barracks_squad_flag are a parallel pair;
        // clear both + reset the index, byte-for-byte what DF's own FUN_1407c49c0 reset does.
        if (contains_squad(mi.barracks_squad, squad)) {
            mi.barracks_squad.clear();
            mi.barracks_squad_flag.clear();
            mi.barracks_selected_squad_ind = 0;
        }
        // Assign-position pickers: if the dying squad is the single-picker subject OR any entry of
        // the squad-list picker, dismiss BOTH pickers -- FUN_1408bd4e0 clears the two flags as a
        // pair, so we do too (conservative; reopening re-derives everything).
        const bool ap_hit = (mi.ap_squad == squad);
        const bool apl_hit = contains_squad(mi.ap_squad_list, squad);
        if (ap_hit)
            mi.ap_squad = nullptr;
        if (apl_hit) {
            mi.ap_squad_list.clear();
            mi.ap_squad_sel = 0;
        }
        if (ap_hit || apl_hit) {
            mi.assigning_position = false;
            mi.assigning_position_squad = false;
        }
    }
    // plotinfo.squads: DF annotates list has-bad-pointers, and DF's own squads-mode reset
    // (FUN_140e5c1c0) nulls list/nearest_squad wholesale -- null-tolerant by DF's own contract, so
    // null-in-place suffices here (list is parallel to squad_id/sel_squads: never erase).
    if (auto* plotinfo = df::global::plotinfo) {
        null_squad_in(plotinfo->squads.list, squad);
        null_if_squad(plotinfo->squads.nearest_squad, squad);
    }
    // The world/mission viewscreen can be on the stack during fort play with fort squads cached;
    // getViewscreenByType<>(0) searches every screen on the stack (n<1 = all), not just the top.
    if (auto* wv = DFHack::Gui::getViewscreenByType<df::viewscreen_worldst>(0))
        null_squad_in(wv->squad, squad);
    if (auto* world = df::global::world)
        null_squad_in(world->squads.order_load, squad);
}

bool do_squad_delete(int32_t squad_id, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        auto world = df::global::world;
        if (!plotinfo || !world) { if (err) *err = "world unavailable"; return false; }
        auto fort = df::historical_entity::find(plotinfo->group_id);
        if (!fort) { if (err) *err = "fort entity unavailable"; return false; }
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }

        // Release every occupied position (leader included).
        for (auto pos : squad->positions) {
            if (!pos || pos->occupant == -1) continue;
            auto hf = df::historical_figure::find(pos->occupant);
            if (hf && hf->unit_id != -1) {
                if (auto unit = df::unit::find(hf->unit_id))
                    DFHack::Military::removeFromSquad(unit->id);
            }
            pos->occupant = -1; // in case removeFromSquad found no live unit to unlink
        }

        // Free the position assignment this squad was tied to (mirrors makeSquad's forward
        // edge: found_assignment->squad_id = result->id).
        for (auto asn : fort->positions.assignments) {
            if (asn && asn->squad_id == squad_id) asn->squad_id = -1;
        }

        // Deep-delete per-position state (own order queue + per-category uniform specs).
        for (auto pos : squad->positions) {
            if (!pos) continue;
            for (auto order : pos->orders) delete order;
            for (int cat = 0; cat <= df::enum_traits<df::uniform_category>::last_item_value; ++cat) {
                for (auto spec : pos->equipment.uniform[cat]) delete spec;
            }
            delete pos;
        }
        squad->positions.clear();

        // Deep-delete the squad's own live order queue.
        for (auto order : squad->orders) delete order;
        squad->orders.clear();

        // Deep-delete the full schedule tree (N routines x 12 months), incl. each month's
        // scheduled orders (own squad_order alloc, per Military.cpp's insert_training_order
        // recipe) and per-position order-assignment markers.
        for (auto routine : squad->schedule.routine) {
            if (!routine) continue;
            for (int m = 0; m < 12; ++m) {
                auto& entry = routine->month[m];
                for (auto sched_order : entry.orders) {
                    if (sched_order) delete sched_order->order;
                    delete sched_order;
                }
                for (auto pa : entry.order_assignments) delete pa;
            }
            delete routine;
        }
        squad->schedule.routine.clear();

        // Barracks rooms: drop this squad's backref from the building side too (mirrors
        // Military::updateRoomAssignments's own removal branch) before deleting our copy.
        for (auto room : squad->rooms) {
            if (!room) continue;
            auto bld = df::building::find(room->building_id);
            auto zone = strict_virtual_cast<df::building_civzonest>(bld);
            if (zone) {
                for (size_t i = 0; i < zone->squad_room_info.size(); ++i) {
                    if (zone->squad_room_info[i] && zone->squad_room_info[i]->squad_id == squad_id) {
                        delete zone->squad_room_info[i];
                        zone->squad_room_info.erase(zone->squad_room_info.begin() + i);
                        break;
                    }
                }
            }
            delete room;
        }
        squad->rooms.clear();

        // Unlink from fort->squads and world->squads.all, then free the squad object.
        for (size_t i = 0; i < fort->squads.size(); ++i) {
            if (fort->squads[i] == squad_id) { fort->squads.erase(fort->squads.begin() + i); break; }
        }
        for (size_t i = 0; i < world->squads.all.size(); ++i) {
            if (world->squads.all[i] == squad) { world->squads.all.erase(world->squads.all.begin() + i); break; }
        }
        // Purge every native squad-UI raw-pointer cache before freeing the squad.
        // Still under run_squad_locked's CoreSuspender.
        purge_ui_caches_for_squad(squad);
        delete squad;
        return true;
    });
}

// ---------------------------------------------------------------------------
// Uniform-template authoring for fort->uniforms.
// Templates are copied on apply, so editing or deleting one never dangles a squad position.
// ---------------------------------------------------------------------------

bool do_uniform_create(int32_t& new_id, const std::string& name, int type, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) { if (err) *err = "world unavailable"; return false; }
        auto fort = df::historical_entity::find(plotinfo->group_id);
        if (!fort) { if (err) *err = "fort entity unavailable"; return false; }
        if (!df::enum_traits<df::entity_uniform_type>::is_valid(type)) {
            if (err) *err = "invalid uniform type (-1..3; 3=Soldier)";
            return false;
        }
        auto u = new df::entity_uniform();
        u->id = fort->next_uniform_id++;
        u->name = name;
        u->type = static_cast<df::entity_uniform_type>(type);
        u->flags.whole = 0;
        fort->uniforms.push_back(u);
        new_id = u->id;
        return true;
    });
}

bool do_uniform_rename(int32_t id, const std::string& name, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto u = find_fort_uniform(id, err);
        if (!u) return false;
        u->name = name;
        return true;
    });
}

bool do_uniform_delete(int32_t id, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) { if (err) *err = "world unavailable"; return false; }
        auto fort = df::historical_entity::find(plotinfo->group_id);
        if (!fort) { if (err) *err = "fort entity unavailable"; return false; }
        for (size_t i = 0; i < fort->uniforms.size(); ++i) {
            auto u = fort->uniforms[i];
            if (!u || u->id != id) continue;
            for (int cat = 0; cat <= df::enum_traits<df::uniform_category>::last_item_value; ++cat)
                for (auto info : u->uniform_item_info[cat]) delete info;
            delete u;
            fort->uniforms.erase(fort->uniforms.begin() + i);
            return true;
        }
        if (err) *err = "uniform template not found";
        return false;
    });
}

bool do_uniform_item_add(int32_t id, int cat, int subtype, int matclass, int mattype,
                         int matindex, int color, unsigned int choice, std::string* err) {
    return run_squad_locked([&]() -> bool {
        if (cat < 0 || cat > df::enum_traits<df::uniform_category>::last_item_value) {
            if (err) *err = "category out of range (0=body..6=weapon)";
            return false;
        }
        auto u = find_fort_uniform(id, err);
        if (!u) return false;
        int it = uniform_item_type_for_category(cat);
        auto info = new df::entity_uniform_item();
        info->material_class = static_cast<df::entity_material_category>(matclass);
        info->mattype = static_cast<int16_t>(mattype);
        info->matindex = matindex;
        info->item_color = static_cast<int16_t>(color);
        info->indiv_choice.whole = choice;
        // Keep the three parallel arrays exactly in lockstep (DF's own invariant).
        u->uniform_item_types[cat].push_back(static_cast<df::item_type>(it));
        u->uniform_item_subtypes[cat].push_back(static_cast<int16_t>(subtype));
        u->uniform_item_info[cat].push_back(info);
        return true;
    });
}

bool do_uniform_item_remove(int32_t id, int cat, int index, std::string* err) {
    return run_squad_locked([&]() -> bool {
        if (cat < 0 || cat > df::enum_traits<df::uniform_category>::last_item_value) {
            if (err) *err = "category out of range (0=body..6=weapon)";
            return false;
        }
        auto u = find_fort_uniform(id, err);
        if (!u) return false;
        auto& types = u->uniform_item_types[cat];
        auto& subs = u->uniform_item_subtypes[cat];
        auto& infos = u->uniform_item_info[cat];
        if (index < 0 || index >= static_cast<int>(types.size())) {
            if (err) *err = "item index out of range";
            return false;
        }
        if (index < static_cast<int>(infos.size()) && infos[index]) delete infos[index];
        types.erase(types.begin() + index);
        if (index < static_cast<int>(subs.size())) subs.erase(subs.begin() + index);
        if (index < static_cast<int>(infos.size())) infos.erase(infos.begin() + index);
        return true;
    });
}

bool do_uniform_flags(int32_t id, bool replace_clothing, bool exact_matches, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto u = find_fort_uniform(id, err);
        if (!u) return false;
        u->flags.bits.replace_clothing = replace_clothing ? 1 : 0;
        u->flags.bits.exact_matches = exact_matches ? 1 : 0;
        return true;
    });
}

// ---------------------------------------------------------------------------
// Squad ammunition authoring. item_type is always AMMO;
// the author chooses the ammo itemdef subtype, material, amount, and combat/training use flags.
// ---------------------------------------------------------------------------

bool do_squad_ammo_add(int32_t squad_id, int subtype, int amount, int matclass, int mattype,
                       int matindex, bool combat, bool training, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        auto spec = new df::squad_ammo_spec();
        spec->item_type = df::item_type::AMMO;
        spec->item_subtype = static_cast<int16_t>(subtype);
        spec->material_class = static_cast<df::entity_material_category>(matclass);
        spec->mattype = static_cast<int16_t>(mattype);
        spec->matindex = matindex;
        spec->amount = amount;
        spec->flags.bits.use_combat = combat ? 1 : 0;
        spec->flags.bits.use_training = training ? 1 : 0;
        squad->ammo.ammunition.push_back(spec);
        nudge_squad_ammo(squad);
        return true;
    });
}

bool do_squad_ammo_update(int32_t squad_id, int index, int amount, int matclass, int mattype,
                          int matindex, bool combat, bool training, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        if (index < 0 || index >= static_cast<int>(squad->ammo.ammunition.size())) {
            if (err) *err = "ammo index out of range";
            return false;
        }
        auto spec = squad->ammo.ammunition[index];
        if (!spec) { if (err) *err = "ammo spec slot empty"; return false; }
        spec->amount = amount;
        spec->material_class = static_cast<df::entity_material_category>(matclass);
        spec->mattype = static_cast<int16_t>(mattype);
        spec->matindex = matindex;
        spec->flags.bits.use_combat = combat ? 1 : 0;
        spec->flags.bits.use_training = training ? 1 : 0;
        nudge_squad_ammo(squad);
        return true;
    });
}

bool do_squad_ammo_remove(int32_t squad_id, int index, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        auto& ammo = squad->ammo.ammunition;
        if (index < 0 || index >= static_cast<int>(ammo.size())) {
            if (err) *err = "ammo index out of range";
            return false;
        }
        delete ammo[index];
        ammo.erase(ammo.begin() + index);
        nudge_squad_ammo(squad);
        return true;
    });
}

bool do_squad_ammo_clear(int32_t squad_id, std::string* err) {
    return run_squad_locked([&]() -> bool {
        auto squad = df::squad::find(squad_id);
        if (!squad) { if (err) *err = "squad not found"; return false; }
        for (auto spec : squad->ammo.ammunition) delete spec;
        squad->ammo.ammunition.clear();
        nudge_squad_ammo(squad);
        return true;
    });
}

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------

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

} // namespace

void register_squad_routes(httplib::Server& server) {
    // GET /squads?player= -> full squad list with members.
    server.Get("/squads", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        SquadState state;
        std::string err;
        if (!squads_snapshot(state, &err)) {
            json_error(res, 503, err);
            return;
        }
        set_no_store_json(res, squads_list_json(player, state));
    });

    // GET /squad?player=&id= -> one squad's detail + routines/uniforms/candidates.
    server.Get("/squad", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int id = -1;
        if (!query_int(req, "id", id)) {
            json_error(res, 400, "missing id");
            return;
        }
        SquadState state;
        std::string err;
        if (!squads_snapshot(state, &err)) {
            json_error(res, 503, err);
            return;
        }
        const SquadInfo* found = nullptr;
        for (const auto& s : state.squads) {
            if (s.id == id) { found = &s; break; }
        }
        if (!found) {
            json_error(res, 404, "squad not found");
            return;
        }
        set_no_store_json(res, squad_detail_json(player, state, *found));
    });

    // POST /squad-create[?position=<assignment id>&uniform=<template id>] -> native's two-step
    // create flow: choose the squad-leading seat, then choose a uniform (omit uniform for none).
    auto squad_create_handler = [](const httplib::Request& req, httplib::Response& res) {
        int32_t new_id = -1;
        int32_t assignment_id = -1;
        int32_t uniform_id = -1;
        if (req.has_param("position") && !query_int(req, "position", assignment_id)) {
            json_error(res, 400, "invalid position");
            return;
        }
        if (assignment_id < 0 && req.has_param("assignment") && !query_int(req, "assignment", assignment_id)) {
            json_error(res, 400, "invalid assignment");
            return;
        }
        if (req.has_param("uniform") && !query_int(req, "uniform", uniform_id)) {
            json_error(res, 400, "invalid uniform");
            return;
        }
        std::string err;
        int status = do_squad_create(assignment_id, uniform_id, new_id, &err);
        if (status == 0) {
            set_no_store_json(res, "{\"ok\":true,\"id\":" + std::to_string(new_id) + "}\n");
            return;
        }
        json_error(res, status == 1 ? 409 : 400, err);
    };
    server.Get("/squad-create", squad_create_handler);
    server.Post("/squad-create", squad_create_handler);

    // POST /squad-rename?id=&name= -> set squad alias.
    auto squad_rename_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id) || !req.has_param("name")) {
            json_error(res, 400, "missing id/name");
            return;
        }
        std::string name = req.get_param_value("name");
        if (name.size() > 64)
            name.resize(64);
        std::string err;
        if (!do_squad_rename(id, name, &err)) {
            json_error(res, 400, err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/squad-rename", squad_rename_handler);
    server.Post("/squad-rename", squad_rename_handler);

    // POST /squad-assign?squad=&unit=&pos= (pos optional, default -1 = first free).
    auto squad_assign_handler = [](const httplib::Request& req, httplib::Response& res) {
        int squad_id = -1;
        int unit_id = -1;
        if (!query_int(req, "squad", squad_id) || !query_int(req, "unit", unit_id)) {
            json_error(res, 400, "missing squad/unit");
            return;
        }
        int pos = -1;
        query_int(req, "pos", pos);
        // Position zero seats the squad commander and updates the noble record.
        std::string err;
        if (!do_squad_assign(squad_id, unit_id, pos, &err)) {
            json_error(res, 400, err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/squad-assign", squad_assign_handler);
    server.Post("/squad-assign", squad_assign_handler);

    // POST /squad-remove?unit= -> removeFromSquad.
    auto squad_remove_handler = [](const httplib::Request& req, httplib::Response& res) {
        int unit_id = -1;
        if (!query_int(req, "unit", unit_id)) {
            json_error(res, 400, "missing unit");
            return;
        }
        std::string err;
        if (!do_squad_remove(unit_id, &err)) {
            json_error(res, 400, err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/squad-remove", squad_remove_handler);
    server.Post("/squad-remove", squad_remove_handler);

    // POST /squad-delete?squad= disbands a squad. It releases every member, frees the leader
    // position's assignment slot, and deep-
    // deletes every heap object the squad exclusively owns, unlinks it from fort->squads /
    // world->squads.all. Irreversible; client should confirm before calling.
    auto squad_delete_handler = [](const httplib::Request& req, httplib::Response& res) {
        int squad_id = -1;
        if (!query_int(req, "squad", squad_id)) {
            json_error(res, 400, "missing squad");
            return;
        }
        // Disbanding is available to every authenticated player. do_squad_delete first nulls the
        // squad out of every fort-mode squad-UI cache (df.d_interface.xml view.squad_list_sq/
        // name_squad/barracks_squad/ap_squad/ap_squad_list + plotinfo.squads.list/nearest_squad)
        // before freeing it, all under CoreSuspender. Join-auth refuses unauthenticated callers.
        std::string err;
        if (!do_squad_delete(squad_id, &err)) {
            json_error(res, 400, err);
            return;
        }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/squad-delete", squad_delete_handler);
    server.Post("/squad-delete", squad_delete_handler);

    // POST /squad-order?squad=&action=move|kill|train|cancel|patrol|defend-burrow.
    // move: player=&px=&py=&w=&h= (same tile-grid pixel contract as /designate and
    //   /hauling-stop-add -- px/py index into the requesting player's rendered window; z comes
    //   from that player's camera). kill: target=<unit id>. train: no extra params.
    // cancel: index=<n> (order slot) or all=1 to clear every current order.
    // patrol: name=<route name>&points=x:y:z;x:y:z (persistent world coordinates).
    // defend-burrow: burrows=<csv ids from GET /burrows>.
    auto squad_order_handler = [](const httplib::Request& req, httplib::Response& res) {
        int squad_id = -1;
        if (!query_int(req, "squad", squad_id) || !req.has_param("action")) {
            json_error(res, 400, "missing squad/action");
            return;
        }
        std::string action = req.get_param_value("action");
        std::string err;
        int32_t patrol_route_id = -1;

        if (action == "move") {
            std::string player = query_player(req);
            int px = 0, py = 0, frame_w = 0, frame_h = 0;
            if (!query_int(req, "px", px) || !query_int(req, "py", py) ||
                    !query_int(req, "w", frame_w) || !query_int(req, "h", frame_h)) {
                json_error(res, 400, "missing px/py/w/h");
                return;
            }
            Camera camera;
            if (!camera_for_player(player, camera, &err)) {
                json_error(res, 503, err.empty() ? "camera unavailable" : err);
                return;
            }
            int probe_w = 0, probe_h = 0;
            if (!effective_capture_viewport_dims(camera, probe_w, probe_h, &err)) {
                json_error(res, 503, err.empty() ? "viewport unavailable" : err);
                return;
            }
            int tx = frame_w > 0 ? std::max(0, std::min(frame_w - 1, px)) : 0;
            int ty = frame_h > 0 ? std::max(0, std::min(frame_h - 1, py)) : 0;
            if (!do_squad_order_move(squad_id, camera.x + tx, camera.y + ty, camera.z, &err)) {
                json_error(res, 400, err);
                return;
            }
        } else if (action == "kill") {
            // Multi-target uses targets=<csv of unit ids>. Single target=<id> remains accepted
            // for backward compatibility.
            std::vector<int32_t> targets;
            if (req.has_param("targets")) {
                std::stringstream ss(req.get_param_value("targets"));
                std::string tok;
                while (std::getline(ss, tok, ',')) {
                    try {
                        size_t consumed = 0;
                        int v = std::stoi(tok, &consumed);
                        if (consumed > 0) targets.push_back(v);
                    } catch (...) { /* skip non-numeric token */ }
                }
                if (targets.empty()) {
                    json_error(res, 400, "no valid unit ids in 'targets'");
                    return;
                }
            } else {
                int target = -1;
                if (!query_int(req, "target", target)) {
                    json_error(res, 400, "missing target (or targets csv)");
                    return;
                }
                targets.push_back(target);
            }
            if (!do_squad_order_kill(squad_id, targets, &err)) {
                json_error(res, 400, err);
                return;
            }
        } else if (action == "train") {
            if (!do_squad_order_train(squad_id, &err)) {
                json_error(res, 400, err);
                return;
            }
        } else if (action == "cancel") {
            int all = 0, index = -1;
            query_int(req, "all", all);
            query_int(req, "index", index);
            if (!all && index < 0) {
                json_error(res, 400, "missing index (or all=1)");
                return;
            }
            if (!do_squad_order_cancel(squad_id, all ? -1 : index, &err)) {
                json_error(res, 400, err);
                return;
            }
        } else if (action == "defend-burrow") {
            // burrows=<csv of burrow ids> (ids from GET /burrows). All validated server-side.
            if (!req.has_param("burrows")) {
                json_error(res, 400, "missing burrows (csv of burrow ids)");
                return;
            }
            std::vector<int32_t> ids;
            std::stringstream ss(req.get_param_value("burrows"));
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                try {
                    size_t consumed = 0;
                    int v = std::stoi(tok, &consumed);
                    if (consumed > 0) ids.push_back(v);
                } catch (...) { /* skip non-numeric token */ }
            }
            if (ids.empty()) {
                json_error(res, 400, "no valid burrow ids in 'burrows'");
                return;
            }
            if (!do_squad_order_defend_burrow(squad_id, ids, &err)) {
                json_error(res, 400, err);
                return;
            }
        } else if (action == "patrol") {
            if (!req.has_param("points")) {
                json_error(res, 400, "missing points (semicolon-separated x:y:z world coordinates)");
                return;
            }
            std::vector<df::coord> points;
            std::stringstream point_stream(req.get_param_value("points"));
            std::string point_token;
            while (std::getline(point_stream, point_token, ';')) {
                std::stringstream coord_stream(point_token);
                std::string coord_token;
                std::vector<int> xyz;
                while (std::getline(coord_stream, coord_token, ':')) {
                    try {
                        size_t consumed = 0;
                        int value = std::stoi(coord_token, &consumed);
                        if (consumed != coord_token.size()) { xyz.clear(); break; }
                        xyz.push_back(value);
                    } catch (...) { xyz.clear(); break; }
                }
                if (xyz.size() != 3) {
                    json_error(res, 400, "invalid patrol point: expected x:y:z");
                    return;
                }
                if (xyz[0] < 0 || xyz[1] < 0 || xyz[2] < 0 ||
                        xyz[0] > 32767 || xyz[1] > 32767 || xyz[2] > 32767) {
                    json_error(res, 400, "patrol point is outside the supported coordinate range");
                    return;
                }
                points.emplace_back(xyz[0], xyz[1], xyz[2]);
            }
            std::string name = req.has_param("name") ? req.get_param_value("name") : "";
            if (!do_squad_order_patrol(squad_id, name, points, &patrol_route_id, &err)) {
                json_error(res, 400, err);
                return;
            }
        } else {
            json_error(res, 400, "unknown squad-order action: " + action);
            return;
        }
        notify_player_input();
        if (patrol_route_id >= 0)
            set_no_store_json(res, "{\"ok\":true,\"routeId\":" + std::to_string(patrol_route_id) + "}\n");
        else
            set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/squad-order", squad_order_handler);
    server.Post("/squad-order", squad_order_handler);

    // POST /squad-emblem?squad=&symbol=&fgR=&fgG=&fgB=&bgR=&bgG=&bgB=.
    // Writes the squad's graphics-mode badge (symbol 0-22, fg/bg RGB 0-255). Every field is
    // OPTIONAL: any param omitted (or sent <0) leaves that field unchanged, so the client can
    // toggle one component without resending the rest. Read side rides GET /squads ("emblem":{}).
    auto squad_emblem_handler = [](const httplib::Request& req, httplib::Response& res) {
        int squad_id = -1;
        if (!query_int(req, "squad", squad_id)) {
            json_error(res, 400, "missing squad");
            return;
        }
        int symbol = -1, fg_r = -1, fg_g = -1, fg_b = -1, bg_r = -1, bg_g = -1, bg_b = -1;
        query_int(req, "symbol", symbol);
        query_int(req, "fgR", fg_r);
        query_int(req, "fgG", fg_g);
        query_int(req, "fgB", fg_b);
        query_int(req, "bgR", bg_r);
        query_int(req, "bgG", bg_g);
        query_int(req, "bgB", bg_b);
        std::string err;
        if (!do_squad_emblem(squad_id, symbol, fg_r, fg_g, fg_b, bg_r, bg_g, bg_b, &err)) {
            json_error(res, 400, err);
            return;
        }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/squad-emblem", squad_emblem_handler);
    server.Post("/squad-emblem", squad_emblem_handler);

    // POST /squad-schedule?squad=&action=set-routine|set-month.
    // set-routine: routine=<idx> -- switch the squad's active routine (schedule.routine is
    //   parallel to the /squad detail response's "routines" list -- same index both places).
    // set-month: month=<0-11>&sleep=<anywhere|barracks-will|barracks-need|none>&
    //   uniform=<regular|civilian|none>[&name=<label>] -- writes one month of the CURRENTLY
    //   active routine's schedule (both sleep and uniform are required together since they're
    //   independent fields on the same struct; pass the month's existing value for whichever
    //   one you are not changing).
    auto squad_schedule_handler = [](const httplib::Request& req, httplib::Response& res) {
        int squad_id = -1;
        if (!query_int(req, "squad", squad_id) || !req.has_param("action")) {
            json_error(res, 400, "missing squad/action");
            return;
        }
        std::string action = req.get_param_value("action");
        std::string err;
        if (action == "set-routine") {
            int routine_idx = -1;
            if (!query_int(req, "routine", routine_idx)) {
                json_error(res, 400, "missing routine");
                return;
            }
            if (!do_squad_set_routine(squad_id, routine_idx, &err)) {
                json_error(res, 400, err);
                return;
            }
        } else if (action == "set-month") {
            int month = -1;
            if (!query_int(req, "month", month) || !req.has_param("sleep") || !req.has_param("uniform")) {
                json_error(res, 400, "missing month/sleep/uniform");
                return;
            }
            int routine_idx = -1;   // -1 -> active routine
            query_int(req, "routine", routine_idx);
            std::string sleep = req.get_param_value("sleep");
            std::string uniform = req.get_param_value("uniform");
            std::string name;
            const std::string* name_ptr = nullptr;
            if (req.has_param("name")) {
                name = req.get_param_value("name");
                if (name.size() > 64) name.resize(64);
                name_ptr = &name;
            }
            if (!do_squad_schedule_set_month(squad_id, routine_idx, month, sleep, uniform, name_ptr, &err)) {
                json_error(res, 400, err);
                return;
            }
        } else if (action == "set-month-order") {
            // month=<0-11>&order=train|none[&routine=<idx>][&min=<n>]. routine defaults to
            // the active one; order=train sets a single train order (min soldiers), none clears.
            int month = -1;
            if (!query_int(req, "month", month) || !req.has_param("order")) {
                json_error(res, 400, "missing month/order");
                return;
            }
            int routine_idx = -1, min_count = 0;
            query_int(req, "routine", routine_idx);
            query_int(req, "min", min_count);
            std::string order = req.get_param_value("order");
            if (!do_squad_schedule_set_month_order(squad_id, routine_idx, month, order, min_count, &err)) {
                json_error(res, 400, err);
                return;
            }
        } else {
            json_error(res, 400, "unknown squad-schedule action: " + action);
            return;
        }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/squad-schedule", squad_schedule_handler);
    server.Post("/squad-schedule", squad_schedule_handler);

    // POST /squad-supplies?squad=&food=<0-3>&water=<none|nowater|water|drink> (native 5.4).
    // Both optional: omit food (or send <0) / omit water to leave that field unchanged.
    auto squad_supplies_handler = [](const httplib::Request& req, httplib::Response& res) {
        int squad_id = -1;
        if (!query_int(req, "squad", squad_id)) {
            json_error(res, 400, "missing squad");
            return;
        }
        int food = -1;
        query_int(req, "food", food);
        std::string water = req.has_param("water") ? req.get_param_value("water") : std::string();
        if (food < 0 && water.empty()) {
            json_error(res, 400, "nothing to change (send food and/or water)");
            return;
        }
        std::string err;
        if (!do_squad_supplies(squad_id, food, water, &err)) {
            json_error(res, 400, err);
            return;
        }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/squad-supplies", squad_supplies_handler);
    server.Post("/squad-supplies", squad_supplies_handler);

    // Routine authoring. Routines are fort-global and served in the /squad detail's
    // "routines" list, same index as each squad's schedule.routine). create/delete keep every
    // squad's parallel schedule.routine in lockstep.
    //   POST /routine-create?name=            -> append a routine (defaults applied per name)
    //   POST /routine-rename?idx=&name=       -> rename routine idx
    //   POST /routine-delete?idx=             -> delete routine idx (refuses the last one)
    auto routine_create_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.has_param("name") ? req.get_param_value("name") : std::string("New routine");
        if (name.size() > 64) name.resize(64);
        int32_t new_id = -1;
        std::string err;
        if (!do_routine_create(new_id, name, &err)) {
            json_error(res, 400, err);
            return;
        }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true,\"id\":" + std::to_string(new_id) + "}\n");
    };
    server.Get("/routine-create", routine_create_handler);
    server.Post("/routine-create", routine_create_handler);

    auto routine_rename_handler = [](const httplib::Request& req, httplib::Response& res) {
        int idx = -1;
        if (!query_int(req, "idx", idx) || !req.has_param("name")) {
            json_error(res, 400, "missing idx/name");
            return;
        }
        std::string name = req.get_param_value("name");
        if (name.size() > 64) name.resize(64);
        std::string err;
        if (!do_routine_rename(idx, name, &err)) {
            json_error(res, 400, err);
            return;
        }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/routine-rename", routine_rename_handler);
    server.Post("/routine-rename", routine_rename_handler);

    auto routine_delete_handler = [](const httplib::Request& req, httplib::Response& res) {
        int idx = -1;
        if (!query_int(req, "idx", idx)) {
            json_error(res, 400, "missing idx");
            return;
        }
        std::string err;
        if (!do_routine_delete(idx, &err)) {
            json_error(res, 400, err);
            return;
        }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/routine-delete", routine_delete_handler);
    server.Post("/routine-delete", routine_delete_handler);

    // POST /squad-uniform?squad=&pos=&action=apply|clear.
    // apply: uniform=<fort uniform template id, from /squad detail's "uniforms" list> -- copies
    //   that template's per-category item specs onto squad.positions[pos].equipment.uniform.
    // clear: drops every uniform spec from that position (no uniform assigned).
    // Existing templates only; this route does not author custom templates.
    auto squad_uniform_handler = [](const httplib::Request& req, httplib::Response& res) {
        int squad_id = -1, pos_idx = -1;
        if (!query_int(req, "squad", squad_id) || !query_int(req, "pos", pos_idx) ||
                !req.has_param("action")) {
            json_error(res, 400, "missing squad/pos/action");
            return;
        }
        std::string action = req.get_param_value("action");
        std::string err;
        if (action == "apply") {
            int uniform_id = -1;
            if (!query_int(req, "uniform", uniform_id)) {
                json_error(res, 400, "missing uniform");
                return;
            }
            if (!do_squad_uniform_apply(squad_id, pos_idx, uniform_id, &err)) {
                json_error(res, 400, err);
                return;
            }
        } else if (action == "clear") {
            if (!do_squad_uniform_clear(squad_id, pos_idx, &err)) {
                json_error(res, 400, err);
                return;
            }
        } else {
            json_error(res, 400, "unknown squad-uniform action: " + action);
            return;
        }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/squad-uniform", squad_uniform_handler);
    server.Post("/squad-uniform", squad_uniform_handler);

    // POST /squad-equipment?squad=&pos=&action=add|update|remove&cat=&index=...
    // Native 5.5 edits one position's copied uniform specs (not the fort-wide template).
    auto squad_equipment_handler = [](const httplib::Request& req, httplib::Response& res) {
        int squad_id = -1, pos_idx = -1, cat = -1, index = -1;
        if (!query_int(req, "squad", squad_id) || !query_int(req, "pos", pos_idx) ||
                !query_int(req, "cat", cat) || !req.has_param("action")) {
            json_error(res, 400, "missing squad/pos/cat/action");
            return;
        }
        std::string action = req.get_param_value("action");
        query_int(req, "index", index);
        int subtype = -1, matclass = -1, mattype = -1, matindex = -1, color = -1, choice = 0;
        query_int(req, "subtype", subtype);
        query_int(req, "matclass", matclass);
        query_int(req, "mattype", mattype);
        query_int(req, "matindex", matindex);
        query_int(req, "color", color);
        query_int(req, "choice", choice);
        std::string err;
        if (!do_squad_equipment_change(squad_id, pos_idx, action, cat, index, subtype,
                                       matclass, mattype, matindex, color,
                                       static_cast<unsigned int>(choice), &err)) {
            json_error(res, 400, err);
            return;
        }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/squad-equipment", squad_equipment_handler);
    server.Post("/squad-equipment", squad_equipment_handler);

    // ---------------------------------------------------------------------------
    // Uniform-template and squad-ammunition authoring.
    // ---------------------------------------------------------------------------

    // GET /uniforms?player= -> all fort uniform templates (full per-category item detail) +
    // authoring catalogs (per-category subtype lists, material-class name table). Kept off the
    // /squad detail response so that response stays lean; the /squad "uniforms" list (id+name)
    // is unchanged.
    server.Get("/uniforms", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        UniformCatalog cat;
        std::string err;
        if (!uniform_catalog_snapshot(cat, &err)) {
            json_error(res, 503, err);
            return;
        }
        set_no_store_json(res, uniform_catalog_json(player, cat));
    });

    // POST /uniform-create?name=&type= (type optional, default 3=Soldier).
    auto uniform_create_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.has_param("name") ? req.get_param_value("name") : std::string("New uniform");
        if (name.size() > 64) name.resize(64);
        int type = 3; // Soldier
        query_int(req, "type", type);
        int32_t new_id = -1;
        std::string err;
        if (!do_uniform_create(new_id, name, type, &err)) {
            json_error(res, 400, err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true,\"id\":" + std::to_string(new_id) + "}\n");
    };
    server.Get("/uniform-create", uniform_create_handler);
    server.Post("/uniform-create", uniform_create_handler);

    // POST /uniform-rename?id=&name=
    auto uniform_rename_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id) || !req.has_param("name")) {
            json_error(res, 400, "missing id/name");
            return;
        }
        std::string name = req.get_param_value("name");
        if (name.size() > 64) name.resize(64);
        std::string err;
        if (!do_uniform_rename(id, name, &err)) {
            json_error(res, 400, err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/uniform-rename", uniform_rename_handler);
    server.Post("/uniform-rename", uniform_rename_handler);

    // POST /uniform-delete?id=
    auto uniform_delete_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            json_error(res, 400, "missing id");
            return;
        }
        std::string err;
        if (!do_uniform_delete(id, &err)) {
            json_error(res, 400, err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/uniform-delete", uniform_delete_handler);
    server.Post("/uniform-delete", uniform_delete_handler);

    // POST /uniform-item-add?id=&cat=&subtype=&matclass=&mattype=&matindex=&color=&choice=
    // cat: 0=body,1=head,2=pants,3=gloves,4=shoes,5=shield,6=weapon (item_type derived from cat).
    // subtype -1 = any subtype; matclass -1 = any material; choice = uniform_indiv_choice bits
    // (1=any,2=melee,4=ranged) for weapon "individual choice".
    auto uniform_item_add_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1, cat = -1;
        if (!query_int(req, "id", id) || !query_int(req, "cat", cat)) {
            json_error(res, 400, "missing id/cat");
            return;
        }
        int subtype = -1, matclass = -1, mattype = -1, matindex = -1, color = -1, choice = 0;
        query_int(req, "subtype", subtype);
        query_int(req, "matclass", matclass);
        query_int(req, "mattype", mattype);
        query_int(req, "matindex", matindex);
        query_int(req, "color", color);
        query_int(req, "choice", choice);
        std::string err;
        if (!do_uniform_item_add(id, cat, subtype, matclass, mattype, matindex, color,
                                 static_cast<unsigned int>(choice), &err)) {
            json_error(res, 400, err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/uniform-item-add", uniform_item_add_handler);
    server.Post("/uniform-item-add", uniform_item_add_handler);

    // POST /uniform-item-remove?id=&cat=&index=
    auto uniform_item_remove_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1, cat = -1, index = -1;
        if (!query_int(req, "id", id) || !query_int(req, "cat", cat) || !query_int(req, "index", index)) {
            json_error(res, 400, "missing id/cat/index");
            return;
        }
        std::string err;
        if (!do_uniform_item_remove(id, cat, index, &err)) {
            json_error(res, 400, err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/uniform-item-remove", uniform_item_remove_handler);
    server.Post("/uniform-item-remove", uniform_item_remove_handler);

    // POST /uniform-flags?id=&replaceClothing=0|1&exactMatches=0|1 (send both; the client reads
    // both checkbox states so toggling one never clobbers the other).
    auto uniform_flags_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1, replace_clothing = 0, exact_matches = 0;
        if (!query_int(req, "id", id) || !query_int(req, "replaceClothing", replace_clothing) ||
                !query_int(req, "exactMatches", exact_matches)) {
            json_error(res, 400, "missing id/replaceClothing/exactMatches");
            return;
        }
        std::string err;
        if (!do_uniform_flags(id, replace_clothing != 0, exact_matches != 0, &err)) {
            json_error(res, 400, err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/uniform-flags", uniform_flags_handler);
    server.Post("/uniform-flags", uniform_flags_handler);

    // POST /squad-ammo?squad=&action=add|update|remove|clear
    // add:    subtype=&amount=&matclass=&mattype=&matindex=&combat=0|1&training=0|1
    // update: index= + amount/matclass/mattype/matindex/combat/training (client resends all)
    // remove: index=
    // clear:  (no extra params)
    auto squad_ammo_handler = [](const httplib::Request& req, httplib::Response& res) {
        int squad_id = -1;
        if (!query_int(req, "squad", squad_id) || !req.has_param("action")) {
            json_error(res, 400, "missing squad/action");
            return;
        }
        std::string action = req.get_param_value("action");
        std::string err;
        if (action == "add") {
            int subtype = -1, amount = 0, matclass = -1, mattype = -1, matindex = -1;
            int combat = 0, training = 0;
            if (!query_int(req, "subtype", subtype)) {
                json_error(res, 400, "missing subtype");
                return;
            }
            query_int(req, "amount", amount);
            query_int(req, "matclass", matclass);
            query_int(req, "mattype", mattype);
            query_int(req, "matindex", matindex);
            query_int(req, "combat", combat);
            query_int(req, "training", training);
            if (!do_squad_ammo_add(squad_id, subtype, amount, matclass, mattype, matindex,
                                   combat != 0, training != 0, &err)) {
                json_error(res, 400, err);
                return;
            }
        } else if (action == "update") {
            int index = -1, amount = 0, matclass = -1, mattype = -1, matindex = -1;
            int combat = 0, training = 0;
            if (!query_int(req, "index", index)) {
                json_error(res, 400, "missing index");
                return;
            }
            query_int(req, "amount", amount);
            query_int(req, "matclass", matclass);
            query_int(req, "mattype", mattype);
            query_int(req, "matindex", matindex);
            query_int(req, "combat", combat);
            query_int(req, "training", training);
            if (!do_squad_ammo_update(squad_id, index, amount, matclass, mattype, matindex,
                                      combat != 0, training != 0, &err)) {
                json_error(res, 400, err);
                return;
            }
        } else if (action == "remove") {
            int index = -1;
            if (!query_int(req, "index", index)) {
                json_error(res, 400, "missing index");
                return;
            }
            if (!do_squad_ammo_remove(squad_id, index, &err)) {
                json_error(res, 400, err);
                return;
            }
        } else if (action == "clear") {
            if (!do_squad_ammo_clear(squad_id, &err)) {
                json_error(res, 400, err);
                return;
            }
        } else {
            json_error(res, 400, "unknown squad-ammo action: " + action);
            return;
        }
        notify_player_input();
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/squad-ammo", squad_ammo_handler);
    server.Post("/squad-ammo", squad_ammo_handler);
}

} // namespace dfcapture
