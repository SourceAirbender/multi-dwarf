#include "info_panel.h"

#include "MiscUtils.h"
#include "modules/Buildings.h"
#include "modules/DFSDL.h"
#include "modules/Items.h"
#include "modules/Job.h"
#include "modules/Translation.h"
#include "modules/Units.h"
#include "modules/World.h"

#include "df/abstract_building.h"
#include "df/abstract_building_contents.h"
#include "df/abstract_building_type.h"
#include "df/artifact_claim.h"
#include "df/artifact_claim_type.h"
#include "df/artifact_record.h"
#include "df/building.h"
#include "df/building_civzonest.h"
#include "df/building_farmplotst.h"
#include "df/building_furnacest.h"
#include "df/building_siegeenginest.h"
#include "df/building_stockpilest.h"
#include "df/building_type.h"
#include "df/building_workshopst.h"
#include "df/civzone_type.h"
#include "df/entity_position.h"
#include "df/entity_position_assignment.h"
#include "df/furnace_type.h"
#include "df/gamest.h"
#include "df/global_objects.h"
#include "df/historical_figure.h"
#include "df/historical_entity.h"
#include "df/item.h"
#include "df/item_type.h"
#include "df/job.h"
#include "df/plant_raw.h"
#include "df/plotinfost.h"
#include "df/season.h"
#include "df/siegeengine_type.h"
#include "df/unit.h"
#include "df/unit_labor.h"
#include "df/workshop_type.h"
#include "df/world.h"
#include "df/world_data.h"
#include "df/world_site.h"
#include "df/written_content.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace DFHack;

namespace dfcapture {
namespace {

std::recursive_mutex g_info_panel_mutex;

// Independent browser Info panels mirror DF's premium Info screen tab names.
// Local source references:
// - dfhack-src/library/modules/Gui.cpp: add_main_interface_focus_strings()
//   exposes the active DF Info tabs/subtabs ("Pets/Livestock", "Dead/Missing",
//   "Work Details", "Standing orders", etc.).
// - dfhack-src/library/modules/Units.cpp provides the fort-control, citizen,
//   tame animal, animal, and visitor predicates used for creature bucketing.
// - dfhack-src/plugins/lua/sort/info.lua shows how DFHack tooling maps units
//   into Info-panel subsets without relying on DF's singleton UI state.
// - dfhack-src/plugins/lua/stocks.lua and df::stocks_interfacest document the
//   native Stocks screen's type_list/storeamount/badamount model. We read those
//   values when DF has populated them, then fall back to world->items.all so the
//   browser screen remains independent of DF's singleton Stocks UI.

std::string json_escape(const std::string& raw) {
    std::ostringstream out;
    for (unsigned char c : DF2UTF(raw)) {
        switch (c) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                out << "\\u" << std::hex << std::uppercase
                    << static_cast<int>(c) << std::dec << std::nouppercase;
            } else {
                out << static_cast<char>(c);
            }
        }
    }
    return out.str();
}

std::string json_string(const std::string& raw) {
    return "\"" + json_escape(raw) + "\"";
}

void append_string_array(std::ostringstream& body, const std::vector<std::string>& values) {
    body << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) body << ",";
        body << json_string(values[i]);
    }
    body << "]";
}

void append_tabs(std::ostringstream& body, const std::vector<InfoTab>& tabs) {
    body << "[";
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (i) body << ",";
        body << "{\"id\":" << json_string(tabs[i].id)
             << ",\"label\":" << json_string(tabs[i].label) << "}";
    }
    body << "]";
}

std::string pretty_key(std::string key) {
    for (char& ch : key) {
        if (ch == '_')
            ch = ' ';
        else
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (!key.empty())
        key[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])));
    return key;
}

std::vector<InfoTab> primary_tabs() {
    return {
        {"nobles", "Nobles and administrators"},
        {"objects", "Objects"},
        {"justice", "Justice"},
    };
}

std::vector<InfoTab> section_tabs() {
    return {
        {"creatures", "Creatures"},
        {"tasks", "Tasks"},
        {"places", "Places"},
        {"labor", "Labor"},
        {"workorders", "Work orders"},
    };
}

std::vector<InfoTab> stocks_section_tabs() {
    return {
        {"stocks", "Stocks"},
    };
}

std::vector<InfoTab> detail_tabs_for(const std::string& section) {
    if (section == "creatures") {
        return {
            {"residents", "Residents"},
            {"pets", "Pets/Livestock"},
            {"other", "Other"},
            {"dead", "Dead/Missing"},
        };
    }
    if (section == "places") {
        return {
            {"zones", "Zones"},
            {"locations", "Locations"},
            {"stockpiles", "Stockpiles"},
            {"workshops", "Workshops"},
            {"farmplots", "Farm plots"},
            {"siege", "Siege engines"},
        };
    }
    if (section == "labor") {
        return {
            {"workdetails", "Work Details"},
            {"standing", "Standing orders"},
            {"kitchen", "Kitchen"},
            {"stone", "Stone use"},
        };
    }
    if (section == "objects") {
        return {
            {"artifacts", "Artifacts"},
            {"symbols", "Symbols"},
            {"named", "Named objects"},
            {"written", "Written content"},
        };
    }
    return {};
}

std::string default_section_for_panel(const std::string& panel) {
    if (panel == "stocks")
        return "stocks";
    if (panel == "labor")
        return "labor";
    if (panel == "locations")
        return "places";
    if (panel == "orders")
        return "tasks";
    if (panel == "workorders")
        return "workorders";
    if (panel == "nobles")
        return "nobles";
    if (panel == "objects")
        return "objects";
    if (panel == "justice")
        return "justice";
    return "creatures";
}

std::string default_detail_for_section(const std::string& section) {
    auto tabs = detail_tabs_for(section);
    return tabs.empty() ? "" : tabs.front().id;
}

std::string job_name(df::unit* unit) {
    if (!unit || !unit->job.current_job)
        return "No job";
    std::string name = Job::getName(unit->job.current_job);
    return name.empty() ? "Working" : name;
}

std::string unit_display_name(df::unit* unit) {
    std::string name = Units::getReadableName(unit);
    if (name.empty())
        name = Units::getRaceReadableName(unit);
    return name.empty() ? ("Unit " + std::to_string(unit->id)) : name;
}

InfoRow row_for_unit(df::unit* unit) {
    InfoRow row;
    row.unit_id = unit->id;
    row.portrait_texpos = unit->portrait_texpos;
    row.name = unit_display_name(unit);
    row.category = Units::getRaceReadableName(unit);
    row.profession = Units::getProfessionName(unit);
    row.job = job_name(unit);
    row.status = Units::isAlive(unit) ? "" : "Dead";
    if (Units::isTame(unit))
        row.badges.push_back("Domesticated");
    if (Units::isGrazer(unit))
        row.badges.push_back("Grazer: requires pasture");
    if (Units::isPet(unit))
        row.badges.push_back("Pet");
    if (Units::isMarkedForSlaughter(unit))
        row.badges.push_back("Marked for slaughter");
    return row;
}

