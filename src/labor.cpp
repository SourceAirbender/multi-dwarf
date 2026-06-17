// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios
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

#include "labor.h"

#include "Core.h"
#include "json_util.h"
#include "sdl_capture.h"

#include "modules/Units.h"

#include "df/global_objects.h"
#include "df/job_skill.h"
#include "df/plotinfost.h"
#include "df/skill_rating.h"
#include "df/unit.h"
#include "df/unit_labor.h"
#include "df/unit_labor_category.h"
#include "df/work_detail.h"
#include "df/work_detail_icon_type.h"
#include "df/work_detail_mode.h"
#include "df/world.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <sstream>

using namespace DFHack;

namespace dfcapture {
namespace {

std::recursive_mutex g_labor_mutex;

template <typename Fn>
bool run_labor_locked(Fn&& fn) {
    std::lock_guard<std::recursive_mutex> labor_lock(g_labor_mutex);
    std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
    DFHack::CoreSuspender suspend;
    return fn();
}

std::string readable_enum_key(std::string key) {
    for (char& c : key) {
        if (c == '_')
            c = ' ';
    }
    bool cap_next = true;
    for (char& c : key) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            cap_next = true;
        } else if (cap_next) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            cap_next = false;
        } else {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return key;
}

int labor_slot_count() {
    return static_cast<int>(df::enum_traits<df::unit_labor>::last_item_value) + 1;
}

bool valid_labor_index(int labor) {
    return labor >= 0 && labor < labor_slot_count() &&
           df::enum_traits<df::unit_labor>::is_valid(labor);
}

df::job_skill skill_for_labor(df::unit_labor labor) {
    if (labor == df::unit_labor::NONE)
        return df::job_skill::NONE;
    for (int i = 0; i <= df::enum_traits<df::job_skill>::last_item_value; ++i) {
        if (!df::enum_traits<df::job_skill>::is_valid(i))
            continue;
        auto skill = static_cast<df::job_skill>(i);
        if (df::enum_traits<df::job_skill>::attrs(skill).labor == labor)
            return skill;
    }
    return df::job_skill::NONE;
}

df::job_skill detail_primary_skill(df::work_detail* detail) {
    if (!detail)
        return df::job_skill::NONE;
    for (int i = 0; i <= df::enum_traits<df::unit_labor>::last_item_value; ++i) {
        if (df::enum_traits<df::unit_labor>::is_valid(i) && detail->allowed_labors[i]) {
            auto skill = skill_for_labor(static_cast<df::unit_labor>(i));
            if (skill != df::job_skill::NONE)
                return skill;
        }
    }
    return df::job_skill::NONE;
}

const char* job_skill_caption(df::job_skill skill) {
    if (skill == df::job_skill::NONE)
        return nullptr;
    return df::enum_traits<df::job_skill>::attrs(skill).caption;
}

std::string skill_label(df::unit* unit, df::job_skill skill) {
    if (skill == df::job_skill::NONE || !unit)
        return "";
    int rating = Units::getEffectiveSkill(unit, skill);
    if (rating <= df::skill_rating::Dabbling)
        return "";

    const char* skill_name = df::enum_traits<df::job_skill>::attrs(skill).caption_noun;
    if (!skill_name)
        skill_name = job_skill_caption(skill);

    const char* rating_name = nullptr;
    if (rating >= 0 && rating <= df::enum_traits<df::skill_rating>::last_item_value) {
        rating_name = df::enum_traits<df::skill_rating>::attrs(
            static_cast<df::skill_rating>(rating)).caption;
    }

    std::string label;
    if (rating_name)
        label = rating_name;
    if (skill_name) {
        if (!label.empty())
            label += " ";
        label += skill_name;
    }
    return label;
}

std::string labor_caption(df::unit_labor labor) {
    const char* caption = df::enum_traits<df::unit_labor>::attrs(labor).caption;
    if (caption && *caption)
        return caption;
    return readable_enum_key(DFHack::enum_item_key(labor));
}

bool labor_is_visible_in_picker(df::unit_labor labor) {
    if (labor == df::unit_labor::NONE)
        return false;
    std::string key = DFHack::enum_item_key(labor);
    return key.rfind("UNUSED_", 0) != 0;
}

std::string work_detail_icon_key(df::work_detail_icon_type icon) {
    int value = static_cast<int>(icon);
    if (!df::enum_traits<df::work_detail_icon_type>::is_valid(value))
        return "NONE";
    return DFHack::enum_item_key(icon);
}

std::string labor_category_key(df::unit_labor_category category) {
    int value = static_cast<int>(category);
    if (!df::enum_traits<df::unit_labor_category>::is_valid(value))
        return "Other";
    return DFHack::enum_item_key(category);
}

std::string labor_category_label(df::unit_labor_category category) {
    std::string key = labor_category_key(category);
    if (key == "Fishing")
        return "Fishing/Related";
    if (key == "Hunting")
        return "Hunting/Related";
    return readable_enum_key(key);
}

int labor_category_order(df::unit_labor_category category) {
    switch (category) {
    case df::unit_labor_category::Woodworking: return 10;
    case df::unit_labor_category::Stoneworking: return 20;
    case df::unit_labor_category::Hunting: return 30;
    case df::unit_labor_category::Healthcare: return 40;
    case df::unit_labor_category::Farming: return 50;
    case df::unit_labor_category::Fishing: return 60;
    case df::unit_labor_category::Metalsmithing: return 70;
    case df::unit_labor_category::Jewelry: return 80;
    case df::unit_labor_category::Crafts: return 90;
    case df::unit_labor_category::Engineering: return 100;
    case df::unit_labor_category::Hauling: return 110;
    case df::unit_labor_category::Other: return 120;
    default: return 130;
    }
}

df::work_detail_icon_type native_icon_for_labor(df::unit_labor labor) {
    switch (labor) {
    case df::unit_labor::MINE:
        return df::work_detail_icon_type::MINERS;
    case df::unit_labor::CUTWOOD:
        return df::work_detail_icon_type::WOODCUTTERS;
    case df::unit_labor::HUNT:
        return df::work_detail_icon_type::HUNTERS;
    case df::unit_labor::PLANT:
        return df::work_detail_icon_type::PLANTERS;
    case df::unit_labor::HERBALIST:
        return df::work_detail_icon_type::PLANT_GATHERERS;
    case df::unit_labor::FISH:
    case df::unit_labor::CLEAN_FISH:
    case df::unit_labor::DISSECT_FISH:
        return df::work_detail_icon_type::FISHERMEN;
    case df::unit_labor::STONECUTTER:
        return df::work_detail_icon_type::STONECUTTERS;
    case df::unit_labor::ENGRAVER:
        return df::work_detail_icon_type::ENGRAVERS;
    case df::unit_labor::HAUL_STONE:
    case df::unit_labor::HAUL_WOOD:
    case df::unit_labor::HAUL_BODY:
    case df::unit_labor::HAUL_FOOD:
    case df::unit_labor::HAUL_REFUSE:
    case df::unit_labor::HAUL_ITEM:
    case df::unit_labor::HAUL_FURNITURE:
    case df::unit_labor::HAUL_ANIMALS:
    case df::unit_labor::CLEAN:
    case df::unit_labor::HAUL_TRADE:
    case df::unit_labor::PULL_LEVER:
    case df::unit_labor::HAUL_WATER:
        return df::work_detail_icon_type::HAULERS;
    case df::unit_labor::DIAGNOSE:
    case df::unit_labor::SURGERY:
    case df::unit_labor::BONE_SETTING:
    case df::unit_labor::SUTURING:
    case df::unit_labor::DRESSING_WOUNDS:
    case df::unit_labor::FEED_WATER_CIVILIANS:
    case df::unit_labor::RECOVER_WOUNDED:
        return df::work_detail_icon_type::ORDERLIES;
    case df::unit_labor::SIEGEOPERATE:
        return df::work_detail_icon_type::SIEGE_OPERATORS;
    default:
        return df::work_detail_icon_type::NONE;
    }
}

df::work_detail_icon_type next_custom_work_detail_icon(const std::vector<df::work_detail*>& details) {
    int custom_count = 0;
    for (auto detail : details) {
        if (!detail)
            continue;
        int icon = static_cast<int>(detail->icon);
        if (icon >= static_cast<int>(df::work_detail_icon_type::CUSTOM_1) &&
            icon <= static_cast<int>(df::work_detail_icon_type::CUSTOM_8)) {
            ++custom_count;
        }
    }
    int icon = static_cast<int>(df::work_detail_icon_type::CUSTOM_1) + (custom_count % 8);
    return static_cast<df::work_detail_icon_type>(icon);
}

void recompute_all_citizen_professions() {
    auto world = df::global::world;
    if (!world)
        return;
    for (auto unit : world->units.active) {
        if (unit && Units::isCitizen(unit, true))
            Units::setAutomaticProfessions(unit);
    }
}

std::string clean_work_detail_name(std::string name) {
    auto first = std::find_if_not(name.begin(), name.end(), [](unsigned char c) {
        return std::isspace(c);
    });
    auto last = std::find_if_not(name.rbegin(), name.rend(), [](unsigned char c) {
        return std::isspace(c);
    }).base();
    if (first >= last)
        return "";
    name = std::string(first, last);
    if (name.size() > 64)
        name.resize(64);
    return name;
}

} // namespace

