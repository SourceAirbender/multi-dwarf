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

#include "kitchen_panel.h"
#include "fort_stock.h"
#include "interaction.h"

#include "Core.h"
#include "json_util.h"
#include "save_barrier.h"
#include "sdl_capture.h"

#include "modules/Kitchen.h"
#include "modules/Items.h"
#include "modules/Materials.h"

#include "df/global_objects.h"
#include "df/item.h"
#include "df/item_fish_rawst.h"
#include "df/item_fishst.h"
#include "df/item_type.h"
#include "df/items_other_id.h"
#include "df/kitchen_exc_type.h"
#include "df/material.h"
#include "df/material_flags.h"
#include "df/plant_material_def.h"
#include "df/plant_material_definition_handlerst.h"
#include "df/plant_raw.h"
#include "df/plant_raw_flags.h"
#include "df/creature_raw.h"
#include "df/world.h"

#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>

using namespace DFHack;

namespace dfcapture {
namespace {

std::recursive_mutex g_kitchen_mutex;

template <typename Fn>
bool run_kitchen_locked(Fn&& fn) {
    std::lock_guard<std::recursive_mutex> kitchen_lock(g_kitchen_mutex);
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

// Per-row brew toggle. Native DF shows cook and brew controls for each
// plant). Brew exclusions use df::kitchen_exc_type.bits.Brew for the PLANT item type against the
// drink material definition used by plant_raw_flags::DRINK crops.
df::kitchen_exc_type brew_exc_type() {
    df::kitchen_exc_type type;
    type.bits.Brew = true;
    return type;
}

df::kitchen_exc_type cook_exc_type() {
    df::kitchen_exc_type type;
    type.bits.Cook = true;
    return type;
}

// The native Kitchen screen lists all cookable stock, not just
// seed-bearing plants -- meat, fish, prepared organs, cheese, etc. -- each with a stack count and
// a cook toggle. SEEDS stay in the `plants` array (whose cook cell IS the seed toggle:
// Kitchen::isSeedCookeryAllowed) so they are excluded here to avoid double-listing.
//
// PLANT is the one item class that carries native's GREY (=CANNOT) cook cell. Rope reeds, rice
// plants, and pig tails have an un-clickable cook tile and a brew tile because their STRUCTURAL
// material has no [EDIBLE_COOKED] (vanilla_plants/plant_standard.txt:
// ROPE_REED's STRUCTURAL template has no EDIBLE_COOKED; its SEED template does). Without a PLANT
// row that tri-state has nowhere to live. Its cook exclusion is keyed on the item's OWN material,
// which is the plant-cookery key, so the
// existing (type, mat, matIndex) toggle addressing drives it correctly with no new route.
// Item categories use the live item_type enum.
bool is_cookable_item_type(df::item_type t) {
    switch (t) {
        case df::item_type::MEAT:
        case df::item_type::FISH:
        case df::item_type::FISH_RAW:
        case df::item_type::EGG:
        case df::item_type::CHEESE:
        case df::item_type::PLANT:
        case df::item_type::PLANT_GROWTH:
        case df::item_type::GLOB:
            return true;
        default:
            return false;
    }
}

bool plant_brew_capable(df::plant_raw* type) {
    return type && type->flags.is_set(df::plant_raw_flags::DRINK);
}

// DF's source of truth for cook capability is the material's
// `material_flags::EDIBLE_COOKED` bit, i.e. the raws' [EDIBLE_COOKED]
// token. It is what separates native's GREY (CANNOT) from RED (RESTRICTED: possible, but the
// player put it on the kitchen exclusion list). Representative raw definitions:
//   plant_standard.txt PLANT:ROPE_REED  STRUCTURAL -> no EDIBLE_COOKED   -> native cook = GREY
//                                       SEED       -> [EDIBLE_COOKED]    -> its seed row = RED/GREEN
//   plant_standard.txt PLANT:MUSHROOM_HELMET_PLUMP STRUCTURAL -> [EDIBLE_COOKED] -> cook = GREEN
// This is a per-material read of an already-loaded raw struct: no scan, no extra suspension.
bool material_cook_capable(int16_t mat_type, int32_t mat_index) {
    DFHack::MaterialInfo info(mat_type, mat_index);
    return info.material && info.material->flags.is_set(df::material_flags::EDIBLE_COOKED);
}

// The plant raw behind an organic PLANT/PLANT_GROWTH material (MaterialInfo::Plant decodes the
// mat_index into the plant raw for us -- never index world->raws.plants.all by hand here, because
// a creature/inorganic material's index means something else entirely).
df::plant_raw* plant_of_material(int16_t mat_type, int32_t mat_index) {
    DFHack::MaterialInfo info(mat_type, mat_index);
    return info.mode == DFHack::MaterialInfo::Plant ? info.plant : nullptr;
}

// Brew capability for a stock item row.
// DF only brews the PLANT item itself, and only when the plant raw carries [DRINK] (a DRINK
// material def -> plant_raw_flags::DRINK) -- the same flag plant_brew_capable() already trusts for
// the plant rows. Growths/meat/cheese/globs are NOT brewable, and native shows them GREY.
bool item_brew_capable(df::item_type t, int16_t mat_type, int32_t mat_index) {
    if (t != df::item_type::PLANT)
        return false;
    return plant_brew_capable(plant_of_material(mat_type, mat_index));
}

// The item-art resolver needs a stable raws identity for families whose cell is species-specific.
// Numeric material indexes are world-order dependent and cannot identify a plant or fish on the
// browser. Keep this shape identical to wire_v1's established item identity extension:
//   1 = plant raw token, 2 = creature raw token.
bool kitchen_item_identity(df::world* world, df::item* item, int16_t mat_type, int32_t mat_index,
                           int& kind, std::string& token) {
    if (!world || !item)
        return false;
    int race = -1;
    if (auto* fish = strict_virtual_cast<df::item_fishst>(item))
        race = fish->race;
    else if (auto* fish_raw = strict_virtual_cast<df::item_fish_rawst>(item))
        race = fish_raw->race;
    if (race >= 0 && static_cast<size_t>(race) < world->raws.creatures.all.size()) {
        auto* creature = world->raws.creatures.all[race];
        if (creature && !creature->creature_id.empty()) {
            kind = 2;
            token = creature->creature_id;
            return true;
        }
    }
    DFHack::MaterialInfo material(mat_type, mat_index);
    if (material.isValid()) {
        if (material.plant && !material.plant->id.empty()) {
            kind = 1;
            token = material.plant->id;
            return true;
        }
        if (material.creature && !material.creature->creature_id.empty()) {
            kind = 2;
            token = material.creature->creature_id;
            return true;
        }
    }
    return false;
}

bool is_brew_allowed(df::plant_raw* type) {
    if (!plant_brew_capable(type))
        return false;
    return DFHack::Kitchen::findExclusion(brew_exc_type(), df::item_type::PLANT, -1,
        type->material_defs.type[df::plant_material_def::drink],
        type->material_defs.idx[df::plant_material_def::drink]) < 0;
}

bool do_kitchen_brew_toggle(int32_t plant_id, bool allow, std::string* err) {
    return run_kitchen_locked([&]() -> bool {
        auto world = df::global::world;
        if (!world) { if (err) *err = "world unavailable"; return false; }
        if (plant_id < 0 || plant_id >= static_cast<int32_t>(world->raws.plants.all.size())) {
            if (err) *err = "invalid plant id";
            return false;
        }
        auto* type = world->raws.plants.all[plant_id];
        if (!plant_brew_capable(type)) {
            if (err) *err = "plant cannot be brewed";
            return false;
        }
        int16_t mat_type = type->material_defs.type[df::plant_material_def::drink];
        int32_t mat_idx = type->material_defs.idx[df::plant_material_def::drink];
        if (allow)
            DFHack::Kitchen::removeExclusion(brew_exc_type(), df::item_type::PLANT, -1, mat_type, mat_idx);
        else
            DFHack::Kitchen::addExclusion(brew_exc_type(), df::item_type::PLANT, -1, mat_type, mat_idx);
        return true;
    });
}

// Kitchen prefs in DF are per-material cook/brew exclusions. The most common
// fort-management task on this screen is stopping seeds from being cooked so
// planting stock survives; the Kitchen module exposes a clean per-plant seed
// cookery toggle (allow/deny + isSeedCookeryAllowed) that we drive directly.
std::string build_kitchen_json(const std::string& player, std::string* err) {
    std::ostringstream body;
    bool ok = run_kitchen_locked([&]() -> bool {
        auto world = df::global::world;
        if (!world) { if (err) *err = "world unavailable"; return false; }
        body << "{\"player\":" << json_string(player) << ",\"plants\":[";
        bool first = true;
        int count = 0;
        for (auto plant : world->raws.plants.all) {
            if (!plant || plant->name.empty())
                continue;
            // Only plants that yield seeds are meaningful for a seed-cookery toggle.
            if (!plant->flags.is_set(df::plant_raw_flags::SEED))
                continue;
            bool seed_cook = DFHack::Kitchen::isSeedCookeryAllowed(plant->index);
            bool plant_cook = DFHack::Kitchen::isPlantCookeryAllowed(plant->index);
            bool brew_capable = plant_brew_capable(plant);
            bool brew_allowed = brew_capable && is_brew_allowed(plant);
            // The cook cell on a plant row is the seed toggle (allow/denyPlantSeedCookery,
            // exclusion key = item_type::SEEDS + the seed material),
            // so `cookCapable` here is the capability of THAT material: the seed's EDIBLE_COOKED.
            // `plantCookCapable` is the structural material's, the twin of `plantCookAllowed`.
            // Emitting both keeps every cell honest about the toggle that sits behind it.
            int16_t seed_mat = plant->material_defs.type[df::plant_material_def::seed];
            int32_t seed_idx = plant->material_defs.idx[df::plant_material_def::seed];
            int16_t basic_mat = plant->material_defs.type[df::plant_material_def::basic_mat];
            int32_t basic_idx = plant->material_defs.idx[df::plant_material_def::basic_mat];
            bool cook_capable = material_cook_capable(seed_mat, seed_idx);
            bool plant_cook_capable = material_cook_capable(basic_mat, basic_idx);
            if (!first) body << ",";
            first = false;
            body << "{\"id\":" << plant->index
                 << ",\"name\":" << json_string(plant->name)
                 << ",\"seedCookAllowed\":" << (seed_cook ? "true" : "false")
                 << ",\"plantCookAllowed\":" << (plant_cook ? "true" : "false")
                 << ",\"cookCapable\":" << (cook_capable ? "true" : "false")
                 << ",\"plantCookCapable\":" << (plant_cook_capable ? "true" : "false")
                 << ",\"brewCapable\":" << (brew_capable ? "true" : "false")
                 << ",\"brewAllowed\":" << (brew_allowed ? "true" : "false")
                 // Portrait metadata lets the client use the same tile rendering path as other items.
                 << ",\"spriteRef\":{\"itemType\":\"SEEDS\""
                 << ",\"itemSubtype\":-1"
                 << ",\"materialType\":" << seed_mat
                 << ",\"materialIndex\":" << seed_idx
                 << ",\"identKind\":1"
                 << ",\"ident\":" << json_string(plant->id) << "}"
                 << "}";
            if (++count >= 400)
                break;
        }
        // Full cookable-item list. The `plants` array above remains the seed-specific path.
        // Group in-play stock by (item_type, material) and sum
        // stack sizes -- exactly DF's own per-(type,material) kitchen row. A group is emitted only
            // when its summed count > 0, so an exclusion entry whose stock is gone never shows a
            // phantom row.
        struct ItemAgg {
            df::item_type type = df::item_type::NONE;
            int16_t mat = -1;
            int32_t mat_index = -1;
            int32_t subtype = -1;
            int64_t count = 0;
            std::string name;
            bool cook_allowed = false;
            bool cook_capable = false;
            bool brew_capable = false;
            bool brew_allowed = false;
            int ident_kind = 0;
            std::string ident;
        };
        std::map<std::tuple<int, int, int, int, std::string>, ItemAgg> groups;
        auto& in_play = world->items.other[df::items_other_id::IN_PLAY];
        for (auto item : in_play) {
            if (!is_fort_stock_item(item, FortItemPurpose::Kitchen))
                continue;
            df::item_type t = item->getType();
            if (!is_cookable_item_type(t))
                continue;
            if (item->flags.bits.dump || item->flags.bits.garbage_collect)
                continue;
            int16_t mat = item->getMaterial();
            int32_t midx = item->getMaterialIndex();
            int32_t stack = item->getStackSize();
            if (stack <= 0)
                continue;
            int ident_kind = 0;
            std::string ident;
            kitchen_item_identity(world, item, mat, midx, ident_kind, ident);
            // Species-specific fish/material rows must never collapse together merely because
            // their numeric material tuple happens to match.
            auto key = std::make_tuple(static_cast<int>(t), static_cast<int>(mat), midx,
                                       ident_kind, ident);
            auto it = groups.find(key);
            if (it == groups.end()) {
                ItemAgg agg;
                agg.type = t;
                agg.mat = mat;
                agg.mat_index = midx;
                agg.subtype = item->getSubtype();
                agg.name = item_display_name(item, 0, true);
                // Cook allowed unless this (type, material) is on the kitchen exclusion list.
                agg.cook_allowed = DFHack::Kitchen::findExclusion(cook_exc_type(), t, -1, mat, midx) < 0;
                // The two capability bits: `cook_capable` is the material's EDIBLE_COOKED;
                // `brew_capable` is the plant raw's DRINK (PLANT items only). `brew_allowed` reads
                // the SAME exclusion key the write path uses -- for PLANT that is the plant's DRINK
                // material def, NOT the item's own structural material (see do_kitchen_item_toggle).
                agg.cook_capable = material_cook_capable(mat, midx);
                auto* brew_plant = plant_of_material(mat, midx);
                agg.brew_capable = item_brew_capable(t, mat, midx);
                agg.brew_allowed = agg.brew_capable && is_brew_allowed(brew_plant);
                agg.ident_kind = ident_kind;
                agg.ident = ident;
                agg.count = stack;
                groups.emplace(key, std::move(agg));
            } else {
                it->second.count += stack;
            }
        }
        body << "],\"items\":[";
        bool item_first = true;
        int item_count = 0;
        for (auto& kv : groups) {
            const ItemAgg& a = kv.second;
            if (a.count <= 0)
                continue;
            if (!item_first) body << ",";
            item_first = false;
            body << "{\"type\":" << static_cast<int>(a.type)
                 << ",\"category\":" << json_string(DFHack::enum_item_key(a.type))
                 << ",\"mat\":" << a.mat
                 << ",\"matIndex\":" << a.mat_index
                 << ",\"name\":" << json_string(a.name)
                 << ",\"count\":" << a.count
                 << ",\"cookAllowed\":" << (a.cook_allowed ? "true" : "false")
                 << ",\"cookCapable\":" << (a.cook_capable ? "true" : "false")
                 << ",\"brewCapable\":" << (a.brew_capable ? "true" : "false")
                 << ",\"brewAllowed\":" << (a.brew_allowed ? "true" : "false")
                 << ",\"spriteRef\":{\"itemType\":" << json_string(DFHack::enum_item_key(a.type))
                 << ",\"itemSubtype\":" << a.subtype
                 << ",\"materialType\":" << a.mat
                 << ",\"materialIndex\":" << a.mat_index;
            if (a.ident_kind != 0 && !a.ident.empty())
                body << ",\"identKind\":" << a.ident_kind
                     << ",\"ident\":" << json_string(a.ident);
            body << "}"
                 << "}";
            if (++item_count >= 600)
                break;
        }
        body << "]}\n";
        return true;
    });
    if (!ok)
        return "";
    return body.str();
}

bool do_kitchen_toggle(int32_t plant_id, bool allow, std::string* err) {
    return run_kitchen_locked([&]() -> bool {
        auto world = df::global::world;
        if (!world) { if (err) *err = "world unavailable"; return false; }
        if (plant_id < 0 || plant_id >= static_cast<int32_t>(world->raws.plants.all.size())) {
            if (err) *err = "invalid plant id";
            return false;
        }
        if (allow)
            DFHack::Kitchen::allowPlantSeedCookery(plant_id);
        else
            DFHack::Kitchen::denyPlantSeedCookery(plant_id);
        return true;
    });
}

// Toggle cook/brew for a full-list item addressed by (item_type, material) rather than a
// plant id. Drives the same exclusion primitives as the plant path, just keyed on the item's
// type+material (subtype -1, matching how DF stores food exclusions).
bool do_kitchen_item_toggle(int type, int mat, int mat_index, bool allow, bool brew, std::string* err) {
    return run_kitchen_locked([&]() -> bool {
        if (type < 0) { if (err) *err = "invalid item type"; return false; }
        df::item_type it = static_cast<df::item_type>(type);
        int16_t exc_mat = static_cast<int16_t>(mat);
        int32_t exc_idx = mat_index;
        if (brew) {
            // A brew exclusion is keyed on the plant's DRINK material def, never on the item's
            // own (structural) material -- exactly as the plant path already does
            // (brew_exc_type + material_defs[plant_material_def::drink]). An item row addresses
            // itself by the material it IS MADE OF, so the brew write must be remapped or it
            // stamps an exclusion nothing ever reads. Reject anything not actually brewable
            // rather than write a key DF will not honour.
            auto* plant = plant_of_material(exc_mat, exc_idx);
            if (it != df::item_type::PLANT || !plant_brew_capable(plant)) {
                if (err) *err = "item cannot be brewed";
                return false;
            }
            exc_mat = plant->material_defs.type[df::plant_material_def::drink];
            exc_idx = plant->material_defs.idx[df::plant_material_def::drink];
        }
        df::kitchen_exc_type exc = brew ? brew_exc_type() : cook_exc_type();
        if (allow)
            DFHack::Kitchen::removeExclusion(exc, it, -1, exc_mat, exc_idx);
        else
            DFHack::Kitchen::addExclusion(exc, it, -1, exc_mat, exc_idx);
        return true;
    });
}

} // namespace

void register_kitchen_routes(httplib::Server& server) {
    // GET /kitchen -> seed-bearing plants + whether their seeds may be cooked.
    server.Get("/kitchen", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        std::string err;
        std::string json = build_kitchen_json(player, &err);
        if (json.empty()) { json_error(res, 503, err.empty() ? "kitchen unavailable" : err); return; }
        set_no_store_json(res, json);
    });