bool is_resident(df::unit* unit) {
    return unit && Units::isCitizen(unit, true);
}

bool is_pet_or_livestock(df::unit* unit) {
    return unit && Units::isAlive(unit) && !is_resident(unit) &&
           Units::isAnimal(unit) &&
           (Units::isFortControlled(unit) || Units::isTame(unit) || Units::isPet(unit));
}

bool is_other_creature(df::unit* unit) {
    return unit && Units::isAlive(unit) && !is_resident(unit) &&
           !is_pet_or_livestock(unit);
}

bool is_dead_or_missing(df::unit* unit) {
    return unit && Units::isOwnGroup(unit) && !is_resident(unit) && !Units::isAlive(unit);
}

bool is_counted_stock_item(df::item* item) {
    if (!item || !item->isActual())
        return false;
    auto& f = item->flags.bits;
    return !f.removed && !f.garbage_collect && !f.hidden && !f.hostile &&
           !f.trader && !f.forbid && !f.rotten;
}

int item_type_index(df::item_type type) {
    int idx = static_cast<int>(type);
    if (idx < 0 || idx > df::enum_traits<df::item_type>::last_item_value)
        return -1;
    return idx;
}

int item_stack_count(df::item* item) {
    if (!item)
        return 0;
    return std::max(1, item->getStackSize());
}

std::string format_stock_count(int count) {
    if (count <= 0)
        return "None";
    if (count < 10)
        return std::to_string(count);
    int rounded = ((count + 5) / 10) * 10;
    return "~" + std::to_string(std::max(10, rounded));
}

struct StockCategory {
    df::item_type type;
    const char* label;
};

std::vector<StockCategory> stock_categories() {
    using namespace df::enums::item_type;
    return {
        {AMMO, "Ammunition"},
        {AMULET, "Amulets"},
        {ANIMALTRAP, "Animal traps"},
        {ANVIL, "Anvils"},
        {ARMOR, "Armor"},
        {ARMORSTAND, "Armor stands"},
        {BACKPACK, "Backpacks"},
        {BAG, "Bags"},
        {BALLISTAARROWHEAD, "Ballista arrow heads"},
        {BALLISTAPARTS, "Ballista parts"},
        {BARREL, "Barrels"},
        {BAR, "Bars"},
        {BED, "Beds"},
        {BIN, "Bins"},
        {BLOCKS, "Blocks"},
        {CORPSEPIECE, "Body parts"},
        {BOLT_THROWER_PARTS, "Bolt thrower parts"},
        {BOULDER, "Boulders"},
        {BOOK, "Books"},
        {BOX, "Boxes"},
        {BRACELET, "Bracelets"},
        {BRANCH, "Branches"},
        {BUCKET, "Buckets"},
        {CABINET, "Cabinets"},
        {CAGE, "Cages"},
        {CATAPULTPARTS, "Catapult parts"},
        {CHAIN, "Chains"},
        {CHAIR, "Chairs"},
        {CHEESE, "Cheese"},
        {CLOTH, "Cloth"},
        {COFFIN, "Coffins"},
        {COIN, "Coins"},
        {CORPSE, "Corpses"},
        {CROWN, "Crowns"},
        {CRUTCH, "Crutches"},
        {DOOR, "Doors"},
        {DRINK, "Drinks"},
        {EARRING, "Earrings"},
        {EGG, "Eggs"},
        {FISH, "Fish"},
        {FISH_RAW, "Raw fish"},
        {FLASK, "Flasks"},
        {FLOODGATE, "Floodgates"},
        {FIGURINE, "Figurines"},
        {FOOD, "Prepared meals"},
        {GEM, "Large gems"},
        {GLOVES, "Gloves"},
        {GOBLET, "Goblets"},
        {GLOB, "Globs"},
        {GRATE, "Grates"},
        {HATCH_COVER, "Hatch covers"},
        {HELM, "Helms"},
        {INSTRUMENT, "Instruments"},
        {SKIN_TANNED, "Leather"},
        {LIQUID_MISC, "Liquids"},
        {MEAT, "Meat"},
        {MILLSTONE, "Millstones"},
        {ORTHOPEDIC_CAST, "Casts"},
        {PANTS, "Pants"},
        {PET, "Vermin pets"},
        {PIPE_SECTION, "Pipe sections"},
        {PLANT, "Plants"},
        {PLANT_GROWTH, "Plant growths"},
        {POWDER_MISC, "Powders"},
        {QUIVER, "Quivers"},
        {QUERN, "Querns"},
        {REMAINS, "Remains"},
        {RING, "Rings"},
        {ROCK, "Small rocks"},
        {ROUGH, "Rough gems"},
        {SCEPTER, "Scepters"},
        {SEEDS, "Seeds"},
        {SHEET, "Sheets"},
        {SHIELD, "Shields"},
        {SHOES, "Shoes"},
        {SIEGEAMMO, "Siege ammunition"},
        {SLAB, "Slabs"},
        {SMALLGEM, "Cut gems"},
        {SPLINT, "Splints"},
        {STATUE, "Statues"},
        {TABLE, "Tables"},
        {THREAD, "Thread"},
        {TOOL, "Tools"},
        {TOTEM, "Totems"},
        {TOY, "Toys"},
        {TRACTION_BENCH, "Traction benches"},
        {TRAPCOMP, "Trap components"},
        {TRAPPARTS, "Mechanisms"},
        {VERMIN, "Vermin"},
        {WEAPON, "Weapons"},
        {WEAPONRACK, "Weapon racks"},
        {WINDOW, "Windows"},
        {WOOD, "Wood"},
    };
}

std::string stock_category_key(df::item_type type) {
    return enum_item_key(type);
}

bool stock_category_for_key(const std::string& key, StockCategory* out) {
    for (const auto& category : stock_categories()) {
        if (stock_category_key(category.type) == key) {
            if (out)
                *out = category;
            return true;
        }
    }
    return false;
}

std::vector<df::unit*> active_units() {
    if (!df::global::world)
        return {};
    return df::global::world->units.active;
}

std::vector<int32_t> scan_stock_counts_from_world() {
    std::vector<int32_t> counts(df::enum_traits<df::item_type>::last_item_value + 1, 0);
    if (!df::global::world)
        return counts;
    for (auto item : df::global::world->items.all) {
        if (!is_counted_stock_item(item))
            continue;
        int idx = item_type_index(item->getType());
        if (idx >= 0)
            counts[idx] += item_stack_count(item);
    }
    return counts;
}

