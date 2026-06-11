#include "building_zone.h"

#include "Core.h"
#include "json_util.h"
#include "sdl_capture.h"

#include "modules/Buildings.h"
#include "modules/Items.h"
#include "modules/Maps.h"
#include "modules/Translation.h"
#include "modules/Units.h"

#include "df/abstract_building.h"
#include "df/abstract_building_type.h"
#include "df/building.h"
#include "df/building_cagest.h"
#include "df/building_civzonest.h"
#include "df/building_doorst.h"
#include "df/building_extents_type.h"
#include "df/building_hatchst.h"
#include "df/civzone_type.h"
#include "df/general_ref.h"
#include "df/general_ref_building_civzone_assignedst.h"
#include "df/general_ref_type.h"
#include "df/item.h"
#include "df/item_petst.h"
#include "df/item_verminst.h"
#include "df/map_block.h"
#include "df/tile_building_occ.h"
#include "df/unit.h"
#include "df/world.h"
#include "df/world_site.h"

#include <algorithm>
#include <mutex>
#include <sstream>

using namespace DFHack;

namespace dfcapture {
namespace {

std::recursive_mutex g_building_zone_mutex;

template <typename Fn>
bool run_building_zone_locked(Fn&& fn) {
    std::lock_guard<std::recursive_mutex> module_lock(g_building_zone_mutex);
    std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
    DFHack::CoreSuspender suspend;
    return fn();
}

bool get_door_passage_state(df::building* b, bool& forbidden, bool& closed) {
    if (auto door = virtual_cast<df::building_doorst>(b)) {
        forbidden = door->door_flags.bits.forbidden;
        closed = door->door_flags.bits.closed;
        return true;
    }
    if (auto hatch = virtual_cast<df::building_hatchst>(b)) {
        forbidden = hatch->door_flags.bits.forbidden;
        closed = hatch->door_flags.bits.closed;
        return true;
    }
    return false;
}

bool set_door_passage_forbidden(df::building* b, bool forbidden) {
    if (auto door = virtual_cast<df::building_doorst>(b)) {
        door->door_flags.bits.forbidden = forbidden;
        return true;
    }
    if (auto hatch = virtual_cast<df::building_hatchst>(b)) {
        hatch->door_flags.bits.forbidden = forbidden;
        if (auto block = Maps::getTileBlock(df::coord(hatch->centerx, hatch->centery, hatch->z))) {
            auto& occ = block->occupancy[hatch->centerx & 15][hatch->centery & 15];
            occ.bits.building = forbidden ? df::tile_building_occ::Floored
                                          : df::tile_building_occ::Dynamic;
        }
        return true;
    }
    return false;
}

bool zone_type_accepts_owner(df::civzone_type type) {
    return type == df::civzone_type::Bedroom ||
           type == df::civzone_type::DiningHall ||
           type == df::civzone_type::Office ||
           type == df::civzone_type::Tomb;
}

bool zone_type_accepts_location(df::civzone_type type) {
    return type == df::civzone_type::MeetingHall ||
           type == df::civzone_type::DiningHall ||
           type == df::civzone_type::Bedroom;
}

std::string abstract_location_type_label(df::abstract_building* loc) {
    if (!loc)
        return "";
    switch (loc->getType()) {
    case df::abstract_building_type::INN_TAVERN: return "Tavern";
    case df::abstract_building_type::TEMPLE: return "Temple";
    case df::abstract_building_type::LIBRARY: return "Library";
    case df::abstract_building_type::GUILDHALL: return "Guildhall";
    case df::abstract_building_type::HOSPITAL: return "Hospital";
    default: return DFHack::enum_item_key(loc->getType());
    }
}

df::abstract_building* find_site_location(int32_t site_id, int32_t location_id) {
    auto site = df::world_site::find(site_id);
    if (!site || location_id < 0)
        return nullptr;
    for (auto loc : site->buildings) {
        if (loc && loc->id == location_id)
            return loc;
    }
    return nullptr;
}

std::string abstract_location_name(df::abstract_building* loc) {
    if (!loc)
        return "";
    std::string name = Translation::translateName(loc->getName(), true);
    if (name.empty())
        name = abstract_location_type_label(loc);
    return name;
}

std::string archery_dir_key(const df::civzone_archery_rangest& archery) {
    if (archery.dir_x == 1 && archery.dir_y == 0)
        return "west";
    if (archery.dir_x == -1 && archery.dir_y == 0)
        return "east";
    if (archery.dir_x == 0 && archery.dir_y == 1)
        return "north";
    if (archery.dir_x == 0 && archery.dir_y == -1)
        return "south";
    return "";
}

bool zone_accepts_unit_assignments(df::building_civzonest* z) {
    return z && (z->type == df::civzone_type::Pen || z->type == df::civzone_type::Pond);
}

bool id_vector_contains(const std::vector<int32_t>& vec, int32_t id) {
    return std::find(vec.begin(), vec.end(), id) != vec.end();
}

void remove_id_from_vector(std::vector<int32_t>& vec, int32_t id) {
    vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
}

bool unit_has_valid_map_pos(df::unit* unit) {
    auto world = df::global::world;
    if (!unit || !world)
        return false;
    return unit->pos.x >= 0 && unit->pos.y >= 0 && unit->pos.z >= 0 &&
           unit->pos.x < world->map.x_count &&
           unit->pos.y < world->map.y_count &&
           unit->pos.z < world->map.z_count;
}

bool unit_contained_in_item(df::unit* unit) {
    if (!unit)
        return false;
    for (auto ref : unit->general_refs) {
        if (ref && ref->getType() == df::general_ref_type::CONTAINED_IN_ITEM)
            return true;
    }
    return false;
}

bool unit_in_built_cage(df::unit* unit) {
    auto world = df::global::world;
    if (!unit || !world)
        return false;
    for (auto building : world->buildings.all) {
        if (!building || building->getType() != df::building_type::Cage)
            continue;
        auto cage = virtual_cast<df::building_cagest>(building);
        if (cage && id_vector_contains(cage->assigned_units, unit->id))
            return true;
    }
    return false;
}

void remove_unit_from_built_cages(df::unit* unit) {
    auto world = df::global::world;
    if (!unit || !world)
        return;
    for (auto building : world->buildings.all) {
        if (!building || building->getType() != df::building_type::Cage)
            continue;
        auto cage = virtual_cast<df::building_cagest>(building);
        if (cage)
            remove_id_from_vector(cage->assigned_units, unit->id);
    }
}

int32_t unit_assigned_zone_id(df::unit* unit) {
    if (!unit)
        return -1;
    for (auto ref : unit->general_refs) {
        if (!ref || ref->getType() != df::general_ref_type::BUILDING_CIVZONE_ASSIGNED)
            continue;
        auto zone_ref = strict_virtual_cast<df::general_ref_building_civzone_assignedst>(ref);
        if (zone_ref)
            return zone_ref->building_id;
    }
    return -1;
}

int32_t item_assigned_zone_id(df::item* item) {
    if (!item)
        return -1;
    for (auto ref : item->general_refs) {
        if (!ref || ref->getType() != df::general_ref_type::BUILDING_CIVZONE_ASSIGNED)
            continue;
        auto zone_ref = strict_virtual_cast<df::general_ref_building_civzone_assignedst>(ref);
        if (zone_ref)
            return zone_ref->building_id;
    }
    return -1;
}

bool remove_unit_zone_assignments(df::unit* unit, int32_t only_zone_id = -1) {
    bool removed = false;
    if (!unit)
        return false;

    for (size_t i = 0; i < unit->general_refs.size();) {
        auto ref = unit->general_refs[i];
        if (!ref || ref->getType() != df::general_ref_type::BUILDING_CIVZONE_ASSIGNED) {
            ++i;
            continue;
        }

        auto zone_ref = strict_virtual_cast<df::general_ref_building_civzone_assignedst>(ref);
        int32_t zone_id = zone_ref ? zone_ref->building_id : -1;
        if (only_zone_id >= 0 && zone_id != only_zone_id) {
            ++i;
            continue;
        }

        unit->general_refs.erase(unit->general_refs.begin() + i);
        if (auto zone = virtual_cast<df::building_civzonest>(df::building::find(zone_id)))
            remove_id_from_vector(zone->assigned_units, unit->id);
        delete ref;
        removed = true;
    }

    if (only_zone_id >= 0) {
        if (auto zone = virtual_cast<df::building_civzonest>(df::building::find(only_zone_id)))
            remove_id_from_vector(zone->assigned_units, unit->id);
    }
    return removed;
}

bool remove_item_zone_assignments(df::item* item, int32_t only_zone_id = -1) {
    bool removed = false;
    if (!item)
        return false;

    for (size_t i = 0; i < item->general_refs.size();) {
        auto ref = item->general_refs[i];
        if (!ref || ref->getType() != df::general_ref_type::BUILDING_CIVZONE_ASSIGNED) {
            ++i;
            continue;
        }

        auto zone_ref = strict_virtual_cast<df::general_ref_building_civzone_assignedst>(ref);
        int32_t zone_id = zone_ref ? zone_ref->building_id : -1;
        if (only_zone_id >= 0 && zone_id != only_zone_id) {
            ++i;
            continue;
        }

        item->general_refs.erase(item->general_refs.begin() + i);
        if (auto zone = virtual_cast<df::building_civzonest>(df::building::find(zone_id)))
            remove_id_from_vector(zone->assigned_items, item->id);
        delete ref;
        removed = true;
    }

    if (only_zone_id >= 0) {
        if (auto zone = virtual_cast<df::building_civzonest>(df::building::find(only_zone_id)))
            remove_id_from_vector(zone->assigned_items, item->id);
    }
    return removed;
}

void mark_zone_occupants_dirty(df::building_civzonest* zone) {
    if (!zone)
        return;
    if (zone->type == df::civzone_type::Pen)
        zone->zone_settings.pen.flags.bits.check_occupants = 1;
    else if (zone->type == df::civzone_type::Pond)
        zone->zone_settings.pond.flag.bits.check_occupants = 1;
}

df::general_ref_building_civzone_assignedst* create_civzone_assignment_ref() {
    return strict_virtual_cast<df::general_ref_building_civzone_assignedst>(
        df::general_ref_building_civzone_assignedst::_identity.instantiate());
}

bool zone_item_is_candidate(df::building_civzonest* zone, df::item* item, bool assigned_here) {
    if (!zone || !item)
        return false;
    if (assigned_here)
        return true;
    if (zone->type != df::civzone_type::Pond)
        return false;
    return virtual_cast<df::item_verminst>(item) || virtual_cast<df::item_petst>(item);
}

std::string zone_item_name(df::item* item) {
    if (!item)
        return "";
    std::string desc = Items::getDescription(item, 0, false);
    if (desc.empty())
        desc = "Item " + std::to_string(item->id);
    return desc;
}

std::vector<std::string> zone_item_flags(df::item* item, bool assigned_here,
                                         bool assigned_elsewhere) {
    std::vector<std::string> flags;
    if (!item)
        return flags;
    if (assigned_here)
        flags.push_back("assigned here");
    else if (assigned_elsewhere)
        flags.push_back("assigned elsewhere");
    if (virtual_cast<df::item_petst>(item))
        flags.push_back("small pet");
    else if (virtual_cast<df::item_verminst>(item))
        flags.push_back("vermin");
    return flags;
}

bool zone_unit_is_candidate(df::unit* unit, bool assigned_here) {
    if (!unit)
        return false;
    if (assigned_here)
        return true;
    if (!Units::isActive(unit) || Units::isDead(unit) || Units::isUndead(unit))
        return false;
    if (Units::isMerchant(unit) || Units::isForest(unit))
        return false;
    if (Units::isOwnRace(unit) || Units::isOwnCiv(unit))
        return false;
    if (!unit_contained_in_item(unit) && !unit_has_valid_map_pos(unit))
        return false;
    return Units::isTame(unit) || Units::isWar(unit) || Units::isHunter(unit) ||
           unit_contained_in_item(unit) || unit_in_built_cage(unit);
}

std::vector<std::string> zone_unit_flags(df::unit* unit, bool assigned_here,
                                         bool assigned_elsewhere) {
    std::vector<std::string> flags;
    if (!unit)
        return flags;
    if (assigned_here)
        flags.push_back("assigned here");
    else if (assigned_elsewhere)
        flags.push_back("assigned elsewhere");
    if (Units::isTame(unit))
        flags.push_back("tame");
    if (Units::isWar(unit))
        flags.push_back("war");
    if (Units::isHunter(unit))
        flags.push_back("hunting");
    if (Units::isGrazer(unit))
        flags.push_back("grazer");
    if (Units::isMilkable(unit))
        flags.push_back("milkable");
    if (unit_contained_in_item(unit))
        flags.push_back("caged");
    else if (unit_in_built_cage(unit))
        flags.push_back("built cage");
    return flags;
}

struct ZoneUnitRow {
    int32_t id = -1;
    std::string kind = "unit";
    std::string name;
    std::string race;
    bool assigned = false;
    bool assigned_elsewhere = false;
    std::vector<std::string> flags;
};

struct ZoneOwnerRow {
    int32_t id = -1;
    std::string name;
    std::string profession;
    bool assigned = false;
    bool dead = false;
    int same_type_rooms = 0;
};

struct ZoneTypeMeta {
    const char* key;
    const char* label;
    int icon_x;
    int icon_y;
};

ZoneTypeMeta zone_type_meta(df::civzone_type type) {
    switch (type) {
    case df::civzone_type::MeetingHall:     return {"meeting", "Meeting Area", 5, 10};
    case df::civzone_type::Pen:             return {"pen", "Pen/Pasture", 5, 6};
    case df::civzone_type::Pond:            return {"pond", "Pit/Pond", 5, 7};
    case df::civzone_type::WaterSource:     return {"water", "Water Source", 5, 2};
    case df::civzone_type::FishingArea:     return {"fishing", "Fishing", 5, 3};
    case df::civzone_type::SandCollection:  return {"sand", "Sand", 5, 8};
    case df::civzone_type::ClayCollection:  return {"clay", "Clay", 5, 9};
    case df::civzone_type::Dump:            return {"dump", "Garbage Dump", 5, 5};
    case df::civzone_type::PlantGathering:  return {"gather", "Gather Fruit", 5, 4};
    case df::civzone_type::AnimalTraining:  return {"training", "Animal Training", 5, 12};
    case df::civzone_type::Dungeon:         return {"dungeon", "Dungeon", 6, 13};
    case df::civzone_type::Bedroom:         return {"bedroom", "Bedroom", 6, 7};
    case df::civzone_type::DiningHall:      return {"dining", "Dining Hall", 6, 8};
    case df::civzone_type::Office:          return {"office", "Office", 6, 9};
    case df::civzone_type::Dormitory:       return {"dormitory", "Dormitory", 6, 12};
    case df::civzone_type::Barracks:        return {"barracks", "Barracks", 6, 11};
    case df::civzone_type::ArcheryRange:    return {"archery", "Archery Range", 6, 10};
    case df::civzone_type::Tomb:            return {"tomb", "Tomb", 6, 14};
    case df::civzone_type::Shrine:          return {"shrine", "Shrine", 6, 4};
    case df::civzone_type::Temple:          return {"temple", "Temple", 6, 5};
    case df::civzone_type::Library:         return {"library", "Library", 6, 1};
    default:                                return {"zone", "Zone", 5, 13};
    }
}

struct ZoneSnapshotItem {
    int id = -1;
    int zone_num = 0;
    std::string key;
    std::string label;
    std::string name;
    int icon_x = 5;
    int icon_y = 13;
    bool active = false;
    int x = 0;
    int y = 0;
    int z = 0;
    int w = 0;
    int h = 0;
    std::string extents;
};

struct ZoneSnapshot {
    Camera camera;
    int viewport_w = 0;
    int viewport_h = 0;
    std::vector<ZoneSnapshotItem> zones;
};

std::string zone_extent_bitmap(df::building_civzonest* zone, int x, int y, int w, int h) {
    std::string bits;
    bits.reserve(static_cast<size_t>(std::max(0, w) * std::max(0, h)));
    bool shaped = zone && zone->room.extents && zone->isExtentShaped();
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            bool present = true;
            if (shaped) {
                int dx = (x + xx) - zone->room.x;
                int dy = (y + yy) - zone->room.y;
                present = dx >= 0 && dy >= 0 &&
                    dx < zone->room.width && dy < zone->room.height &&
                    zone->room.extents[dx + dy * zone->room.width] !=
                        df::building_extents_type::None;
            }
            bits.push_back(present ? '1' : '0');
        }
    }
    return bits;
}