    // POST /kitchen-toggle?id=&on=1[&mode=cook|brew] -> allow(1)/deny(0) cookery (default) or
    // brewing state for a plant's seed or basic material.
    // Full-list items address by &type=&mat=&matIndex= instead of &id= (item_type+material
    // exclusion); &id= addresses the plant-specific path.
    auto toggle_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "cook";
        std::string err;
        int type = -1;
        if (query_int(req, "type", type)) {
            int on = 1;
            query_int(req, "on", on);
            int mat = -1, mat_index = -1;
            query_int(req, "mat", mat);
            query_int(req, "matIndex", mat_index);
            if (!do_kitchen_item_toggle(type, mat, mat_index, on != 0, mode == "brew", &err)) {
                json_error(res, 400, err);
                return;
            }
            set_no_store_json(res, "{\"ok\":true}\n");
            return;
        }
        int id = -1;
        if (!query_int(req, "id", id)) { json_error(res, 400, "missing id"); return; }
        int on = 1;
        query_int(req, "on", on);
        bool ok = (mode == "brew") ? do_kitchen_brew_toggle(id, on != 0, &err)
                                    : do_kitchen_toggle(id, on != 0, &err);
        if (!ok) { json_error(res, 400, err); return; }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/kitchen-toggle", toggle_handler);
    server.Post("/kitchen-toggle", toggle_handler);
}

} // namespace dfcapture