std::vector<int32_t> stock_counts() {
    auto counts = scan_stock_counts_from_world();
    auto game = df::global::game;
    if (!game)
        return counts;

    auto& stocks = game->main_interface.stocks;
    bool has_native_counts =
        std::any_of(stocks.storeamount.begin(), stocks.storeamount.end(), [](int32_t v) { return v != 0; }) ||
        std::any_of(stocks.badamount.begin(), stocks.badamount.end(), [](int32_t v) { return v != 0; });
    if (!has_native_counts)
        return counts;

    for (size_t idx = 0; idx < counts.size(); ++idx) {
        int32_t good = idx < stocks.storeamount.size() ? stocks.storeamount[idx] : 0;
        int32_t bad = idx < stocks.badamount.size() ? stocks.badamount[idx] : 0;
        counts[idx] = std::max<int32_t>(0, good + bad);
    }
    return counts;
}

std::string stock_item_name(df::item* item) {
    if (!item)
        return "";
    std::string desc = Items::getDescription(item, 0, false);
    if (desc.empty())
        desc = enum_item_key(item->getType()) + " #" + std::to_string(item->id);
    return desc;
}

void add_stock_items_for_type(InfoPanel& panel, df::item_type type) {
    if (!df::global::world)
        return;
    for (auto item : df::global::world->items.all) {
        if (!is_counted_stock_item(item) || item->getType() != type)
            continue;
        StockItemRow row;
        row.item_id = item->id;
        row.count = item_stack_count(item);
        row.name = stock_item_name(item);
        row.status = row.count > 1 ? ("[" + std::to_string(row.count) + "]") : "";
        row.subtitle = enum_item_key(item->getType());
        panel.stock_items.push_back(row);
    }
    std::sort(panel.stock_items.begin(), panel.stock_items.end(), [](const StockItemRow& a, const StockItemRow& b) {
        if (a.name != b.name)
            return a.name < b.name;
        return a.item_id < b.item_id;
    });
}

void add_unit_rows(InfoPanel& panel, bool (*predicate)(df::unit*), size_t limit = 80) {
    for (auto unit : active_units()) {
        if (!predicate(unit))
            continue;
        panel.rows.push_back(row_for_unit(unit));
        if (panel.rows.size() >= limit)
            break;
    }
}

void build_creatures_panel(InfoPanel& panel) {
    if (panel.detail == "pets") {
        add_unit_rows(panel, is_pet_or_livestock);
        for (auto& row : panel.rows) {
            if (row.status.empty())
                row.status = row.badges.empty() ? "Domesticated" : row.badges.front();
            if (row.status == "Domesticated") {
                row.job = "Unavailable as pet";
                if (auto unit = df::unit::find(row.unit_id)) {
                    if (Units::isAvailableForAdoption(unit))
                        row.job = "Interested in an owner";
                }
            }
        }
        panel.footer = "Ctrl+a: Autoretrain livestock: Off";
    } else if (panel.detail == "other") {
        add_unit_rows(panel, is_other_creature);
        for (auto& row : panel.rows) {
            row.status = "Wild Animal";
            row.job.clear();
        }
    } else if (panel.detail == "dead") {
        add_unit_rows(panel, is_dead_or_missing);
        panel.footer = "[d: Show death cause]";
    } else {
        add_unit_rows(panel, is_resident);
        for (auto& row : panel.rows) {
            row.status = row.job;
            row.job.clear();
        }
    }
    if (panel.rows.empty() && panel.detail == "dead")
        panel.messages.push_back("No dead or missing citizens.");
    else if (panel.rows.empty())
        panel.messages.push_back("No entries.");
}

void build_tasks_panel(InfoPanel& panel) {
    for (auto unit : active_units()) {
        if (!is_resident(unit) || !unit->job.current_job)
            continue;
        auto row = row_for_unit(unit);
        row.status = job_name(unit);
        panel.rows.push_back(row);
        if (panel.rows.size() >= 80)
            break;
    }
    if (panel.rows.empty())
        panel.messages.push_back("No active citizen tasks.");
}