bool build_labor_state(int selected, LaborState& out, std::string* err) {
    return run_labor_locked([&]() {
        auto plotinfo = df::global::plotinfo;
        auto world = df::global::world;
        if (!plotinfo || !world) {
            if (err) *err = "plotinfo/world unavailable";
            return false;
        }

        auto& details = plotinfo->labor_info.work_details;
        for (size_t i = 0; i < details.size(); ++i) {
            auto wd = details[i];
            if (!wd)
                continue;
            LaborDetail detail;
            detail.index = static_cast<int>(i);
            detail.name = wd->name;
            detail.mode = static_cast<int>(wd->flags.bits.mode);
            if (auto caption = job_skill_caption(detail_primary_skill(wd)))
                detail.skill_name = caption;
            detail.no_modify = wd->flags.bits.no_modify;
            detail.icon_key = work_detail_icon_key(wd->icon);
            out.details.push_back(detail);
        }

        out.selected = selected;
        if (selected < 0 || selected >= static_cast<int>(details.size()) || !details[selected])
            return true;

        auto wd = details[selected];
        auto skill = detail_primary_skill(wd);
        if (auto caption = job_skill_caption(skill))
            out.selected_skill_name = caption;
        out.selected_no_modify = wd->flags.bits.no_modify;

        for (int i = 0; i < labor_slot_count(); ++i) {
            if (!valid_labor_index(i))
                continue;
            auto labor = static_cast<df::unit_labor>(i);
            if (!labor_is_visible_in_picker(labor))
                continue;
            LaborTask task;
            task.id = i;
            task.key = DFHack::enum_item_key(labor);
            task.name = labor_caption(labor);
            auto category = df::enum_traits<df::unit_labor>::attrs(labor).category;
            task.category_key = labor_category_key(category);
            task.category = labor_category_label(category);
            task.category_order = labor_category_order(category);
            auto task_skill = skill_for_labor(labor);
            if (auto caption = job_skill_caption(task_skill))
                task.skill_name = caption;
            task.icon_key = work_detail_icon_key(native_icon_for_labor(labor));
            task.allowed = wd->allowed_labors[i];
            out.tasks.push_back(std::move(task));
        }
        std::stable_sort(out.tasks.begin(), out.tasks.end(), [](const LaborTask& a, const LaborTask& b) {
            if (a.category_order != b.category_order)
                return a.category_order < b.category_order;
            return a.id < b.id;
        });

        auto& assigned = wd->assigned_units;
        for (auto unit : world->units.active) {
            if (!unit || !Units::isCitizen(unit, true))
                continue;
            LaborRow row;
            row.id = unit->id;
            row.portrait_texpos = unit->portrait_texpos;
            row.name = Units::getReadableName(unit);
            row.assigned = std::binary_search(assigned.begin(), assigned.end(), unit->id);
            row.skill = skill != df::job_skill::NONE ? Units::getEffectiveSkill(unit, skill) : 0;
            row.skill_label = skill_label(unit, skill);
            row.specialist = unit->flags4.bits.only_do_assigned_jobs;
            for (auto other : details) {
                if (!other)
                    continue;
                if (std::binary_search(other->assigned_units.begin(), other->assigned_units.end(), unit->id)) {
                    if (!row.assigned_to.empty())
                        row.assigned_to += ", ";
                    row.assigned_to += other->name;
                }
            }
            out.rows.push_back(std::move(row));
        }
        std::stable_sort(out.rows.begin(), out.rows.end(), [](const LaborRow& a, const LaborRow& b) {
            if (a.skill != b.skill)
                return a.skill > b.skill;
            return a.name < b.name;
        });
        return true;
    });
}

