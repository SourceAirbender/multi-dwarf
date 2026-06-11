#include "hud.h"

#include "TileTypes.h"
#include "json_util.h"
#include "modules/DFSDL.h"
#include "modules/Maps.h"
#include "modules/Translation.h"
#include "modules/Units.h"

#include "df/coord.h"
#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/historical_entity.h"
#include "df/map_block.h"
#include "df/plotinfost.h"
#include "df/tile_liquid.h"
#include "df/tiletype_material.h"
#include "df/tiletype_shape_basic.h"
#include "df/unit.h"
#include "df/world.h"
#include "df/world_data.h"
#include "df/world_site.h"

#include <algorithm>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>

namespace dfcapture {
namespace {

std::recursive_mutex g_hud_mutex;

const char* month_name(int month) {
    static const char* names[] = {
        "Granite", "Slate", "Felsite",
        "Hematite", "Malachite", "Galena",
        "Limestone", "Sandstone", "Timber",
        "Moonstone", "Opal", "Obsidian"
    };
    if (month < 0 || month >= 12)
        return "Month";
    return names[month];
}

const char* season_phase_name(int month) {
    static const char* seasons[] = {"Spring", "Summer", "Autumn", "Winter"};
    static const char* phases[] = {"Early", "Mid", "Late"};
    if (month < 0 || month >= 12)
        return "Season";
    static thread_local std::string label;
    label = std::string(phases[month % 3]) + " " + seasons[month / 3];
    return label.c_str();
}

int moon_icon_for_phase(int phase) {
    phase %= 28;
    if (phase < 0)
        phase += 28;
    if (phase <= 1 || phase >= 27) return 3;
    if (phase <= 5) return 4;
    if (phase <= 9) return 5;
    if (phase <= 12) return 6;
    if (phase <= 15) return 7;
    if (phase <= 18) return 0;
    if (phase <= 22) return 1;
    return 2;
}

const char* rank_name(int rank) {
    static const char* names[] = {
        "Outpost", "Hamlet", "Village", "Town", "City", "Metropolis"
    };
    if (rank < 0 || rank >= 6)
        return "Fortress";
    return names[rank];
}

void effective_viewport_dims(int& w, int& h) {
    auto gps = df::global::gps;
    auto vp = gps ? gps->main_viewport : nullptr;
    if (vp && vp->dim_x > 0 && vp->dim_y > 0) {
        w = vp->dim_x;
        h = vp->dim_y;
    } else {
        w = 80;
        h = 50;
    }
}

int minimap_color_for_tile(df::tiletype tt, const df::tile_designation& des) {
    auto shape = DFHack::tileShapeBasic(DFHack::tileShape(tt));
    auto mat = DFHack::tileMaterial(tt);
    bool wall = shape == df::tiletype_shape_basic::Wall;

    if (des.bits.flow_size > 0)
        return des.bits.liquid_type == df::tile_liquid::Magma ? 8 : 7;

    using namespace df::enums::tiletype_material;
    switch (mat) {
    case AIR: return 14;
    case GRASS_LIGHT:
    case GRASS_DARK: return 9;
    case GRASS_DRY:
    case GRASS_DEAD: return 11;
    case PLANT:
    case TREE:
    case ROOT:
    case MUSHROOM:
    case DRIFTWOOD: return 10;
    case FROZEN_LIQUID: return 12;
    case MAGMA:
    case FIRE:
    case CAMPFIRE: return 8;
    case POOL:
    case BROOK:
    case RIVER: return 7;
    case SOIL:
    case ASHES: return wall ? 0 : 1;
    case STONE:
    case LAVA_STONE: return wall ? 13 : 2;
    case MINERAL:
    case FEATURE: return wall ? 4 : 2;
    case CONSTRUCTION: return wall ? 6 : 5;
    default: break;
    }

    if (shape == df::tiletype_shape_basic::Open || shape == df::tiletype_shape_basic::None)
        return 14;
    return wall ? 4 : 3;
}

int minimap_column_category(int x, int y, df::world* world, int top_z) {
    if (!world || x < 0 || y < 0)
        return 14;
    int zmax = static_cast<int>(world->map.z_count) - 1;
    top_z = std::max(0, std::min(top_z, zmax));

    for (int z = top_z; z >= 0; --z) {
        df::coord pos(x, y, z);
        auto block = DFHack::Maps::getTileBlock(pos);
        if (!block)
            continue;
        const auto& des = block->designation[x & 15][y & 15];
        auto ttp = DFHack::Maps::getTileType(pos);
        if (!ttp)
            continue;

        auto shape = DFHack::tileShapeBasic(DFHack::tileShape(*ttp));
        if (shape == df::tiletype_shape_basic::Open || shape == df::tiletype_shape_basic::None) {
            if (des.bits.flow_size > 0)
                return des.bits.liquid_type == df::tile_liquid::Magma ? 8 : 7;
            continue;
        }
        return minimap_color_for_tile(*ttp, des);
    }
    return 14;
}

int compute_surface_z(int x, int y, df::world* world) {
    if (!world || x < 0 || y < 0)
        return 0;
    for (int z = static_cast<int>(world->map.z_count) - 1; z >= 0; --z) {
        auto ttp = DFHack::Maps::getTileType(df::coord(x, y, z));
        if (!ttp)
            continue;
        auto basic = DFHack::tileShapeBasic(DFHack::tileShape(*ttp));
        if (basic != df::tiletype_shape_basic::Open && basic != df::tiletype_shape_basic::None)
            return z;
    }
    return 0;
}

int compute_deepest_z(int x, int y, df::world* world) {
    if (!world || x < 0 || y < 0)
        return 0;
    for (int z = 0; z < static_cast<int>(world->map.z_count); ++z) {
        auto block = DFHack::Maps::getTileBlock(df::coord(x, y, z));
        if (!block)
            continue;
        if (!block->designation[x & 15][y & 15].bits.hidden)
            return z;
    }
    return 0;
}

bool build_hud_state(const Camera& camera, HudState& hud, std::string* err) {
    auto world = df::global::world;
    auto plotinfo = df::global::plotinfo;
    if (!world || !plotinfo) {
        if (err) *err = "world/plotinfo unavailable";
        return false;
    }

    hud = HudState{};
    hud.camera = camera;
    hud.map_w = world->map.x_count;
    hud.map_h = world->map.y_count;
    hud.map_z = world->map.z_count;
    effective_viewport_dims(hud.viewport_w, hud.viewport_h);
    hud.paused = df::global::pause_state && *df::global::pause_state;

    if (plotinfo->main.fortress_entity) {
        auto name = DFHack::Translation::translateName(&plotinfo->main.fortress_entity->name);
        if (!name.empty())
            hud.fort_name = name;
    }
    if (plotinfo->main.fortress_site) {
        auto name = DFHack::Translation::translateName(&plotinfo->main.fortress_site->name);
        if (!name.empty())
            hud.site_name = name;
    }
    hud.rank = rank_name(plotinfo->fortress_rank);

    for (auto unit : world->units.active) {
        if (!unit || !DFHack::Units::isCitizen(unit, true))
            continue;
        ++hud.population;
        int cat = std::max(0, std::min(6, DFHack::Units::getStressCategory(unit)));
        ++hud.happiness[6 - cat];
    }

    hud.food = plotinfo->tasks.food.total;
    hud.drink = plotinfo->tasks.food.drink;
    hud.elevation = world->map.region_z + camera.z - 100;

    hud.year = df::global::cur_year ? *df::global::cur_year : 0;
    int year_tick = df::global::cur_year_tick ? *df::global::cur_year_tick : 0;
    int day_of_year = std::max(0, year_tick / 1200);
    hud.month = std::min(11, day_of_year / 28);
    hud.day = (day_of_year % 28) + 1;
    hud.moon_phase = world->world_data ? world->world_data->moon_phase : day_of_year % 28;
    hud.moon_icon = moon_icon_for_phase(hud.moon_phase);
    hud.month_label = month_name(hud.month);
    hud.season_label = season_phase_name(hud.month);

    int mw = std::max(1, static_cast<int>(world->map.x_count));
    int mh = std::max(1, static_cast<int>(world->map.y_count));
    const int cap = 96;
    if (mw >= mh) {
        hud.minimap_w = std::min(cap, mw);
        hud.minimap_h = std::max(1, hud.minimap_w * mh / mw);
    } else {
        hud.minimap_h = std::min(cap, mh);
        hud.minimap_w = std::max(1, hud.minimap_h * mw / mh);
    }

    auto enc = [](int c) -> char {
        c = std::max(0, std::min(14, c));
        return static_cast<char>(c < 10 ? ('0' + c) : ('a' + c - 10));
    };
    hud.minimap.clear();
    hud.minimap.reserve(static_cast<size_t>(hud.minimap_w) * hud.minimap_h);
    for (int gy = 0; gy < hud.minimap_h; ++gy) {
        for (int gx = 0; gx < hud.minimap_w; ++gx) {
            int wx = std::min(mw - 1, static_cast<int>((gx + 0.5) * mw / hud.minimap_w));
            int wy = std::min(mh - 1, static_cast<int>((gy + 0.5) * mh / hud.minimap_h));
            hud.minimap.push_back(enc(minimap_column_category(wx, wy, world, camera.z)));
        }
    }
    hud.surface_z = compute_surface_z(camera.x, camera.y, world);
    hud.deepest_z = compute_deepest_z(camera.x, camera.y, world);
    return true;
}

struct RenderThreadHudRequest {
    Camera camera;
    HudState hud;
    std::string err;
    std::promise<bool> done;
};

} // namespace

bool hud_on_render_thread(const Camera& camera, HudState& hud, std::string* err) {
    std::lock_guard<std::recursive_mutex> lock(g_hud_mutex);

    auto request = std::make_shared<RenderThreadHudRequest>();
    request->camera = camera;
    auto future = request->done.get_future();

    DFHack::runOnRenderThread([request]() {
        request->done.set_value(build_hud_state(request->camera, request->hud, &request->err));
    });

    bool ok = future.get();
    if (!ok) {
        if (err) *err = request->err;
        return false;
    }
    hud = std::move(request->hud);
    return true;
}

std::string hud_json(const std::string& player, const HudState& hud) {
    std::ostringstream body;
    body << "{"
         << "\"player\":" << json_string(player) << ","
         << "\"camera\":{\"x\":" << hud.camera.x
         << ",\"y\":" << hud.camera.y
         << ",\"z\":" << hud.camera.z
         << ",\"zoom\":" << (hud.camera.zoom_factor >= 0 ? hud.camera.zoom_factor : 100)
         << ",\"zoomExplicit\":" << (hud.camera.zoom_factor >= 0 ? "true" : "false") << "},"
         << "\"elevation\":" << hud.elevation << ","
         << "\"map\":{\"w\":" << hud.map_w
         << ",\"h\":" << hud.map_h
         << ",\"z\":" << hud.map_z << "},"
         << "\"viewport\":{\"w\":" << hud.viewport_w
         << ",\"h\":" << hud.viewport_h << "},"
         << "\"paused\":" << (hud.paused ? "true" : "false") << ","
         << "\"fort\":{\"name\":" << json_string(hud.fort_name)
         << ",\"site\":" << json_string(hud.site_name)
         << ",\"rank\":" << json_string(hud.rank) << "},"
         << "\"stocks\":{\"food\":" << hud.food
         << ",\"drink\":" << hud.drink << "},"
         << "\"population\":{\"total\":" << hud.population << "},"
         << "\"happiness\":[" << hud.happiness[0] << "," << hud.happiness[1] << ","
                              << hud.happiness[2] << "," << hud.happiness[3] << ","
                              << hud.happiness[4] << "," << hud.happiness[5] << ","
                              << hud.happiness[6] << "],"
         << "\"date\":{\"year\":" << hud.year
         << ",\"month\":" << hud.month
         << ",\"monthName\":" << json_string(hud.month_label)
         << ",\"day\":" << hud.day
         << ",\"season\":" << json_string(hud.season_label)
         << ",\"moonPhase\":" << hud.moon_phase
         << ",\"moonIcon\":" << hud.moon_icon << "},"
         << "\"minimap\":{\"w\":" << hud.minimap_w
         << ",\"h\":" << hud.minimap_h
         << ",\"surfaceZ\":" << hud.surface_z
         << ",\"deepestZ\":" << hud.deepest_z
         << ",\"cells\":" << json_string(hud.minimap) << "}}\n";
    return body.str();
}

} // namespace dfcapture