std::string pretty_enum_key(const std::string& key) {
    std::string out;
    out.reserve(key.size() + 8);
    char prev = 0;
    for (char ch : key) {
        if (ch == '_') {
            out.push_back(' ');
        } else {
            if (prev && std::islower(static_cast<unsigned char>(prev)) &&
                std::isupper(static_cast<unsigned char>(ch)))
                out.push_back(' ');
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        prev = ch;
    }
    if (!out.empty())
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

std::string map_pos_label(df::building* b) {
    if (!b)
        return "";
    return "Tile: " + std::to_string(b->centerx) + ", " +
        std::to_string(b->centery) + ", " + std::to_string(b->z);
}

std::string building_size_label(df::building* b) {
    if (!b)
        return "";
    int w = std::max(1, b->x2 - b->x1 + 1);
    int h = std::max(1, b->y2 - b->y1 + 1);
    return std::to_string(w) + "x" + std::to_string(h);
}

bool visible_building(df::building* b) {
    return b && b->flags.bits.exists && !b->flags.bits.almost_deleted;
}

std::string building_label(df::building* b) {
    if (!b)
        return "";
    std::string name = Buildings::getName(b);
    if (!name.empty())
        return name;
    if (auto sp = virtual_cast<df::building_stockpilest>(b))
        return "Stockpile #" + std::to_string(sp->stockpile_number);
    if (auto ws = virtual_cast<df::building_workshopst>(b))
        return pretty_enum_key(enum_item_key(ws->type));
    if (auto furnace = virtual_cast<df::building_furnacest>(b))
        return pretty_enum_key(enum_item_key(furnace->type));
    if (auto siege = virtual_cast<df::building_siegeenginest>(b)) {
        if (siege->type == df::siegeengine_type::BoltThrower)
            return "Bolt Thrower";
        return pretty_enum_key(enum_item_key(siege->type));
    }
    if (b->getType() == df::building_type::FarmPlot)
        return "Farm Plot";
    return pretty_enum_key(enum_item_key(b->getType()));
}

std::string construction_status(df::building* b) {
    if (!b)
        return "";
    if (Buildings::markedForRemoval(b))
        return "Marked for removal";
    if (b->getBuildStage() < b->getMaxBuildStage()) {
        bool suspended = false;
        for (auto job : b->jobs) {
            if (job && job->flags.bits.suspend) {
                suspended = true;
                break;
            }
        }
        return suspended ? "Construction suspended" : "Under construction";
    }
    return "";
}

void set_row_pos(InfoRow& row, df::building* b) {
    if (!b)
        return;
    row.x = b->centerx;
    row.y = b->centery;
    row.z = b->z;
    row.has_pos = true;
}

InfoRow base_place_row(df::building* b, const std::string& kind, const std::string& category) {
    InfoRow row;
    row.kind = kind;
    row.building_id = b ? b->id : -1;
    row.name = building_label(b);
    row.subtitle = map_pos_label(b);
    row.category = category;
    row.profession = building_size_label(b);
    row.status = construction_status(b);
    set_row_pos(row, b);
    return row;
}

struct ZoneTypeMeta {
    const char* key;
    const char* label;
    int icon_x;
    int icon_y;
};

ZoneTypeMeta place_zone_type_meta(df::civzone_type type) {
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

int stockpile_icon_row(df::building_stockpilest* sp) {
    if (!sp)
        return 17;
    const auto& f = sp->settings.flags.bits;
    if (f.ammo) return 0;
    if (f.animals) return 1;
    if (f.armor) return 2;
    if (f.bars_blocks) return 3;
    if (f.cloth) return 4;
    if (f.coins) return 5;
    if (f.finished_goods) return 6;
    if (f.food) return 7;
    if (f.furniture) return 8;
    if (f.gems) return 9;
    if (f.leather) return 10;
    if (f.corpses) return 11;
    if (f.refuse) return 12;
    if (f.sheet) return 13;
    if (f.stone) return 15;
    if (f.weapons) return 16;
    if (f.wood) return 17;
    return 17;
}

std::string stockpile_groups_text(df::building_stockpilest* sp) {
    if (!sp)
        return "";
    const auto& f = sp->settings.flags.bits;
    std::vector<std::string> labels;
    if (f.animals) labels.push_back("Animals");
    if (f.food) labels.push_back("Food");
    if (f.furniture) labels.push_back("Furniture");
    if (f.corpses) labels.push_back("Corpses");
    if (f.refuse) labels.push_back("Refuse");
    if (f.stone) labels.push_back("Stone");
    if (f.ammo) labels.push_back("Ammo");
    if (f.coins) labels.push_back("Coins");
    if (f.bars_blocks) labels.push_back("Bars/blocks");
    if (f.gems) labels.push_back("Gems");
    if (f.finished_goods) labels.push_back("Finished goods");
    if (f.leather) labels.push_back("Leather");
    if (f.cloth) labels.push_back("Cloth");
    if (f.wood) labels.push_back("Wood");
    if (f.weapons) labels.push_back("Weapons");
    if (f.armor) labels.push_back("Armor");
    if (f.sheet) labels.push_back("Sheets");
    if (labels.empty())
        return "No item categories";
    std::ostringstream out;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i)
            out << ", ";
        out << labels[i];
        if (i == 2 && labels.size() > 3) {
            out << ", +" << (labels.size() - 3) << " more";
            break;
        }
    }
    return out.str();
}

const char* workshop_icon_key(df::workshop_type type) {
    switch (type) {
    case df::workshop_type::Carpenters:       return "workshop_carpenter";
    case df::workshop_type::Masons:           return "workshop_mason";
    case df::workshop_type::MetalsmithsForge: return "workshop_metalsmith";
    case df::workshop_type::MagmaForge:       return "workshop_metalsmith";
    case df::workshop_type::Craftsdwarfs:     return "workshop_crafts";
    case df::workshop_type::Jewelers:         return "workshop_jeweler";
    case df::workshop_type::Bowyers:          return "workshop_bowyer";
    case df::workshop_type::Mechanics:        return "workshop_mechanic";
    case df::workshop_type::Siege:            return "workshop_siege";
    case df::workshop_type::Ashery:           return "workshop_ashery";
    case df::workshop_type::Leatherworks:     return "workshop_leather";
    case df::workshop_type::Loom:             return "workshop_loom";
    case df::workshop_type::Clothiers:        return "workshop_clothes";
    case df::workshop_type::Dyers:            return "workshop_dyer";
    case df::workshop_type::Butchers:         return "workshop_butcher";
    case df::workshop_type::Tanners:          return "workshop_tanner";
    case df::workshop_type::Fishery:          return "workshop_fishery";
    case df::workshop_type::Kitchen:          return "workshop_kitchen";
    case df::workshop_type::Farmers:          return "workshop_farmer";
    case df::workshop_type::Quern:            return "workshop_quern";
    case df::workshop_type::Millstone:        return "workshop_millstone";
    case df::workshop_type::Kennels:          return "workshop_kennel";
    case df::workshop_type::Still:            return "workshop_still";
    default:                                  return "workshops";
    }
}

const char* furnace_icon_key(df::furnace_type type) {
    switch (type) {
    case df::furnace_type::WoodFurnace: return "furnace_wood";
    case df::furnace_type::Smelter:
    case df::furnace_type::MagmaSmelter: return "furnace_smelter";
    case df::furnace_type::GlassFurnace:
    case df::furnace_type::MagmaGlassFurnace: return "furnace_glass";
    case df::furnace_type::Kiln:
    case df::furnace_type::MagmaKiln: return "furnace_kiln";
    default: return "workshops_furnaces";
    }
}

std::string first_job_label(df::building* b) {
    if (!b)
        return "";
    for (auto job : b->jobs) {
        if (!job)
            continue;
        std::string name = Job::getName(job);
        if (!name.empty())
            return job->flags.bits.suspend ? (name + " (suspended)") : name;
    }
    return "";
}

df::world_site* current_site() {
    int32_t site_id = World::GetCurrentSiteId();
    if (site_id >= 0) {
        if (auto site = df::world_site::find(site_id))
            return site;
    }
    auto world = df::global::world;
    if (world && world->world_data && !world->world_data->active_site.empty())
        return world->world_data->active_site.front();
    return nullptr;
}

std::string abstract_location_type_label(df::abstract_building_type type) {
    switch (type) {
    case df::abstract_building_type::INN_TAVERN: return "Inn Tavern";
    case df::abstract_building_type::TEMPLE: return "Temple";
    case df::abstract_building_type::HOSPITAL: return "Hospital";
    case df::abstract_building_type::GUILDHALL: return "Guildhall";
    case df::abstract_building_type::LIBRARY: return "Library";
    case df::abstract_building_type::TOMB: return "Tomb";
    case df::abstract_building_type::COUNTING_HOUSE: return "Counting House";
    case df::abstract_building_type::DUNGEON: return "Dungeon";
    default: return pretty_enum_key(enum_item_key(type));
    }
}

const char* abstract_location_icon_key(df::abstract_building_type type) {
    switch (type) {
    case df::abstract_building_type::INN_TAVERN: return "table";
    case df::abstract_building_type::TEMPLE: return "offering_place";
    case df::abstract_building_type::HOSPITAL: return "bed";
    case df::abstract_building_type::GUILDHALL: return "workshops";
    case df::abstract_building_type::LIBRARY: return "bookcase";
    case df::abstract_building_type::TOMB: return "coffin";
    case df::abstract_building_type::COUNTING_HOUSE: return "box";
    case df::abstract_building_type::DUNGEON: return "cage";
    default: return "furniture";
    }
}

std::string abstract_location_name(df::abstract_building* location) {
    if (!location)
        return "";
    std::string type = abstract_location_type_label(location->getType());
    auto name = location->getName();
    if (name) {
        std::string translated = Translation::translateName(name, true);
        if (!translated.empty())
            return translated;
    }
    return type + " #" + std::to_string(location->id);
}

df::building* first_location_building(df::abstract_building* location) {
    if (!location)
        return nullptr;
    auto contents = location->getContents();
    if (!contents)
        return nullptr;
    for (int32_t id : contents->building_ids) {
        if (auto b = df::building::find(id))
            return b;
    }
    return nullptr;
}

std::string farm_crop_label(df::building_farmplotst* farm) {
    if (!farm)
        return "";
    int season = static_cast<int>(farm->last_season);
    if (season < 0 || season > df::enum_traits<df::season>::last_item_value)
        season = 0;
    int plant_id = farm->plant_id[season];
    if (plant_id < 0)
        return "No crop selected";
    if (auto plant = df::plant_raw::find(plant_id)) {
        if (!plant->name_plural.empty())
            return plant->name_plural;
        if (!plant->id.empty())
            return pretty_enum_key(plant->id);
    }
    return "Plant " + std::to_string(plant_id);
}

std::string siege_engine_type_label(df::building_siegeenginest* siege) {
    if (!siege)
        return "";
    if (siege->type == df::siegeengine_type::BoltThrower)
        return "Bolt Thrower";
    return pretty_enum_key(enum_item_key(siege->type));
}

const char* siege_engine_icon_key(df::building_siegeenginest* siege) {
    if (!siege)
        return "workshop_siege";
    return siege->type == df::siegeengine_type::Catapult ? "catapult" : "ballista";
}

void sort_place_rows(InfoPanel& panel) {
    std::sort(panel.rows.begin(), panel.rows.end(), [](const InfoRow& a, const InfoRow& b) {
        if (a.name != b.name)
            return a.name < b.name;
        return a.building_id < b.building_id;
    });
}

void build_places_panel(InfoPanel& panel) {
    auto world = df::global::world;
    if (!world) {
        panel.messages.push_back("World data is not loaded.");
        return;
    }

    if (panel.detail == "zones") {
        for (auto zone : world->buildings.other.ANY_ZONE) {
            if (!visible_building(zone))
                continue;
            auto meta = place_zone_type_meta(zone->type);
            auto row = base_place_row(zone, "zone", meta.label);
            row.icon_sheet = "zone";
            row.icon_x = meta.icon_x;
            row.icon_y = meta.icon_y;
            row.status = zone->spec_sub_flag.bits.active ? "Active" : "Suspended";
            if (row.name.empty())
                row.name = meta.label;
            panel.rows.push_back(row);
            if (panel.rows.size() >= 120)
                break;
        }
        sort_place_rows(panel);
        if (panel.rows.empty()) {
            panel.messages.push_back("You do not have any zones.");
            panel.messages.push_back("Zones can be added using the zone menu at the bottom of the screen.");
        }
    } else if (panel.detail == "locations") {
        if (auto site = current_site()) {
            for (auto location : site->buildings) {
                if (!location)
                    continue;
                std::string type = abstract_location_type_label(location->getType());
                InfoRow row;
                row.kind = "location";
                row.location_id = location->id;
                row.name = abstract_location_name(location);
                row.category = "Location";
                row.profession = type;
                row.icon_key = abstract_location_icon_key(location->getType());
                if (auto b = first_location_building(location)) {
                    row.building_id = b->id;
                    row.subtitle = map_pos_label(b);
                    set_row_pos(row, b);
                    if (b->getType() == df::building_type::Civzone)
                        row.kind = "zone";
                    else
                        row.kind = "building";
                }
                if (auto contents = location->getContents()) {
                    row.status = std::to_string(contents->building_ids.size()) +
                        (contents->building_ids.size() == 1 ? " zone" : " zones");
                    if (contents->location_value > 0)
                        row.job = "Value: " + std::to_string(contents->location_value);
                }
                panel.rows.push_back(row);
                if (panel.rows.size() >= 80)
                    break;
            }
            sort_place_rows(panel);
        }
        if (panel.rows.empty())
            panel.messages.push_back("No locations listed.");
    } else if (panel.detail == "stockpiles") {
        for (auto sp : world->buildings.other.STOCKPILE) {
            if (!visible_building(sp))
                continue;
            auto row = base_place_row(sp, "stockpile", "Stockpile");
            row.icon_sheet = "stockpile";
            row.icon_row = stockpile_icon_row(sp);
            row.job = stockpile_groups_text(sp);
            if (sp->stockpile_flag.bits.use_links_only)
                row.status = "Links only";
            panel.rows.push_back(row);
            if (panel.rows.size() >= 120)
                break;
        }
        sort_place_rows(panel);
        if (panel.rows.empty())
            panel.messages.push_back("No stockpiles listed.");
    } else if (panel.detail == "workshops") {
        for (auto ws : world->buildings.other.WORKSHOP_ANY) {
            if (!visible_building(ws))
                continue;
            auto row = base_place_row(ws, "workshop", "Workshop");
            row.icon_key = workshop_icon_key(ws->type);
            row.profession = pretty_enum_key(enum_item_key(ws->type));
            row.job = first_job_label(ws);
            if (row.job.empty() && row.status.empty())
                row.status = "Idle";
            panel.rows.push_back(row);
        }
        for (auto furnace : world->buildings.other.FURNACE_ANY) {
            if (!visible_building(furnace))
                continue;
            auto row = base_place_row(furnace, "workshop", "Furnace");
            row.icon_key = furnace_icon_key(furnace->type);
            row.profession = pretty_enum_key(enum_item_key(furnace->type));
            row.job = first_job_label(furnace);
            if (row.job.empty() && row.status.empty())
                row.status = "Idle";
            panel.rows.push_back(row);
        }
        sort_place_rows(panel);
        if (panel.rows.empty())
            panel.messages.push_back("No workshops listed.");
    } else if (panel.detail == "farmplots") {
        for (auto farm : world->buildings.other.FARM_PLOT) {
            if (!visible_building(farm))
                continue;
            auto row = base_place_row(farm, "building", "Farm plot");
            row.icon_key = "farm_plot";
            row.job = farm_crop_label(farm);
            if (farm->max_fertilization > 0)
                row.status = "Fertilized: " + std::to_string(farm->current_fertilization) +
                    "/" + std::to_string(farm->max_fertilization);
            panel.rows.push_back(row);
            if (panel.rows.size() >= 120)
                break;
        }
        sort_place_rows(panel);
        if (panel.rows.empty())
            panel.messages.push_back("No farm plots listed.");
    } else {
        for (auto b : world->buildings.all) {
            auto siege = virtual_cast<df::building_siegeenginest>(b);
            if (!visible_building(siege))
                continue;
            auto row = base_place_row(siege, "building", "Siege engine");
            row.icon_key = siege_engine_icon_key(siege);
            row.profession = siege_engine_type_label(siege);
            row.status = pretty_enum_key(enum_item_key(siege->action));
            row.job = first_job_label(siege);
            panel.rows.push_back(row);
            if (panel.rows.size() >= 80)
                break;
        }
        sort_place_rows(panel);
        if (panel.rows.empty())
            panel.messages.push_back("No siege engines listed.");
    }
}

void add_labor_side_items(InfoPanel& panel) {
    panel.side_items = {
        "Add new work detail",
        "Miners",
        "Woodcutters",
        "Hunters",
        "Planters",
        "Fisherdwarves",
        "Plant gatherers",
        "Stonecutters",
        "Engravers",
        "Haulers",
        "Orderlies",
        "Siege operators",
    };
}

bool unit_has_labor(df::unit* unit, df::unit_labor labor) {
    int idx = static_cast<int>(labor);
    return idx >= 0 && idx <= df::enum_traits<df::unit_labor>::last_item_value &&
           unit->status.labors[idx];
}

void build_labor_panel(InfoPanel& panel) {
    add_labor_side_items(panel);
    if (panel.detail != "workdetails") {
        panel.messages.push_back("This independent labor subtab shell is ready; detailed controls are not wired yet.");
        return;
    }
    for (auto unit : active_units()) {
        if (!is_resident(unit))
            continue;
        auto row = row_for_unit(unit);
        row.status = unit_has_labor(unit, df::unit_labor::MINE) ? "Selected" : "";
        row.job = row.profession;
        panel.rows.push_back(row);
        if (panel.rows.size() >= 80)
            break;
    }
    if (panel.rows.empty())
        panel.messages.push_back("No citizens available for work details.");
    panel.footer = "Ctrl+e: Save work details | Ctrl+i: Load work details";
}

std::string position_name(df::entity_position* position) {
    if (!position)
        return "";
    if (!position->name[0].empty())
        return position->name[0];
    if (!position->code.empty())
        return pretty_key(position->code);
    return "Position " + std::to_string(position->id);
}

void build_nobles_panel(InfoPanel& panel) {
    auto plotinfo = df::global::plotinfo;
    auto entity = plotinfo ? df::historical_entity::find(plotinfo->civ_id) : nullptr;
    panel.messages.push_back("Ask host to assign");
    panel.messages.push_back("Members of the nobility have required rooms and can make demands. They cannot be reassigned.");
    panel.messages.push_back("Administrators handle various aspects of your fortress and can be reassigned.");
    if (entity) {
        for (auto position : entity->positions.own) {
            if (!position)
                continue;
            InfoRow row;
            row.name = position_name(position);
            row.status = "VACANT";
            for (auto assignment : entity->positions.assignments) {
                if (!assignment || assignment->position_id != position->id)
                    continue;
                if (assignment->histfig != -1) {
                    row.status = "Assigned";
                    row.subtitle = "historical figure " + std::to_string(assignment->histfig);
                    for (auto unit : active_units()) {
                        if (unit && unit->hist_figure_id == assignment->histfig) {
                            row.subtitle = unit_display_name(unit);
                            row.unit_id = unit->id;
                            row.portrait_texpos = unit->portrait_texpos;
                            row.profession = Units::getProfessionName(unit);
                            break;
                        }
                    }
                    break;
                }
            }
            panel.rows.push_back(row);
            if (panel.rows.size() >= 40)
                break;
        }
    }
    if (panel.rows.empty()) {
        const char* fallback[] = {
            "Expedition leader", "Militia commander", "Sheriff", "Hammerer",
            "Manager", "Chief medical dwarf", "Broker", "Bookkeeper", "Messenger"
        };
        for (auto name : fallback) {
            InfoRow row;
            row.name = name;
            row.status = std::string(name) == "Messenger" ? "NEW" : "VACANT";
            panel.rows.push_back(row);
        }
    }
}

std::string translated_name(const df::language_name& name) {
    if (!name.has_name)
        return "";
    std::string native = Translation::translateName(&name, false);
    std::string english = Translation::translateName(&name, true);
    if (native.empty())
        return english;
    if (english.empty() || english == native)
        return native;
    return native + " \"" + english + "\"";
}

std::string historical_figure_name(int32_t id) {
    if (id < 0)
        return "";
    auto hf = df::historical_figure::find(id);
    if (!hf)
        return "";
    std::string name = translated_name(hf->name);
    return name.empty() ? ("Historical figure " + std::to_string(id)) : name;
}

std::string artifact_name(df::artifact_record* artifact) {
    if (!artifact)
        return "";
    std::string name = translated_name(artifact->name);
    if (!name.empty())
        return name;
    if (artifact->item) {
        std::string desc = Items::getDescription(artifact->item, 1, true);
        if (!desc.empty())
            return desc;
    }
    return "Artifact " + std::to_string(artifact->id);
}

std::string object_item_description(df::item* item) {
    if (!item)
        return "";
    std::string desc = Items::getDescription(item, 1, true);
    if (desc.empty())
        desc = enum_item_key(item->getType()) + " #" + std::to_string(item->id);
    return desc;
}

bool is_written_artifact(df::artifact_record* artifact) {
    if (!artifact || !artifact->item)
        return false;
    using namespace df::enums::item_type;
    auto type = artifact->item->getType();
    return type == BOOK || type == SLAB || type == TOOL;
}

bool artifact_has_claim_type(df::artifact_record* artifact, df::artifact_claim_type type) {
    auto world = df::global::world;
    if (!world || !artifact)
        return false;
    for (auto entity : world->entities.all) {
        if (!entity)
            continue;
        for (auto claim : entity->artifact_claims) {
            if (claim && claim->artifact_id == artifact->id && claim->claim_type == type)
                return true;
        }
    }
    return false;
}

void set_row_item_position(InfoRow& row, df::item* item) {
    if (!item || item->flags.bits.removed || item->flags.bits.garbage_collect)
        return;
    df::coord pos = Items::getPosition(item);
    if (pos.x < 0 || pos.y < 0 || pos.z < 0)
        return;
    row.x = pos.x;
    row.y = pos.y;
    row.z = pos.z;
    row.has_pos = true;
}

InfoRow row_for_artifact(df::artifact_record* artifact, const std::string& category) {
    InfoRow row;
    row.kind = "item";
    row.category = category;
    if (!artifact)
        return row;
    row.name = artifact_name(artifact);
    row.subtitle = object_item_description(artifact->item);
    if (artifact->item) {
        row.item_id = artifact->item->id;
        row.profession = pretty_enum_key(enum_item_key(artifact->item->getType()));
        set_row_item_position(row, artifact->item);
    }
    std::string holder = historical_figure_name(artifact->holder_hf >= 0 ? artifact->holder_hf : artifact->owner_hf);
    if (!holder.empty())
        row.job = "Held by " + holder;
    if (row.has_pos)
        row.status = "On map";
    else if (artifact->site >= 0 || artifact->storage_site >= 0)
        row.status = "Known";
    else
        row.status = "No location";
    row.badges.push_back("Artifact");
    return row;
}

void add_artifact_rows(InfoPanel& panel,
                       const std::vector<df::artifact_record*>& artifacts,
                       const std::string& category,
                       size_t limit) {
    std::vector<int32_t> seen;
    for (auto artifact : artifacts) {
        if (!artifact || std::find(seen.begin(), seen.end(), artifact->id) != seen.end())
            continue;
        seen.push_back(artifact->id);
        panel.rows.push_back(row_for_artifact(artifact, category));
        if (panel.rows.size() >= limit)
            break;
    }
}

std::vector<df::artifact_record*> all_world_artifacts() {
    std::vector<df::artifact_record*> out;
    auto world = df::global::world;
    if (!world)
        return out;
    for (auto artifact : world->artifacts.all) {
        if (artifact)
            out.push_back(artifact);
    }
    return out;
}

std::vector<df::artifact_record*> symbol_artifacts() {
    std::vector<df::artifact_record*> out;
    auto world = df::global::world;
    if (!world)
        return out;
    for (auto entity : world->entities.all) {
        if (!entity)
            continue;
        for (auto claim : entity->artifact_claims) {
            if (!claim || claim->claim_type != df::artifact_claim_type::Symbol)
                continue;
            auto artifact = df::artifact_record::find(claim->artifact_id);
            if (artifact && std::find(out.begin(), out.end(), artifact) == out.end())
                out.push_back(artifact);
        }
    }
    return out;
}

std::vector<df::artifact_record*> named_object_artifacts() {
    std::vector<df::artifact_record*> out;
    for (auto artifact : all_world_artifacts()) {
        if (is_written_artifact(artifact))
            continue;
        if (artifact_has_claim_type(artifact, df::artifact_claim_type::Symbol))
            continue;
        if (artifact->item && artifact->item->flags.bits.artifact)
            out.push_back(artifact);
    }
    return out;
}

std::string written_author_label(df::written_content* content) {
    if (!content || content->author < 0)
        return "";
    std::string author = historical_figure_name(content->author);
    return author.empty() ? "" : ("By " + author);
}

std::string written_page_label(df::written_content* content) {
    if (!content)
        return "";
    if (content->page_start >= 0 && content->page_end >= content->page_start)
        return "Pages " + std::to_string(content->page_start) + "-" + std::to_string(content->page_end);
    if (content->page_start >= 0)
        return "Page " + std::to_string(content->page_start);
    return "";
}

void build_written_content_panel(InfoPanel& panel) {
    auto world = df::global::world;
    if (!world)
        return;
    for (auto content : world->written_contents.all) {
        if (!content)
            continue;
        InfoRow row;
        row.kind = "written";
        row.name = content->title.empty() ? ("Written content " + std::to_string(content->id)) : content->title;
        row.category = pretty_enum_key(enum_item_key(content->type));
        row.profession = written_author_label(content);
        row.status = written_page_label(content);
        if (!content->styles.empty())
            row.job = pretty_enum_key(enum_item_key(content->styles.front()));
        panel.rows.push_back(row);
        if (panel.rows.size() >= 120)
            break;
    }
}

void build_objects_panel(InfoPanel& panel) {
    if (panel.detail.empty())
        panel.detail = "artifacts";
    if (panel.detail == "artifacts") {
        add_artifact_rows(panel, all_world_artifacts(), "Artifact", 120);
    } else if (panel.detail == "symbols") {
        add_artifact_rows(panel, symbol_artifacts(), "Symbol", 120);
        for (auto& row : panel.rows) {
            row.badges.push_back("Symbol");
            row.status = row.has_pos ? "On map" : "Assigned";
        }
    } else if (panel.detail == "named") {
        add_artifact_rows(panel, named_object_artifacts(), "Named object", 120);
    } else if (panel.detail == "written") {
        for (auto artifact : all_world_artifacts()) {
            if (is_written_artifact(artifact)) {
                panel.rows.push_back(row_for_artifact(artifact, "Written object"));
                if (panel.rows.size() >= 120)
                    break;
            }
        }
        if (panel.rows.empty())
            build_written_content_panel(panel);
    }

    if (panel.rows.empty()) {
        if (panel.detail == "artifacts")
            panel.messages.push_back("No artifacts listed.");
        else if (panel.detail == "symbols")
            panel.messages.push_back("No symbols listed.");
        else if (panel.detail == "named")
            panel.messages.push_back("No named objects listed.");
        else if (panel.detail == "written")
            panel.messages.push_back("No written content listed.");
        else
            panel.messages.push_back("No entries.");
    } else {
        panel.footer = std::to_string(panel.rows.size()) + " entries listed.";
    }
}

void build_workorders_panel(InfoPanel& panel) {
    auto world = df::global::world;
    size_t count = world ? world->manager_orders.all.size() : 0;
    if (count == 0) {
        panel.messages.push_back("No work orders.");
        panel.messages.push_back("Manager work order editing is not wired into the independent client yet.");
    } else {
        panel.messages.push_back(std::to_string(count) + " manager orders exist.");
        panel.messages.push_back("Detailed manager order decoding is the next work-order pass.");
    }
}

void build_justice_panel(InfoPanel& panel) {
    panel.messages.push_back("No open justice cases listed.");
}

void build_stocks_panel(InfoPanel& panel) {
    panel.panel = "stocks";
    panel.section = "stocks";
    auto categories = stock_categories();
    if (panel.detail.empty() && !categories.empty())
        panel.detail = stock_category_key(categories.front().type);
    panel.title = "Stocks";
    panel.primary_tabs.clear();
    panel.section_tabs = stocks_section_tabs();
    panel.detail_tabs.clear();
    panel.footer = "Counts update from the host fort.";

    auto counts = stock_counts();
    for (const auto& category : categories) {
        int idx = item_type_index(category.type);
        int32_t count = idx >= 0 && static_cast<size_t>(idx) < counts.size() ? counts[idx] : 0;
        InfoRow row;
        row.name = category.label;
        row.category = "Stock";
        row.status = format_stock_count(count);
        row.job = stock_category_key(category.type);
        row.muted = count <= 0;
        panel.rows.push_back(row);
    }

    StockCategory selected;
    if (stock_category_for_key(panel.detail, &selected))
        add_stock_items_for_type(panel, selected.type);
}

struct RenderThreadPanelRequest {
    std::string panel_name;
    std::string section;
    std::string detail;
    InfoPanel panel;
    std::string err;
    std::promise<bool> done;
};

} // namespace