bool build_zone_snapshot(const Camera& camera, ZoneSnapshot& snapshot, std::string* err) {
    auto world = df::global::world;
    if (!world) {
        if (err) *err = "world unavailable";
        return false;
    }
    snapshot.camera = camera;
    if (!effective_capture_viewport_dims(camera, snapshot.viewport_w, snapshot.viewport_h, err))
        return false;

    int vx2 = camera.x + snapshot.viewport_w;
    int vy2 = camera.y + snapshot.viewport_h;
    for (auto zone : world->buildings.other.ANY_ZONE) {
        if (!zone || zone->z != camera.z || !zone->flags.bits.exists)
            continue;

        int zx = zone->room.width > 0 ? zone->room.x : zone->x1;
        int zy = zone->room.height > 0 ? zone->room.y : zone->y1;
        int zw = zone->room.width > 0 ? zone->room.width : (zone->x2 - zone->x1 + 1);
        int zh = zone->room.height > 0 ? zone->room.height : (zone->y2 - zone->y1 + 1);
        if (zw <= 0 || zh <= 0)
            continue;
        if (zx + zw <= camera.x || zy + zh <= camera.y || zx >= vx2 || zy >= vy2)
            continue;

        ZoneTypeMeta meta = zone_type_meta(zone->type);
        ZoneSnapshotItem item;
        item.id = zone->id;
        item.zone_num = zone->zone_num;
        item.key = meta.key;
        item.label = meta.label;
        item.icon_x = meta.icon_x;
        item.icon_y = meta.icon_y;
        item.active = zone->spec_sub_flag.bits.active;
        item.x = zx;
        item.y = zy;
        item.z = zone->z;
        item.w = zw;
        item.h = zh;
        item.extents = zone_extent_bitmap(zone, zx, zy, zw, zh);
        item.name = Buildings::getName(zone);
        if (item.name.empty())
            item.name = item.label;
        snapshot.zones.push_back(std::move(item));
    }
    return true;
}