std::string labor_json(const LaborState& state) {
    std::ostringstream body;
    body << "{\"details\":[";
    for (size_t i = 0; i < state.details.size(); ++i) {
        if (i) body << ",";
        const auto& d = state.details[i];
        body << "{\"index\":" << d.index
             << ",\"name\":" << json_string(d.name)
             << ",\"mode\":" << d.mode
             << ",\"skillName\":" << json_string(d.skill_name)
             << ",\"iconKey\":" << json_string(d.icon_key)
             << ",\"noModify\":" << (d.no_modify ? "true" : "false") << "}";
    }
    body << "],\"selected\":" << state.selected
         << ",\"skillName\":" << json_string(state.selected_skill_name)
         << ",\"selectedNoModify\":" << (state.selected_no_modify ? "true" : "false")
         << ",\"tasks\":[";
    for (size_t i = 0; i < state.tasks.size(); ++i) {
        if (i) body << ",";
        const auto& t = state.tasks[i];
        body << "{\"id\":" << t.id
             << ",\"key\":" << json_string(t.key)
             << ",\"name\":" << json_string(t.name)
             << ",\"categoryKey\":" << json_string(t.category_key)
             << ",\"category\":" << json_string(t.category)
             << ",\"categoryOrder\":" << t.category_order
             << ",\"skillName\":" << json_string(t.skill_name)
             << ",\"iconKey\":" << json_string(t.icon_key)
             << ",\"allowed\":" << (t.allowed ? "true" : "false") << "}";
    }
    body << "],\"rows\":[";
    for (size_t i = 0; i < state.rows.size(); ++i) {
        if (i) body << ",";
        const auto& r = state.rows[i];
        body << "{\"id\":" << r.id
             << ",\"name\":" << json_string(r.name)
             << ",\"portraitTexpos\":" << r.portrait_texpos
             << ",\"assigned\":" << (r.assigned ? "true" : "false")
             << ",\"skill\":" << r.skill
             << ",\"skillLabel\":" << json_string(r.skill_label)
             << ",\"specialist\":" << (r.specialist ? "true" : "false")
             << ",\"assignedTo\":" << json_string(r.assigned_to) << "}";
    }
    body << "]}\n";
    return body.str();
}