InfoPanel build_info_panel(const std::string& panel_name,
                           const std::string& requested_section,
                           const std::string& requested_detail) {
    InfoPanel panel;
    panel.panel = panel_name.empty() ? "citizens" : panel_name;
    panel.section = requested_section.empty() ? default_section_for_panel(panel.panel) : requested_section;
    panel.detail = requested_detail.empty() ? default_detail_for_section(panel.section) : requested_detail;
    panel.title = "Citizens";
    panel.primary_tabs = primary_tabs();
    panel.section_tabs = section_tabs();
    panel.detail_tabs = detail_tabs_for(panel.section);

    if (panel.panel == "stocks" || panel.section == "stocks") {
        build_stocks_panel(panel);
    } else if (panel.section == "creatures") {
        panel.title = "Citizens";
        build_creatures_panel(panel);
    } else if (panel.section == "tasks") {
        panel.title = "Tasks";
        build_tasks_panel(panel);
    } else if (panel.section == "places") {
        panel.title = "Places";
        build_places_panel(panel);
    } else if (panel.section == "labor") {
        panel.title = "Labor";
        build_labor_panel(panel);
    } else if (panel.section == "workorders") {
        panel.title = "Work Orders";
        build_workorders_panel(panel);
    } else if (panel.section == "nobles") {
        panel.title = "Nobles and administrators";
        build_nobles_panel(panel);
    } else if (panel.section == "objects") {
        panel.title = "Objects";
        panel.detail_tabs = detail_tabs_for("objects");
        if (panel.detail.empty())
            panel.detail = "artifacts";
        build_objects_panel(panel);
    } else if (panel.section == "justice") {
        panel.title = "Justice";
        build_justice_panel(panel);
    } else {
        panel.messages.push_back("Unknown panel section.");
    }

    return panel;
}