bool zone_owner_candidate(df::building_civzonest* zone, df::unit* unit, bool assigned_here) {
    if (!zone || !unit)
        return false;
    if (assigned_here)
        return true;
    if (Units::isMerchant(unit) || Units::isForest(unit) || Units::isAnimal(unit))
        return false;
    if (zone->type == df::civzone_type::Tomb)
        return Units::isOwnCiv(unit) && (Units::isDead(unit) || Units::isCitizen(unit, true));
    return Units::isActive(unit) && !Units::isDead(unit) && Units::isCitizen(unit, true);
}

int count_owned_zones_of_type(df::unit* unit, df::civzone_type type, df::building_civzonest* ignore) {
    if (!unit)
        return 0;
    int count = 0;
    for (auto b : unit->owned_buildings) {
        if (!b || b == ignore)
            continue;
        if (b->type == type)
            ++count;
    }
    return count;
}

} // namespace

bool building_info_on_core_thread(int32_t id, BuildingPanelInfo& out) {
    return run_building_zone_locked([&]() -> bool {
        auto b = df::building::find(id);
        if (!b)
            return false;
        out.id = id;
        out.exists = true;
        out.name = Buildings::getName(b);
        out.built = b->getBuildStage() >= b->getMaxBuildStage();
        out.marked = Buildings::markedForRemoval(b);
        bool suspended = false;
        bool any_job = false;
        for (auto* job : b->jobs) {
            if (!job)
                continue;
            any_job = true;
            if (job->flags.bits.suspend)
                suspended = true;
        }
        out.has_jobs = any_job;
        out.suspended = suspended;
        out.passage_control = get_door_passage_state(b, out.passage_forbidden,
                                                     out.passage_closed);
        return true;
    });
}

