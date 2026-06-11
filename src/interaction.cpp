#include "interaction.h"

#include "Core.h"
#include "TileTypes.h"
#include "json_util.h"
#include "sdl_capture.h"

#include "modules/Buildings.h"
#include "modules/Items.h"
#include "modules/MapCache.h"
#include "modules/Maps.h"
#include "modules/Materials.h"
#include "modules/Units.h"
#include "modules/World.h"

#include "df/building.h"
#include "df/building_civzonest.h"
#include "df/building_type.h"
#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/item.h"
#include "df/item_actual.h"
#include "df/map_block.h"
#include "df/tiletype.h"
#include "df/unit.h"
#include "df/world.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <sstream>

using namespace DFHack;

namespace dfcapture {
namespace {

std::recursive_mutex g_interaction_mutex;

template <typename Fn>
bool run_suspended(Fn&& fn) {
    std::lock_guard<std::recursive_mutex> interaction_lock(g_interaction_mutex);
    std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
    DFHack::CoreSuspender suspend;
    return fn();
}

bool valid_map_pos(const df::coord& pos) {
    return pos.x >= 0 && pos.y >= 0 && pos.z >= 0;
}

df::coord building_center_pos(df::building* building) {
    if (!building)
        return df::coord();
    return df::coord(building->centerx, building->centery, building->z);
}

std::string readable_unit_name(df::unit* unit) {
    if (!unit)
        return "";
    std::string name = Units::getReadableName(unit);
    if (name.empty())
        name = Units::getRaceReadableName(unit);
    return name;
}

df::item* outer_container(df::item* item) {
    auto cur = item;
    for (int i = 0; cur && i < 32; ++i) {
        auto next = Items::getContainer(cur);
        if (!next || next == cur)
            break;
        cur = next;
    }
    return cur;
}

std::string item_material_name(df::item* item) {
    if (!item)
        return "";
    MaterialInfo mi;
    if (mi.decode(item->getMaterial(), item->getMaterialIndex()))
        return mi.toString();
    return "";
}

std::string item_quality_name(int16_t quality) {
    switch (quality) {
    case 1: return "Well-crafted";
    case 2: return "Finely-crafted";
    case 3: return "Superior";
    case 4: return "Exceptional";
    case 5: return "Masterful";
    default: return "";
    }
}

std::string item_wear_name(int16_t wear) {
    switch (wear) {
    case 0: return "None";
    case 1: return "Worn";
    case 2: return "Very worn";
    case 3: return "Tattered";
    default:
        if (wear > 3)
            return "Rotten";
        return "None";
    }
}

std::string item_weight_text(df::item* item) {
    if (!item)
        return "";
    if (item->flags.bits.weight_computed) {
        if (item->weight.whole > 0)
            return std::to_string(item->weight.whole);
        if (item->weight.fraction > 0)
            return "<1";
    }
    int32_t base = item->getBaseWeight();
    if (base > 0)
        return std::to_string(base);
    return "";
}

void resolve_stock_item_location(df::item* item, StockItemActionResult& result) {
    if (!item)
        return;

    auto outer = outer_container(item);
    auto building = Items::getHolderBuilding(item);
    if (!building && outer && outer != item)
        building = Items::getHolderBuilding(outer);
    auto holder = Items::getHolderUnit(item);
    if (!holder && outer && outer != item)
        holder = Items::getHolderUnit(outer);

    if (holder) {
        result.holder_unit_id = holder->id;
        result.holder_unit_name = readable_unit_name(holder);
        result.location = "With " + result.holder_unit_name;
    }

    if (auto owner = Items::getOwner(item)) {
        result.owner_unit_id = owner->id;
        result.owner_unit_name = readable_unit_name(owner);
    }

    df::coord pos = Items::getPosition(item);
    if (!valid_map_pos(pos) && outer && outer != item)
        pos = Items::getPosition(outer);
    if (!valid_map_pos(pos) && holder)
        pos = Units::getPosition(holder);
    if (!valid_map_pos(pos) && building)
        pos = building_center_pos(building);

    if (valid_map_pos(pos)) {
        result.has_map_pos = true;
        result.map_x = pos.x;
        result.map_y = pos.y;
        result.map_z = pos.z;
        if (result.location.empty()) {
            if (outer && outer != item) {
                std::string container = Items::getDescription(outer, 0, false);
                result.location = container.empty() ? "In container" : ("In " + container);
            } else if (building) {
                std::string bname = Buildings::getName(building);
                result.location = bname.empty() ? "In building" : ("In " + bname);
            } else {
                result.location = "On map";
            }
        }
    }
}

bool is_workshop_like_building(df::building* building) {
    if (!building)
        return false;
    auto type = building->getType();
    return type == df::building_type::Workshop || type == df::building_type::Furnace;
}

df::item* find_ground_item_at_tile(const df::coord& pos) {
    auto world = df::global::world;
    if (!world)
        return nullptr;
    df::item* best = nullptr;
    for (auto item : world->items.all) {
        if (!item || !item->flags.bits.on_ground)
            continue;
        if (item->pos.x == pos.x && item->pos.y == pos.y && item->pos.z == pos.z) {
            if (!best || item->id > best->id)
                best = item;
        }
    }
    return best;
}

df::unit* find_unit_near_tile(const df::coord& pos) {
    auto world = df::global::world;
    if (!world)
        return nullptr;

    df::unit* best = nullptr;
    int best_score = 9999;
    for (auto unit : world->units.active) {
        if (!unit || unit->pos.z != pos.z || Units::isDead(unit))
            continue;
        int dx = std::abs(unit->pos.x - pos.x);
        int dy = std::abs(unit->pos.y - pos.y);
        if (dx > 1 || dy > 1)
            continue;
        int score = dx + dy;
        if (score < best_score) {
            best = unit;
            best_score = score;
            if (score == 0)
                break;
        }
    }
    return best;
}

int pixel_to_tile_coord(int p, int frame, int dim) {
    if (frame <= 0 || dim <= 0)
        return 0;
    return std::max(0, std::min(dim - 1, (p * dim) / frame));
}

bool pixel_to_map_pos(const Camera& camera,
                      int px,
                      int py,
                      int frame_w,
                      int frame_h,
                      df::coord& pos,
                      int& tile_px,
                      int& tile_py,
                      std::string* err) {
    int view_w = 0;
    int view_h = 0;
    if (!effective_capture_viewport_dims(camera, view_w, view_h, err))
        return false;
    if (frame_w <= 0 || frame_h <= 0) {
        if (err) *err = "bad frame dimensions";
        return false;
    }

    tile_px = std::max(1, frame_w / view_w);
    tile_py = std::max(1, frame_h / view_h);
    int tile_x = pixel_to_tile_coord(px, frame_w, view_w);
    int tile_y = pixel_to_tile_coord(py, frame_h, view_h);
    pos = df::coord(camera.x + tile_x, camera.y + tile_y, camera.z);
    return true;
}

bool inspect_at_pixel(const Camera& camera,
                      int px,
                      int py,
                      int frame_w,
                      int frame_h,
                      InspectResult& result,
                      std::string* err) {
    df::coord pos;
    int tile_px = 0;
    int tile_py = 0;
    if (!pixel_to_map_pos(camera, px, py, frame_w, frame_h, pos, tile_px, tile_py, err))
        return false;

    result.camera = camera;
    result.px = px;
    result.py = py;
    result.tile_px = tile_px;
    result.tile_py = tile_py;
    result.map_x = pos.x;
    result.map_y = pos.y;
    result.map_z = pos.z;

    if (auto building = Buildings::findAtTile(pos)) {
        if (is_workshop_like_building(building)) {
            result.kind = "workshop";
            result.building_id = building->id;
            result.title = Buildings::getName(building);
            if (result.title.empty())
                result.title = "Workshop";
            result.lines.push_back("Position: " + std::to_string(pos.x) + "," +
                                   std::to_string(pos.y) + "," + std::to_string(pos.z));
            result.lines.push_back("Building id: " + std::to_string(building->id));
            return true;
        }
    }

    if (auto unit = find_unit_near_tile(pos)) {
        result.kind = "unit";
        result.title = readable_unit_name(unit);
        result.unit = build_unit_sheet(unit);
        result.lines.push_back("Profession: " + Units::getProfessionName(unit));
        result.lines.push_back("Creature: " + Units::getRaceReadableName(unit));
        result.lines.push_back("Position: " + std::to_string(unit->pos.x) + "," +
                               std::to_string(unit->pos.y) + "," + std::to_string(unit->pos.z));
        result.lines.push_back("Unit id: " + std::to_string(unit->id));
        return true;
    }

    if (auto building = Buildings::findAtTile(pos)) {
        result.kind = building->getType() == df::building_type::Stockpile ? "stockpile" : "building";
        result.building_id = building->id;
        result.title = Buildings::getName(building);
        if (result.title.empty())
            result.title = "Building";
        result.lines.push_back("Position: " + std::to_string(pos.x) + "," +
                               std::to_string(pos.y) + "," + std::to_string(pos.z));
        result.lines.push_back("Building id: " + std::to_string(building->id));
        return true;
    }

    if (auto item = find_ground_item_at_tile(pos)) {
        result.kind = "item";
        result.item_id = item->id;
        result.title = Items::getDescription(item, 0, true);
        if (result.title.empty())
            result.title = "Item " + std::to_string(item->id);
        result.lines.push_back("Position: " + std::to_string(pos.x) + "," +
                               std::to_string(pos.y) + "," + std::to_string(pos.z));
        return true;
    }

    std::vector<df::building_civzonest*> zones;
    if (Buildings::findCivzonesAt(&zones, pos) && !zones.empty()) {
        auto zone = zones.front();
        result.kind = "zone";
        result.building_id = zone->id;
        result.title = Buildings::getName(zone);
        if (result.title.empty())
            result.title = "Zone";
        result.lines.push_back("Position: " + std::to_string(pos.x) + "," +
                               std::to_string(pos.y) + "," + std::to_string(pos.z));
        return true;
    }

    result.kind = "tile";
    if (auto tt = Maps::getTileType(pos))
        result.title = tileName(*tt);
    else
        result.title = "Unknown tile";
    result.lines.push_back("Position: " + std::to_string(pos.x) + "," +
                           std::to_string(pos.y) + "," + std::to_string(pos.z));
    return true;
}

bool hover_at_pixel(const Camera& camera,
                    int px,
                    int py,
                    int frame_w,
                    int frame_h,
                    HoverResult& out,
                    std::string* err) {
    df::coord pos;
    int tile_px = 0;
    int tile_py = 0;
    if (!pixel_to_map_pos(camera, px, py, frame_w, frame_h, pos, tile_px, tile_py, err))
        return false;

    out.map_x = pos.x;
    out.map_y = pos.y;
    out.map_z = pos.z;

    if (auto world = df::global::world) {
        for (auto unit : world->units.active) {
            if (!unit || unit->pos.x != pos.x || unit->pos.y != pos.y || unit->pos.z != pos.z)
                continue;
            std::string name = readable_unit_name(unit);
            if (!name.empty())
                out.lines.push_back(name);
            if (out.lines.size() >= 8)
                break;
        }
    }

    if (auto building = Buildings::findAtTile(pos)) {
        std::string name = Buildings::getName(building);
        if (!name.empty())
            out.lines.push_back(name);
    }

    if (auto block = Maps::getTileBlock(pos)) {
        int shown = 0;
        for (int32_t id : block->items) {
            if (shown >= 6 || out.lines.size() >= 12)
                break;
            auto item = df::item::find(id);
            if (!item || item->pos.x != pos.x || item->pos.y != pos.y || item->pos.z != pos.z)
                continue;
            std::string desc = Items::getDescription(item, 0, false);
            if (!desc.empty()) {
                out.lines.push_back(desc);
                ++shown;
            }
        }
    }

    MapExtras::MapCache mc;
    auto mp = mc.baseMaterialAt(pos);
    MaterialInfo mi;
    if (mi.decode(mp.mat_type, mp.mat_index))
        out.material = mi.toString();
    if (out.material.empty()) {
        if (auto tt = Maps::getTileType(pos))
            out.material = tileName(*tt);
    }
    return true;
}

} // namespace

bool action_on_core_thread(const std::string& action, std::string* err) {
    return run_suspended([&]() {
        if (action == "pause") {
            World::SetPauseState(true);
            return true;
        }
        if (action == "play" || action == "resume" || action == "unpause") {
            World::SetPauseState(false);
            return true;
        }
        if (action == "toggle-pause") {
            if (!df::global::pause_state) {
                if (err) *err = "pause state unavailable";
                return false;
            }
            World::SetPauseState(!*df::global::pause_state);
            return true;
        }
        if (err) *err = "unsupported action";
        return false;
    });
}

bool stock_item_action_on_core_thread(int32_t item_id,
                                      const std::string& action,
                                      StockItemActionResult& result) {
    return run_suspended([&]() {
        auto item = df::item::find(item_id);
        if (!item) {
            result.err = "item not found";
            return false;
        }

        result.title = Items::getDescription(item, 0, false);
        if (result.title.empty())
            result.title = "Item " + std::to_string(item_id);
        result.description = Items::getReadableDescription(item);
        if (result.description.empty())
            result.description = result.title;

        resolve_stock_item_location(item, result);

        if (action == "zoom" || action == "view" || action == "follow") {
            if (result.has_map_pos) {
                int half_w = 40;
                int half_h = 25;
                if (auto gps = df::global::gps; gps && gps->main_viewport) {
                    half_w = std::max(1, gps->main_viewport->dim_x / 2);
                    half_h = std::max(1, gps->main_viewport->dim_y / 2);
                }
                result.camera = {result.map_x - half_w, result.map_y - half_h, result.map_z};
                result.has_camera = true;
            }
        } else if (action == "info") {
            // Read-only: report current state.
        } else if (action == "forbid") {
            item->flags.bits.forbid = !item->flags.bits.forbid;
        } else if (action == "dump") {
            item->flags.bits.dump = !item->flags.bits.dump;
        } else if (action == "hide") {
            item->flags.bits.hidden = !item->flags.bits.hidden;
        } else {
            result.err = "unsupported item action";
            return false;
        }

        result.forbidden = item->flags.bits.forbid;
        result.dump = item->flags.bits.dump;
        result.hidden = item->flags.bits.hidden;
        result.weight = item_weight_text(item);

        result.lines.push_back(result.description);
        result.lines.push_back("Type: " + DFHack::enum_item_key(item->getType()));
        if (auto mat = item_material_name(item); !mat.empty())
            result.lines.push_back("Material: " + mat);
        if (auto quality = item_quality_name(item->getOverallQuality()); !quality.empty())
            result.lines.push_back("Quality: " + quality);
        if (auto weight = item_weight_text(item); !weight.empty())
            result.lines.push_back("Weight: " + weight);
        if (auto actual = virtual_cast<df::item_actual>(item))
            result.lines.push_back("Wear: " + item_wear_name(actual->wear));
        if (auto container = Items::getContainer(item)) {
            std::string desc = Items::getDescription(container, 0, false);
            result.lines.push_back("Container: " +
                (desc.empty() ? ("Item " + std::to_string(container->id)) : desc));
        }
        if (!result.location.empty())
            result.lines.push_back("Location: " + result.location);
        if (result.has_map_pos) {
            result.lines.push_back("Position: " + std::to_string(result.map_x) + "," +
                                   std::to_string(result.map_y) + "," + std::to_string(result.map_z));
        } else {
            result.lines.push_back("Position: Unknown");
        }
        if (!result.owner_unit_name.empty())
            result.lines.push_back("Owner: " + result.owner_unit_name);
        result.lines.push_back(std::string("Forbidden: ") + (item->flags.bits.forbid ? "Yes" : "No"));
        result.lines.push_back(std::string("Dump: ") + (item->flags.bits.dump ? "Yes" : "No"));
        result.lines.push_back(std::string("Hidden: ") + (item->flags.bits.hidden ? "Yes" : "No"));

        std::vector<df::item*> contained;
        Items::getContainedItems(item, &contained);
        for (auto child : contained) {
            if (!child)
                continue;
            std::string desc = Items::getDescription(child, 0, false);
            result.contents.emplace_back(child->id,
                desc.empty() ? ("Item " + std::to_string(child->id)) : desc);
            if (result.contents.size() >= 40)
                break;
        }

        result.ok = true;
        return true;
    });
}

bool inspect_on_core_thread(const Camera& camera,
                            int px,
                            int py,
                            int frame_w,
                            int frame_h,
                            InspectResult& result,
                            std::string* err) {
    return run_suspended([&]() {
        return inspect_at_pixel(camera, px, py, frame_w, frame_h, result, err);
    });
}

bool hover_on_core_thread(const Camera& camera,
                          int px,
                          int py,
                          int frame_w,
                          int frame_h,
                          HoverResult& result,
                          std::string* err) {
    return run_suspended([&]() {
        return hover_at_pixel(camera, px, py, frame_w, frame_h, result, err);
    });
}

std::string inspect_json(const std::string& player, const InspectResult& result) {
    std::ostringstream body;
    body << "{"
         << "\"player\":" << json_string(player) << ","
         << "\"kind\":" << json_string(result.kind) << ","
         << "\"title\":" << json_string(result.title) << ","
         << "\"buildingId\":" << result.building_id << ","
         << "\"itemId\":" << result.item_id << ","
         << "\"camera\":{\"x\":" << result.camera.x
         << ",\"y\":" << result.camera.y
         << ",\"z\":" << result.camera.z << "},"
         << "\"tile\":{\"x\":" << result.map_x
         << ",\"y\":" << result.map_y
         << ",\"z\":" << result.map_z << "},"
         << "\"pixel\":{\"x\":" << result.px << ",\"y\":" << result.py << "},"
         << "\"tileSize\":{\"x\":" << result.tile_px << ",\"y\":" << result.tile_py << "},"
         << "\"lines\":";
    append_json_string_array(body, result.lines);
    if (result.unit.present) {
        body << ",\"unit\":";
        append_unit_sheet_json(body, result.unit);
    }
    body << "}\n";
    return body.str();
}

std::string hover_json(const std::string& player, const HoverResult& h) {
    std::ostringstream body;
    body << "{"
         << "\"player\":" << json_string(player) << ","
         << "\"tile\":{\"x\":" << h.map_x << ",\"y\":" << h.map_y << ",\"z\":" << h.map_z << "},"
         << "\"material\":" << json_string(h.material) << ","
         << "\"lines\":";
    append_json_string_array(body, h.lines);
    body << "}\n";
    return body.str();
}

std::string stock_item_action_json(int32_t item_id, const StockItemActionResult& result) {
    std::ostringstream body;
    body << "{\"ok\":true,"
         << "\"id\":" << item_id << ","
         << "\"title\":" << json_string(result.title) << ","
         << "\"description\":" << json_string(result.description) << ","
         << "\"weight\":" << json_string(result.weight) << ","
         << "\"forbidden\":" << (result.forbidden ? "true" : "false") << ","
         << "\"dump\":" << (result.dump ? "true" : "false") << ","
         << "\"hidden\":" << (result.hidden ? "true" : "false") << ","
         << "\"camera\":";
    if (result.has_camera) {
        body << "{\"x\":" << result.camera.x
             << ",\"y\":" << result.camera.y
             << ",\"z\":" << result.camera.z << "}";
    } else {
        body << "null";
    }
    body << ",\"mapPos\":";
    if (result.has_map_pos) {
        body << "{\"x\":" << result.map_x
             << ",\"y\":" << result.map_y
             << ",\"z\":" << result.map_z << "}";
    } else {
        body << "null";
    }
    body << ",\"holderUnit\":";
    if (result.holder_unit_id >= 0) {
        body << "{\"id\":" << result.holder_unit_id
             << ",\"name\":" << json_string(result.holder_unit_name) << "}";
    } else {
        body << "null";
    }
    body << ",\"ownerUnit\":";
    if (result.owner_unit_id >= 0) {
        body << "{\"id\":" << result.owner_unit_id
             << ",\"name\":" << json_string(result.owner_unit_name) << "}";
    } else {
        body << "null";
    }
    body << ",\"location\":" << json_string(result.location)
         << ",\"contents\":[";
    for (size_t i = 0; i < result.contents.size(); ++i) {
        if (i)
            body << ",";
        body << "{\"id\":" << result.contents[i].first
             << ",\"name\":" << json_string(result.contents[i].second) << "}";
    }
    body << "],\"lines\":";
    append_json_string_array(body, result.lines);
    body << "}\n";
    return body.str();
}

} // namespace dfcapture
