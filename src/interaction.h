#pragma once

#include "camera.h"
#include "unit_sheet.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dfcapture_public {

struct InspectResult {
    Camera camera;
    int map_x = 0;
    int map_y = 0;
    int map_z = 0;
    int px = 0;
    int py = 0;
    int tile_px = 0;
    int tile_py = 0;
    std::string kind = "tile";
    std::string title;
    std::vector<std::string> lines;
    int32_t building_id = -1;
    int32_t item_id = -1;
    UnitSheet unit;
};

struct HoverResult {
    int map_x = 0;
    int map_y = 0;
    int map_z = 0;
    std::string material;
    std::vector<std::string> lines;
};

struct StockItemActionResult {
    bool ok = false;
    bool has_camera = false;
    Camera camera;
    bool has_map_pos = false;
    int map_x = 0;
    int map_y = 0;
    int map_z = 0;
    int32_t holder_unit_id = -1;
    std::string holder_unit_name;
    int32_t owner_unit_id = -1;
    std::string owner_unit_name;
    std::string location;
    std::string description;
    std::string title;
    std::string weight;
    bool forbidden = false;
    bool dump = false;
    bool hidden = false;
    std::vector<std::string> lines;
    std::vector<std::pair<int32_t, std::string>> contents;
    std::string err;
};

bool action_on_core_thread(const std::string& action, std::string* err = nullptr);

bool stock_item_action_on_core_thread(int32_t item_id,
                                      const std::string& action,
                                      StockItemActionResult& result);

bool inspect_on_core_thread(const Camera& camera,
                            int px,
                            int py,
                            int frame_w,
                            int frame_h,
                            InspectResult& result,
                            std::string* err = nullptr);

bool hover_on_core_thread(const Camera& camera,
                          int px,
                          int py,
                          int frame_w,
                          int frame_h,
                          HoverResult& result,
                          std::string* err = nullptr);

std::string inspect_json(const std::string& player, const InspectResult& result);
std::string hover_json(const std::string& player, const HoverResult& result);
std::string stock_item_action_json(int32_t item_id, const StockItemActionResult& result);

} // namespace dfcapture_public