bool building_action_on_core_thread(int32_t id, const std::string& action, std::string* err) {
    return run_building_zone_locked([&]() -> bool {
        auto b = df::building::find(id);
        if (!b) {
            if (err) *err = "building not found";
            return false;
        }
        if (action == "cancel" || action == "remove" || action == "deconstruct")
            return Buildings::deconstruct(b);
        if (action == "suspend" || action == "resume") {
            bool suspend = action == "suspend";
            bool changed = false;
            for (auto* job : b->jobs) {
                if (!job)
                    continue;
                job->flags.bits.suspend = suspend;
                changed = true;
            }
            if (!changed && err)
                *err = "no construction job to " + action;
            return changed;
        }
        if (action == "toggle-passage" || action == "forbid-passage" ||
            action == "allow-passage") {
            bool forbidden = false;
            bool closed = false;
            if (!get_door_passage_state(b, forbidden, closed)) {
                if (err) *err = "building does not control passage";
                return false;
            }
            bool next = action == "toggle-passage" ? !forbidden : action == "forbid-passage";
            if (!set_door_passage_forbidden(b, next)) {
                if (err) *err = "failed to update passage state";
                return false;
            }
            if (df::global::world)
                df::global::world->reindex_pathfinding = true;
            return true;
        }
        if (err) *err = "unknown action: " + action;
        return false;
    });
}

