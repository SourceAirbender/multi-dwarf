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

#include "stockpile_panel.h"

#include "Core.h"
#include "json_util.h"
#include "sdl_capture.h"

#include "modules/Buildings.h"

#include "df/building.h"
#include "df/building_stockpilest.h"
#include "df/building_type.h"
#include "df/stockpile_group_set.h"
#include "df/stockpile_settings.h"
#include "df/world.h"

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <mutex>
#include <sstream>
#include <vector>

using namespace DFHack;

namespace dfcapture {
namespace {

std::recursive_mutex g_stockpile_mutex;

template <typename Fn>
bool run_stockpile_locked(Fn&& fn) {
    std::lock_guard<std::recursive_mutex> module_lock(g_stockpile_mutex);
    std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
    DFHack::CoreSuspender suspend;
    return fn();
}

df::building_stockpilest* find_stockpile(int32_t id) {
    auto b = df::building::find(id);
    if (b && b->getType() == df::building_type::Stockpile)
        return virtual_cast<df::building_stockpilest>(b);
    return nullptr;
}

template <typename T>
bool ptr_vector_contains(const std::vector<T*>& vec, T* value) {
    return std::find(vec.begin(), vec.end(), value) != vec.end();
}

template <typename T>
void ptr_vector_add_unique(std::vector<T*>& vec, T* value) {
    if (value && !ptr_vector_contains(vec, value))
        vec.push_back(value);
}

template <typename T>
bool ptr_vector_remove_all(std::vector<T*>& vec, T* value) {
    auto old_size = vec.size();
    vec.erase(std::remove(vec.begin(), vec.end(), value), vec.end());
    return vec.size() != old_size;
}

template <typename T>
void ptr_vector_replace_all(std::vector<T*>& vec, T* old_value, T* new_value) {
    for (auto& entry : vec) {
        if (entry == old_value)
            entry = new_value;
    }
}

std::string stockpile_building_label(df::building* b) {
    if (!b)
        return "";
    std::string name = Buildings::getName(b);
    if (!name.empty())
        return name;
    if (auto sp = virtual_cast<df::building_stockpilest>(b))
        return "Stockpile #" + std::to_string(sp->stockpile_number);
    return "Building " + std::to_string(b->id);
}

const char* stockpile_target_kind(df::building* b) {
    if (!b)
        return "building";
    if (b->getType() == df::building_type::Stockpile)
        return "stockpile";
    return "workshop";
}

void append_stockpile_building_ref(std::ostringstream& out, df::building* b) {
    if (!b)
        return;
    out << "{\"id\":" << b->id
        << ",\"kind\":" << json_string(stockpile_target_kind(b))
        << ",\"name\":" << json_string(stockpile_building_label(b))
        << ",\"pos\":{\"x\":" << b->centerx << ",\"y\":" << b->centery
        << ",\"z\":" << b->z << "}}";
}

template <typename T>
void append_stockpile_ref_array(std::ostringstream& out, const std::vector<T*>& vec) {
    out << "[";
    bool first = true;
    for (auto b : vec) {
        if (!b)
            continue;
        if (!first)
            out << ",";
        first = false;
        append_stockpile_building_ref(out, static_cast<df::building*>(b));
    }
    out << "]";
}

bool is_stockpile_link_target(df::building_stockpilest* selected, df::building* candidate) {
    if (!candidate || candidate == selected)
        return false;
    if (candidate->getType() == df::building_type::Stockpile)
        return true;
    return candidate->canLinkToStockpile() && candidate->getStockpileLinks();
}

void append_stockpile_link_targets(std::ostringstream& out, df::building_stockpilest* selected) {
    out << "[";
    bool first = true;
    auto world = df::global::world;
    if (world) {
        for (auto b : world->buildings.all) {
            if (!is_stockpile_link_target(selected, b))
                continue;
            if (!first)
                out << ",";
            first = false;
            append_stockpile_building_ref(out, b);
        }
    }
    out << "]";
}

void remove_self_links(df::building_stockpilest* sp) {
    if (!sp)
        return;
    ptr_vector_remove_all(sp->links.give_to_pile, sp);
    ptr_vector_remove_all(sp->links.take_from_pile, sp);
    ptr_vector_remove_all(sp->links.give_to_workshop, static_cast<df::building*>(sp));
    ptr_vector_remove_all(sp->links.take_from_workshop, static_cast<df::building*>(sp));
}

void replace_stockpile_link_refs(df::building_stockpilest* old_sp,
                                 df::building_stockpilest* new_sp) {
    auto world = df::global::world;
    if (!world || !old_sp || !new_sp)
        return;
    for (auto b : world->buildings.all) {
        if (!b)
            continue;
        auto links = b->getStockpileLinks();
        if (!links)
            continue;
        ptr_vector_replace_all(links->give_to_pile, old_sp, new_sp);
        ptr_vector_replace_all(links->take_from_pile, old_sp, new_sp);
        ptr_vector_replace_all(links->give_to_workshop, static_cast<df::building*>(old_sp),
                               static_cast<df::building*>(new_sp));
        ptr_vector_replace_all(links->take_from_workshop, static_cast<df::building*>(old_sp),
                               static_cast<df::building*>(new_sp));
    }
    remove_self_links(new_sp);
}

int16_t clamp_storage_value(int value) {
    return static_cast<int16_t>(std::max(0, std::min(3000, value)));
}

std::string lowercase_key(std::string key) {
    for (auto& ch : key)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return key;
}

void set_all_stockpile_groups(df::stockpile_settings& settings, bool on) {
    auto& f = settings.flags.bits;
    f.animals = on;
    f.food = on;
    f.furniture = on;
    f.corpses = on;
    f.refuse = on;
    f.stone = on;
    f.ammo = on;
    f.coins = on;
    f.bars_blocks = on;
    f.gems = on;
    f.finished_goods = on;
    f.leather = on;
    f.cloth = on;
    f.wood = on;
    f.weapons = on;
    f.armor = on;
    f.sheet = on;
}

bool set_stockpile_group_flag(df::stockpile_settings& settings, const std::string& key, bool on) {
    auto& f = settings.flags.bits;
    if (key == "animals") { f.animals = on; return true; }
    if (key == "food") { f.food = on; return true; }
    if (key == "furniture") { f.furniture = on; return true; }
    if (key == "corpses") { f.corpses = on; return true; }
    if (key == "refuse") { f.refuse = on; return true; }
    if (key == "stone") { f.stone = on; return true; }
    if (key == "ammo") { f.ammo = on; return true; }
    if (key == "coins") { f.coins = on; return true; }
    if (key == "bars" || key == "bars_blocks" || key == "bars/blocks") {
        f.bars_blocks = on;
        return true;
    }
    if (key == "gems") { f.gems = on; return true; }
    if (key == "finished" || key == "finished_goods") {
        f.finished_goods = on;
        return true;
    }
    if (key == "leather") { f.leather = on; return true; }
    if (key == "cloth") { f.cloth = on; return true; }
    if (key == "wood") { f.wood = on; return true; }
    if (key == "weapons") { f.weapons = on; return true; }
    if (key == "armor") { f.armor = on; return true; }
    if (key == "sheet" || key == "sheets") { f.sheet = on; return true; }
    return false;
}

} // namespace

std::string stockpile_info_json_on_core_thread(int32_t id) {
    std::string json;
    bool ok = run_stockpile_locked([&]() -> bool {
        auto sp = find_stockpile(id);
        if (!sp)
            return false;
        const auto& f = sp->settings.flags.bits;
        auto jb = [](bool v) { return v ? "true" : "false"; };
        std::ostringstream b;
        b << "{\"ok\":true,\"id\":" << sp->id
          << ",\"name\":" << json_string(sp->name)
          << ",\"displayName\":" << json_string(Buildings::getName(sp))
          << ",\"number\":" << sp->stockpile_number
          << ",\"pos\":{\"x\":" << sp->x1 << ",\"y\":" << sp->y1 << ",\"z\":" << sp->z << "}"
          << ",\"size\":{\"w\":" << (sp->x2 - sp->x1 + 1)
          << ",\"h\":" << (sp->y2 - sp->y1 + 1) << "}"
          << ",\"linksOnly\":" << jb(sp->stockpile_flag.bits.use_links_only)
          << ",\"storage\":{\"barrels\":" << sp->storage.max_barrels
          << ",\"bins\":" << sp->storage.max_bins
          << ",\"wheelbarrows\":" << sp->storage.max_wheelbarrows << "}"
          << ",\"groups\":{\"animals\":" << jb(f.animals)
          << ",\"food\":" << jb(f.food)
          << ",\"furniture\":" << jb(f.furniture)
          << ",\"corpses\":" << jb(f.corpses)
          << ",\"refuse\":" << jb(f.refuse)
          << ",\"stone\":" << jb(f.stone)
          << ",\"ammo\":" << jb(f.ammo)
          << ",\"coins\":" << jb(f.coins)
          << ",\"bars_blocks\":" << jb(f.bars_blocks)
          << ",\"gems\":" << jb(f.gems)
          << ",\"finished_goods\":" << jb(f.finished_goods)
          << ",\"leather\":" << jb(f.leather)
          << ",\"cloth\":" << jb(f.cloth)
          << ",\"wood\":" << jb(f.wood)
          << ",\"weapons\":" << jb(f.weapons)
          << ",\"armor\":" << jb(f.armor)
          << ",\"sheet\":" << jb(f.sheet) << "}"
          << ",\"links\":{\"give\":";
        append_stockpile_ref_array(b, sp->links.give_to_pile);
        b << ",\"giveWorkshops\":";
        append_stockpile_ref_array(b, sp->links.give_to_workshop);
        b << ",\"take\":";
        append_stockpile_ref_array(b, sp->links.take_from_pile);
        b << ",\"takeWorkshops\":";
        append_stockpile_ref_array(b, sp->links.take_from_workshop);
        b << "},\"targets\":";
        append_stockpile_link_targets(b, sp);
        b << "}\n";
        json = b.str();
        return true;
    });
    return ok ? json : "";
}

bool rename_stockpile_on_core_thread(int32_t id, const std::string& name) {
    return run_stockpile_locked([&]() -> bool {
        auto sp = find_stockpile(id);
        if (sp)
            sp->name = name;
        return sp != nullptr;
    });
}

bool remove_stockpile_on_core_thread(int32_t id) {
    return run_stockpile_locked([&]() -> bool {
        auto sp = find_stockpile(id);
        return sp ? Buildings::deconstruct(sp) : false;
    });
}

bool set_stockpile_links_only_on_core_thread(int32_t id, bool on) {
    return run_stockpile_locked([&]() -> bool {
        auto sp = find_stockpile(id);
        if (sp)
            sp->stockpile_flag.bits.use_links_only = on;
        return sp != nullptr;
    });
}

bool set_stockpile_storage_on_core_thread(int32_t id, int barrels, int bins, int wheelbarrows) {
    return run_stockpile_locked([&]() -> bool {
        auto sp = find_stockpile(id);
        if (!sp)
            return false;
        if (barrels >= 0)
            sp->storage.max_barrels = clamp_storage_value(barrels);
        if (bins >= 0)
            sp->storage.max_bins = clamp_storage_value(bins);
        if (wheelbarrows >= 0)
            sp->storage.max_wheelbarrows = clamp_storage_value(wheelbarrows);
        return true;
    });
}

bool set_stockpile_link_on_core_thread(int32_t id, int32_t target_id, const std::string& mode,
                                       bool on, std::string* err) {
    return run_stockpile_locked([&]() -> bool {
        auto sp = find_stockpile(id);
        auto target = df::building::find(target_id);
        if (!sp) {
            if (err) *err = "not a stockpile";
            return false;
        }
        if (!is_stockpile_link_target(sp, target)) {
            if (err) *err = "target cannot link to stockpiles";
            return false;
        }
        bool give = mode == "give";
        bool take = mode == "take";
        if (!give && !take) {
            if (err) *err = "mode must be give or take";
            return false;
        }

        if (auto target_sp = virtual_cast<df::building_stockpilest>(target)) {
            if (give) {
                if (on) {
                    ptr_vector_add_unique(sp->links.give_to_pile, target_sp);
                    ptr_vector_add_unique(target_sp->links.take_from_pile, sp);
                } else {
                    ptr_vector_remove_all(sp->links.give_to_pile, target_sp);
                    ptr_vector_remove_all(target_sp->links.take_from_pile, sp);
                }
            } else {
                if (on) {
                    ptr_vector_add_unique(sp->links.take_from_pile, target_sp);
                    ptr_vector_add_unique(target_sp->links.give_to_pile, sp);
                } else {
                    ptr_vector_remove_all(sp->links.take_from_pile, target_sp);
                    ptr_vector_remove_all(target_sp->links.give_to_pile, sp);
                }
            }
        } else {
            auto target_links = target->getStockpileLinks();
            if (!target_links) {
                if (err) *err = "target has no stockpile link data";
                return false;
            }
            if (give) {
                if (on) {
                    ptr_vector_add_unique(sp->links.give_to_workshop, target);
                    ptr_vector_add_unique(target_links->take_from_pile, sp);
                } else {
                    ptr_vector_remove_all(sp->links.give_to_workshop, target);
                    ptr_vector_remove_all(target_links->take_from_pile, sp);
                }
            } else {
                if (on) {
                    ptr_vector_add_unique(sp->links.take_from_workshop, target);
                    ptr_vector_add_unique(target_links->give_to_pile, sp);
                } else {
                    ptr_vector_remove_all(sp->links.take_from_workshop, target);
                    ptr_vector_remove_all(target_links->give_to_pile, sp);
                }
            }
        }

        return true;
    });
}

bool set_stockpile_category_on_core_thread(int32_t id, const std::string& preset,
                                           const std::string& mode, std::string* err) {
    return run_stockpile_locked([&]() -> bool {
        auto sp = find_stockpile(id);
        if (!sp) {
            if (err) *err = "not a stockpile";
            return false;
        }

        std::string key = lowercase_key(preset.empty() ? std::string("all") : preset);
        std::string op = lowercase_key(mode.empty() ? std::string("set") : mode);
        if (op != "set" && op != "enable" && op != "disable")
            op = "set";

        if (key == "none") {
            set_all_stockpile_groups(sp->settings, false);
            return true;
        }
        if (key == "all" || key == "everything") {
            if (op == "disable")
                set_all_stockpile_groups(sp->settings, false);
            else
                set_all_stockpile_groups(sp->settings, true);
            return true;
        }

        if (op == "set")
            set_all_stockpile_groups(sp->settings, false);

        bool on = op != "disable";
        if (!set_stockpile_group_flag(sp->settings, key, on)) {
            if (err) *err = "unknown stockpile category: " + preset;
            return false;
        }
        return true;
    });
}

bool finish_stockpile_repaint_on_core_thread(int32_t old_id, int32_t new_id,
                                             int32_t& final_id, std::string* err) {
    return run_stockpile_locked([&]() -> bool {
        auto old_sp = find_stockpile(old_id);
        auto new_sp = find_stockpile(new_id);
        if (!old_sp || !new_sp) {
            if (err) *err = "old or new stockpile not found";
            return false;
        }

        const auto settings = old_sp->settings;
        const auto stockpile_flag = old_sp->stockpile_flag;
        const auto links = old_sp->links;
        const auto name = old_sp->name;
        const int32_t stockpile_number = old_sp->stockpile_number;
        const int16_t max_barrels = old_sp->storage.max_barrels;
        const int16_t max_bins = old_sp->storage.max_bins;
        const int16_t max_wheelbarrows = old_sp->storage.max_wheelbarrows;

        new_sp->settings = settings;
        new_sp->stockpile_flag = stockpile_flag;
        new_sp->links = links;
        new_sp->name = name;
        new_sp->storage.max_barrels = max_barrels;
        new_sp->storage.max_bins = max_bins;
        new_sp->storage.max_wheelbarrows = max_wheelbarrows;
        new_sp->storage.container_type.clear();
        new_sp->storage.container_item_id.clear();
        new_sp->storage.container_x.clear();
        new_sp->storage.container_y.clear();

        replace_stockpile_link_refs(old_sp, new_sp);

        old_sp->stockpile_number = -1;
        if (!Buildings::deconstruct(old_sp)) {
            if (err) *err = "old stockpile could not be removed";
            return false;
        }

        new_sp->stockpile_number = stockpile_number;
        final_id = new_sp->id;
        return true;
    });
}

} // namespace dfcapture