bool labor_toggle_impl(int detail, int unit_id, bool on, std::string* err) {
    return run_labor_locked([&]() {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) {
            if (err) *err = "no plotinfo";
            return false;
        }
        auto& details = plotinfo->labor_info.work_details;
        if (detail < 0 || detail >= static_cast<int>(details.size()) || !details[detail]) {
            if (err) *err = "bad detail index";
            return false;
        }
        auto& units = details[detail]->assigned_units;
        auto it = std::lower_bound(units.begin(), units.end(), unit_id);
        bool present = it != units.end() && *it == unit_id;
        if (on && !present)
            units.insert(it, unit_id);
        else if (!on && present)
            units.erase(it);
        if (auto unit = df::unit::find(unit_id))
            Units::setAutomaticProfessions(unit);
        return true;
    });
}

bool labor_mode_impl(int detail, int mode, std::string* err) {
    return run_labor_locked([&]() {
        auto plotinfo = df::global::plotinfo;
        auto world = df::global::world;
        if (!plotinfo || !world) {
            if (err) *err = "plotinfo/world unavailable";
            return false;
        }
        auto& details = plotinfo->labor_info.work_details;
        if (detail < 0 || detail >= static_cast<int>(details.size()) || !details[detail]) {
            if (err) *err = "bad detail index";
            return false;
        }
        if (mode < 0 || mode > 3) {
            if (err) *err = "bad mode";
            return false;
        }
        details[detail]->flags.bits.mode = static_cast<df::work_detail_mode>(mode);
        recompute_all_citizen_professions();
        return true;
    });
}