std::string building_info_json(const BuildingPanelInfo& b) {
    std::ostringstream js;
    js << "{\"id\":" << b.id
       << ",\"name\":" << json_string(b.name)
       << ",\"built\":" << (b.built ? "true" : "false")
       << ",\"hasJobs\":" << (b.has_jobs ? "true" : "false")
       << ",\"suspended\":" << (b.suspended ? "true" : "false")
       << ",\"marked\":" << (b.marked ? "true" : "false")
       << ",\"passageControl\":" << (b.passage_control ? "true" : "false")
       << ",\"passageForbidden\":" << (b.passage_forbidden ? "true" : "false")
       << ",\"passageClosed\":" << (b.passage_closed ? "true" : "false") << "}";
    return js.str();
}

bool zone_info_on_core_thread(int32_t id, ZonePanelInfo& out) {
    return run_building_zone_locked([&]() -> bool {
        auto z = virtual_cast<df::building_civzonest>(df::building::find(id));
        if (!z)
            return false;
        out.id = id;
        out.exists = true;
        out.name = Buildings::getName(z);
        out.type = DFHack::enum_item_key(z->type);
        out.active = z->spec_sub_flag.bits.active;
        out.assigned_units = static_cast<int>(z->assigned_units.size());
        out.is_pit_pond = z->type == df::civzone_type::Pond;
        out.is_pen = z->type == df::civzone_type::Pen;
        out.filling_pond = z->type == df::civzone_type::Pond &&
            (z->zone_settings.pond.fill_timer > 0 || z->zone_settings.pond.flag.bits.keep_filled);
        out.can_owner = zone_type_accepts_owner(z->type);
        if (out.can_owner) {
            if (auto owner = Buildings::getOwner(z)) {
                out.owner_id = owner->id;
                out.owner_name = Units::getReadableName(owner);
                if (out.owner_name.empty())
                    out.owner_name = Units::getRaceName(owner);
            }
        }
        out.can_location = zone_type_accepts_location(z->type);
        if (out.can_location && z->site_id >= 0 && z->location_id >= 0) {
            if (auto loc = find_site_location(z->site_id, z->location_id)) {
                out.location_id = loc->id;
                out.location_name = abstract_location_name(loc);
                out.location_type = abstract_location_type_label(loc);
            }
        }
        if (z->type == df::civzone_type::PlantGathering) {
            out.is_gather = true;
            out.gather_trees = z->zone_settings.gather.flags.bits.pick_trees;
            out.gather_shrubs = z->zone_settings.gather.flags.bits.pick_shrubs;
            out.gather_fallen = z->zone_settings.gather.flags.bits.gather_fallen;
        }
        if (z->type == df::civzone_type::Tomb) {
            out.is_tomb = true;
            out.tomb_pets = !z->zone_settings.tomb.flags.bits.no_pets;
            out.tomb_citizens = !z->zone_settings.tomb.flags.bits.no_citizens;
        }
        if (z->type == df::civzone_type::ArcheryRange) {
            out.is_archery = true;
            out.archery_dir = archery_dir_key(z->zone_settings.archery);
        }
        return true;
    });
}