std::string info_panel_json(const InfoPanel& panel) {
    std::ostringstream body;
    body << "{"
         << "\"panel\":" << json_string(panel.panel) << ","
         << "\"section\":" << json_string(panel.section) << ","
         << "\"detail\":" << json_string(panel.detail) << ","
         << "\"title\":" << json_string(panel.title) << ","
         << "\"primaryTabs\":";
    append_tabs(body, panel.primary_tabs);
    body << ",\"sectionTabs\":";
    append_tabs(body, panel.section_tabs);
    body << ",\"detailTabs\":";
    append_tabs(body, panel.detail_tabs);
    body << ",\"messages\":";
    append_string_array(body, panel.messages);
    body << ",\"sideItems\":";
    append_string_array(body, panel.side_items);
    body << ",\"footer\":" << json_string(panel.footer) << ",\"rows\":[";
    for (size_t i = 0; i < panel.rows.size(); ++i) {
        const auto& row = panel.rows[i];
        if (i) body << ",";
        body << "{"
             << "\"unitId\":" << row.unit_id << ","
             << "\"itemId\":" << row.item_id << ","
             << "\"portraitTexpos\":" << row.portrait_texpos << ","
             << "\"buildingId\":" << row.building_id << ","
             << "\"locationId\":" << row.location_id << ","
             << "\"kind\":" << json_string(row.kind) << ","
             << "\"hasPos\":" << (row.has_pos ? "true" : "false") << ","
             << "\"x\":" << row.x << ","
             << "\"y\":" << row.y << ","
             << "\"z\":" << row.z << ","
             << "\"iconKey\":" << json_string(row.icon_key) << ","
             << "\"iconSheet\":" << json_string(row.icon_sheet) << ","
             << "\"iconX\":" << row.icon_x << ","
             << "\"iconY\":" << row.icon_y << ","
             << "\"iconRow\":" << row.icon_row << ","
             << "\"name\":" << json_string(row.name) << ","
             << "\"subtitle\":" << json_string(row.subtitle) << ","
             << "\"category\":" << json_string(row.category) << ","
             << "\"profession\":" << json_string(row.profession) << ","
             << "\"job\":" << json_string(row.job) << ","
             << "\"status\":" << json_string(row.status) << ","
             << "\"muted\":" << (row.muted ? "true" : "false") << ","
             << "\"badges\":";
        append_string_array(body, row.badges);
        body << "}";
    }
    body << "],\"stockItems\":[";
    for (size_t i = 0; i < panel.stock_items.size(); ++i) {
        const auto& item = panel.stock_items[i];
        if (i) body << ",";
        body << "{"
             << "\"itemId\":" << item.item_id << ","
             << "\"count\":" << item.count << ","
             << "\"name\":" << json_string(item.name) << ","
             << "\"subtitle\":" << json_string(item.subtitle) << ","
             << "\"status\":" << json_string(item.status) << ","
             << "\"muted\":" << (item.muted ? "true" : "false")
             << "}";
    }
    body << "]}\n";
    return body.str();
}

bool info_panel_on_render_thread(const std::string& panel_name,
                                 const std::string& section,
                                 const std::string& detail,
                                 InfoPanel& panel,
                                 std::string* err) {
    std::lock_guard<std::recursive_mutex> lock(g_info_panel_mutex);

    auto request = std::make_shared<RenderThreadPanelRequest>();
    request->panel_name = panel_name;
    request->section = section;
    request->detail = detail;
    auto future = request->done.get_future();

    DFHack::runOnRenderThread([request]() {
        try {
            request->panel = build_info_panel(request->panel_name,
                                              request->section,
                                              request->detail);
            request->done.set_value(true);
        } catch (const std::exception& ex) {
            request->err = ex.what();
            request->done.set_value(false);
        } catch (...) {
            request->err = "unknown info panel error";
            request->done.set_value(false);
        }
    });

    bool ok = future.get();
    if (!ok) {
        if (err) *err = request->err;
        return false;
    }
    panel = std::move(request->panel);
    return true;
}

} // namespace dfcapture
