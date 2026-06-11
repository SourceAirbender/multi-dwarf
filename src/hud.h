#pragma once

#include "camera.h"

#include <string>

namespace dfcapture {

struct HudState {
    Camera camera;
    int map_w = 0;
    int map_h = 0;
    int map_z = 0;
    int viewport_w = 0;
    int viewport_h = 0;
    bool paused = false;

    std::string fort_name = "Fortress";
    std::string site_name = "Site";
    std::string rank = "Outpost";
    int population = 0;
    int happiness[7] = {};
    int food = 0;
    int drink = 0;

    int elevation = 0;
    int year = 0;
    int month = 0;
    int day = 1;
    int moon_phase = 0;
    int moon_icon = 0;
    std::string month_label = "Granite";
    std::string season_label = "Early Spring";

    std::string minimap;
    int minimap_w = 0;
    int minimap_h = 0;
    int surface_z = 0;
    int deepest_z = 0;
};

bool hud_on_render_thread(const Camera& camera, HudState& hud, std::string* err = nullptr);
std::string hud_json(const std::string& player, const HudState& hud);

} // namespace dfcapture