bool zone_action_on_core_thread(int32_t id, const std::string& action, std::string* err) {
    return run_building_zone_locked([&]() -> bool {
        auto z = virtual_cast<df::building_civzonest>(df::building::find(id));
        if (!z) {
            if (err) *err = "zone not found";
            return false;
        }
        if (action == "enable") {
            z->spec_sub_flag.bits.active = 1;
            return true;
        }
        if (action == "disable") {
            z->spec_sub_flag.bits.active = 0;
            return true;
        }
        if (action == "remove" || action == "cancel" || action == "deconstruct")
            return Buildings::deconstruct(z);
        if (action == "pond") {
            if (z->type != df::civzone_type::Pond) {
                if (err) *err = "not a pit/pond zone";
                return false;
            }
            z->zone_settings.pond.flag.bits.keep_filled = 1;
            z->zone_settings.pond.fill_timer = 1;
            return true;
        }
        if (action == "pit") {
            if (z->type != df::civzone_type::Pond) {
                if (err) *err = "not a pit/pond zone";
                return false;
            }
            z->zone_settings.pond.flag.bits.keep_filled = 0;
            z->zone_settings.pond.fill_timer = 0;
            return true;
        }
        if (z->type == df::civzone_type::PlantGathering) {
            if (action == "gather-trees-on") { z->zone_settings.gather.flags.bits.pick_trees = 1; return true; }
            if (action == "gather-trees-off") { z->zone_settings.gather.flags.bits.pick_trees = 0; return true; }
            if (action == "gather-shrubs-on") { z->zone_settings.gather.flags.bits.pick_shrubs = 1; return true; }
            if (action == "gather-shrubs-off") { z->zone_settings.gather.flags.bits.pick_shrubs = 0; return true; }
            if (action == "gather-fallen-on") { z->zone_settings.gather.flags.bits.gather_fallen = 1; return true; }
            if (action == "gather-fallen-off") { z->zone_settings.gather.flags.bits.gather_fallen = 0; return true; }
        }
        if (z->type == df::civzone_type::Tomb) {
            if (action == "tomb-pets-on") { z->zone_settings.tomb.flags.bits.no_pets = 0; return true; }
            if (action == "tomb-pets-off") { z->zone_settings.tomb.flags.bits.no_pets = 1; return true; }
            if (action == "tomb-citizens-on") { z->zone_settings.tomb.flags.bits.no_citizens = 0; return true; }
            if (action == "tomb-citizens-off") { z->zone_settings.tomb.flags.bits.no_citizens = 1; return true; }
        }
        if (z->type == df::civzone_type::ArcheryRange) {
            if (action == "archery-west") { z->zone_settings.archery.dir_x = 1; z->zone_settings.archery.dir_y = 0; return true; }
            if (action == "archery-east") { z->zone_settings.archery.dir_x = -1; z->zone_settings.archery.dir_y = 0; return true; }
            if (action == "archery-north") { z->zone_settings.archery.dir_x = 0; z->zone_settings.archery.dir_y = 1; return true; }
            if (action == "archery-south") { z->zone_settings.archery.dir_x = 0; z->zone_settings.archery.dir_y = -1; return true; }
        }
        if (err) *err = "unknown zone action: " + action;
        return false;
    });
}

std::string zone_info_json(const ZonePanelInfo& z) {
    std::ostringstream js;
    js << "{\"id\":" << z.id
       << ",\"name\":" << json_string(z.name)
       << ",\"type\":" << json_string(z.type)
       << ",\"active\":" << (z.active ? "true" : "false")
       << ",\"assignedUnits\":" << z.assigned_units
       << ",\"isPitPond\":" << (z.is_pit_pond ? "true" : "false")
       << ",\"isPen\":" << (z.is_pen ? "true" : "false")
       << ",\"fillingPond\":" << (z.filling_pond ? "true" : "false")
       << ",\"canOwner\":" << (z.can_owner ? "true" : "false")
       << ",\"owner\":{\"id\":" << z.owner_id << ",\"name\":" << json_string(z.owner_name) << "}"
       << ",\"canLocation\":" << (z.can_location ? "true" : "false")
       << ",\"location\":{\"id\":" << z.location_id
       << ",\"name\":" << json_string(z.location_name)
       << ",\"type\":" << json_string(z.location_type) << "}"
       << ",\"isGather\":" << (z.is_gather ? "true" : "false")
       << ",\"gather\":{\"trees\":" << (z.gather_trees ? "true" : "false")
       << ",\"shrubs\":" << (z.gather_shrubs ? "true" : "false")
       << ",\"fallen\":" << (z.gather_fallen ? "true" : "false") << "}"
       << ",\"isTomb\":" << (z.is_tomb ? "true" : "false")
       << ",\"tomb\":{\"pets\":" << (z.tomb_pets ? "true" : "false")
       << ",\"citizens\":" << (z.tomb_citizens ? "true" : "false") << "}"
       << ",\"isArchery\":" << (z.is_archery ? "true" : "false")
       << ",\"archery\":{\"direction\":" << json_string(z.archery_dir) << "}"
       << "}";
    return js.str();
}