bool labor_specialist_impl(int unit_id, bool on, std::string* err) {
    return run_labor_locked([&]() {
        auto unit = df::unit::find(unit_id);
        if (!unit) {
            if (err) *err = "unit not found";
            return false;
        }
        unit->flags4.bits.only_do_assigned_jobs = on;
        return true;
    });
}

bool labor_create_impl(const std::string& requested_name, int* out_index, std::string* err) {
    return run_labor_locked([&]() {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) {
            if (err) *err = "no plotinfo";
            return false;
        }
        auto& details = plotinfo->labor_info.work_details;
        auto detail = new df::work_detail();
        if (!detail) {
            if (err) *err = "could not allocate work detail";
            return false;
        }

        std::string name = clean_work_detail_name(requested_name);
        if (name.empty())
            name = "Custom Detail " + std::to_string(static_cast<int>(details.size()) + 1);
        detail->name = name;
        detail->flags.whole = 0;
        detail->flags.bits.mode = df::work_detail_mode::OnlySelectedDoesThis;
        detail->icon = next_custom_work_detail_icon(details);
        for (int i = 0; i < labor_slot_count(); ++i)
            detail->allowed_labors[i] = false;

        details.push_back(detail);
        if (out_index)
            *out_index = static_cast<int>(details.size()) - 1;
        recompute_all_citizen_professions();
        return true;
    });
}

bool labor_rename_impl(int detail, const std::string& requested_name, std::string* err) {
    return run_labor_locked([&]() {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) {
            if (err) *err = "no plotinfo";
            return false;
        }
        auto& details = plotinfo->labor_info.work_details;
        if (detail < 0 || detail >= static_cast<int>(details.size()) || !details[detail]) {
            if (err) *err = "bad detail index";
            return false;
        }
        std::string name = clean_work_detail_name(requested_name);
        if (name.empty()) {
            if (err) *err = "name cannot be empty";
            return false;
        }
        details[detail]->name = name;
        return true;
    });
}

bool labor_delete_impl(int detail, std::string* err) {
    return run_labor_locked([&]() {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) {
            if (err) *err = "no plotinfo";
            return false;
        }
        auto& details = plotinfo->labor_info.work_details;
        if (detail < 0 || detail >= static_cast<int>(details.size()) || !details[detail]) {
            if (err) *err = "bad detail index";
            return false;
        }
        if (details[detail]->flags.bits.no_modify) {
            if (err) *err = "default work details cannot be deleted";
            return false;
        }
        auto old = details[detail];
        details.erase(details.begin() + detail);
        delete old;
        recompute_all_citizen_professions();
        return true;
    });
}

bool labor_task_toggle_impl(int detail, int labor, bool on, std::string* err) {
    return run_labor_locked([&]() {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) {
            if (err) *err = "no plotinfo";
            return false;
        }
        auto& details = plotinfo->labor_info.work_details;
        if (detail < 0 || detail >= static_cast<int>(details.size()) || !details[detail]) {
            if (err) *err = "bad detail index";
            return false;
        }
        if (!valid_labor_index(labor) ||
            !labor_is_visible_in_picker(static_cast<df::unit_labor>(labor))) {
            if (err) *err = "bad labor index";
            return false;
        }
        details[detail]->allowed_labors[labor] = on;
        recompute_all_citizen_professions();
        return true;
    });
}

} // namespace dfcapture