std::string zone_units_json_on_core_thread(int32_t zone_id, std::string* err) {
    std::string json;
    bool ok = run_building_zone_locked([&]() -> bool {
        auto zone = virtual_cast<df::building_civzonest>(df::building::find(zone_id));
        if (!zone || !zone_accepts_unit_assignments(zone)) {
            if (err) *err = "zone does not accept unit assignments";
            return false;
        }
        auto world = df::global::world;
        if (!world) {
            if (err) *err = "world unavailable";
            return false;
        }

        std::vector<ZoneUnitRow> rows;
        rows.reserve(world->units.all.size());
        for (auto unit : world->units.all) {
            if (!unit)
                continue;
            int32_t assigned_zone = unit_assigned_zone_id(unit);
            bool assigned_here = assigned_zone == zone->id ||
                                 id_vector_contains(zone->assigned_units, unit->id);
            if (!zone_unit_is_candidate(unit, assigned_here))
                continue;
            ZoneUnitRow row;
            row.id = unit->id;
            row.name = Units::getReadableName(unit);
            if (row.name.empty())
                row.name = Units::getRaceName(unit);
            row.race = Units::getRaceName(unit);
            row.assigned = assigned_here;
            row.assigned_elsewhere = assigned_zone >= 0 && assigned_zone != zone->id;
            row.flags = zone_unit_flags(unit, row.assigned, row.assigned_elsewhere);
            rows.push_back(std::move(row));
        }

        if (zone->type == df::civzone_type::Pond) {
            auto add_item_row = [&](df::item* item) {
                if (!item)
                    return;
                int32_t assigned_zone = item_assigned_zone_id(item);
                bool assigned_here = assigned_zone == zone->id ||
                                     id_vector_contains(zone->assigned_items, item->id);
                if (!zone_item_is_candidate(zone, item, assigned_here))
                    return;
                ZoneUnitRow row;
                row.id = item->id;
                row.kind = "item";
                row.name = zone_item_name(item);
                row.race = "item";
                row.assigned = assigned_here;
                row.assigned_elsewhere = assigned_zone >= 0 && assigned_zone != zone->id;
                row.flags = zone_item_flags(item, row.assigned, row.assigned_elsewhere);
                rows.push_back(std::move(row));
            };
            for (auto item : world->items.other.VERMIN)
                add_item_row(item);
            for (auto item : world->items.other.PET)
                add_item_row(item);
        }

        std::sort(rows.begin(), rows.end(), [](const ZoneUnitRow& a, const ZoneUnitRow& b) {
            if (a.assigned != b.assigned)
                return a.assigned > b.assigned;
            if (a.assigned_elsewhere != b.assigned_elsewhere)
                return a.assigned_elsewhere > b.assigned_elsewhere;
            if (a.kind != b.kind)
                return a.kind < b.kind;
            return a.name < b.name;
        });

        std::ostringstream js;
        js << "{\"id\":" << zone->id
           << ",\"type\":" << json_string(DFHack::enum_item_key(zone->type))
           << ",\"name\":" << json_string(Buildings::getName(zone))
           << ",\"units\":[";
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            if (i) js << ",";
            js << "{\"id\":" << row.id
               << ",\"kind\":" << json_string(row.kind)
               << ",\"name\":" << json_string(row.name)
               << ",\"race\":" << json_string(row.race)
               << ",\"assigned\":" << (row.assigned ? "true" : "false")
               << ",\"assignedElsewhere\":" << (row.assigned_elsewhere ? "true" : "false")
               << ",\"flags\":";
            append_json_string_array(js, row.flags);
            js << "}";
        }
        js << "]}";
        json = js.str();
        return true;
    });
    return ok ? json : "";
}

bool zone_unit_action_on_core_thread(int32_t zone_id, int32_t unit_id, bool assign,
                                     const std::string& kind, std::string* err) {
    return run_building_zone_locked([&]() -> bool {
        auto zone = virtual_cast<df::building_civzonest>(df::building::find(zone_id));
        if (!zone || !zone_accepts_unit_assignments(zone)) {
            if (err) *err = "zone does not accept unit assignments";
            return false;
        }
        if (kind == "item") {
            auto item = df::item::find(unit_id);
            if (!item) {
                if (err) *err = "item not found";
                return false;
            }

            if (!assign) {
                remove_item_zone_assignments(item, zone->id);
                mark_zone_occupants_dirty(zone);
                return true;
            }

            bool already_here = id_vector_contains(zone->assigned_items, item->id);
            if (already_here && item_assigned_zone_id(item) == zone->id)
                return true;
            if (!zone_item_is_candidate(zone, item, false) && !already_here) {
                if (err) *err = "item is not assignable to this zone";
                return false;
            }

            remove_item_zone_assignments(item);
            auto ref = create_civzone_assignment_ref();
            if (!ref) {
                if (err) *err = "could not create civzone assignment ref";
                return false;
            }
            ref->building_id = zone->id;
            item->general_refs.push_back(ref);
            if (!id_vector_contains(zone->assigned_items, item->id))
                zone->assigned_items.push_back(item->id);
            mark_zone_occupants_dirty(zone);
            return true;
        }

        auto unit = df::unit::find(unit_id);
        if (!unit) {
            if (err) *err = "unit not found";
            return false;
        }
        if (!assign) {
            remove_unit_zone_assignments(unit, zone->id);
            mark_zone_occupants_dirty(zone);
            return true;
        }

        bool already_here = id_vector_contains(zone->assigned_units, unit->id);
        if (already_here && unit_assigned_zone_id(unit) == zone->id)
            return true;
        if (!zone_unit_is_candidate(unit, false) && !already_here) {
            if (err) *err = "unit is not assignable to this zone";
            return false;
        }

        remove_unit_zone_assignments(unit);
        remove_unit_from_built_cages(unit);

        auto ref = create_civzone_assignment_ref();
        if (!ref) {
            if (err) *err = "could not create civzone assignment ref";
            return false;
        }
        ref->building_id = zone->id;
        unit->general_refs.push_back(ref);
        if (!id_vector_contains(zone->assigned_units, unit->id))
            zone->assigned_units.push_back(unit->id);
        mark_zone_occupants_dirty(zone);
        return true;
    });
}

std::string zone_owners_json_on_core_thread(int32_t zone_id, std::string* err) {
    std::string json;
    bool ok = run_building_zone_locked([&]() -> bool {
        auto zone = virtual_cast<df::building_civzonest>(df::building::find(zone_id));
        if (!zone || !zone_type_accepts_owner(zone->type)) {
            if (err) *err = "zone does not accept owner assignment";
            return false;
        }
        auto world = df::global::world;
        if (!world) {
            if (err) *err = "world unavailable";
            return false;
        }

        std::vector<ZoneOwnerRow> rows;
        for (auto unit : world->units.all) {
            if (!unit)
                continue;
            bool assigned_here = zone->assigned_unit_id == unit->id;
            if (!zone_owner_candidate(zone, unit, assigned_here))
                continue;
            ZoneOwnerRow row;
            row.id = unit->id;
            row.name = Units::getReadableName(unit);
            if (row.name.empty())
                row.name = Units::getRaceName(unit);
            row.profession = Units::getProfessionName(unit);
            row.assigned = assigned_here;
            row.dead = Units::isDead(unit);
            row.same_type_rooms = count_owned_zones_of_type(unit, zone->type, zone);
            rows.push_back(std::move(row));
        }

        std::sort(rows.begin(), rows.end(), [](const ZoneOwnerRow& a, const ZoneOwnerRow& b) {
            if (a.assigned != b.assigned)
                return a.assigned > b.assigned;
            if (a.dead != b.dead)
                return a.dead < b.dead;
            if (a.same_type_rooms != b.same_type_rooms)
                return a.same_type_rooms < b.same_type_rooms;
            return a.name < b.name;
        });

        std::ostringstream js;
        js << "{\"id\":" << zone->id
           << ",\"type\":" << json_string(DFHack::enum_item_key(zone->type))
           << ",\"name\":" << json_string(Buildings::getName(zone))
           << ",\"ownerId\":" << zone->assigned_unit_id
           << ",\"owners\":[";
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            if (i) js << ",";
            js << "{\"id\":" << row.id
               << ",\"name\":" << json_string(row.name)
               << ",\"profession\":" << json_string(row.profession)
               << ",\"assigned\":" << (row.assigned ? "true" : "false")
               << ",\"dead\":" << (row.dead ? "true" : "false")
               << ",\"sameTypeRooms\":" << row.same_type_rooms
               << "}";
        }
        js << "]}";
        json = js.str();
        return true;
    });
    return ok ? json : "";
}

bool zone_owner_action_on_core_thread(int32_t zone_id, int32_t unit_id, std::string* err) {
    return run_building_zone_locked([&]() -> bool {
        auto zone = virtual_cast<df::building_civzonest>(df::building::find(zone_id));
        if (!zone || !zone_type_accepts_owner(zone->type)) {
            if (err) *err = "zone does not accept owner assignment";
            return false;
        }
        if (unit_id < 0)
            return Buildings::setOwner(zone, nullptr);

        auto unit = df::unit::find(unit_id);
        if (!unit) {
            if (err) *err = "unit not found";
            return false;
        }
        if (!zone_owner_candidate(zone, unit, false) && zone->assigned_unit_id != unit_id) {
            if (err) *err = "unit is not assignable to this zone";
            return false;
        }
        bool ok = Buildings::setOwner(zone, unit);
        if (ok && zone->type == df::civzone_type::Tomb) {
            zone->zone_settings.tomb.flags.bits.no_pets = 1;
            zone->zone_settings.tomb.flags.bits.no_citizens = 1;
        }
        return ok;
    });
}

std::string zones_json_on_core_thread(const std::string& player, const Camera& camera,
                                      std::string* err) {
    ZoneSnapshot snapshot;
    bool ok = run_building_zone_locked([&]() -> bool {
        return build_zone_snapshot(camera, snapshot, err);
    });
    if (!ok)
        return "";

    std::ostringstream body;
    body << "{\"player\":" << json_string(player)
         << ",\"camera\":{\"x\":" << snapshot.camera.x << ",\"y\":" << snapshot.camera.y
         << ",\"z\":" << snapshot.camera.z << "}"
         << ",\"viewport\":{\"w\":" << snapshot.viewport_w
         << ",\"h\":" << snapshot.viewport_h << "}"
         << ",\"zones\":[";
    for (size_t i = 0; i < snapshot.zones.size(); ++i) {
        const auto& z = snapshot.zones[i];
        if (i)
            body << ",";
        body << "{\"id\":" << z.id
             << ",\"zoneNum\":" << z.zone_num
             << ",\"key\":" << json_string(z.key)
             << ",\"label\":" << json_string(z.label)
             << ",\"name\":" << json_string(z.name)
             << ",\"iconX\":" << z.icon_x
             << ",\"iconY\":" << z.icon_y
             << ",\"active\":" << (z.active ? "true" : "false")
             << ",\"x\":" << z.x
             << ",\"y\":" << z.y
             << ",\"z\":" << z.z
             << ",\"w\":" << z.w
             << ",\"h\":" << z.h
             << ",\"extents\":" << json_string(z.extents)
             << "}";
    }
    body << "]}\n";
    return body.str();
}

} // namespace dfcapture
