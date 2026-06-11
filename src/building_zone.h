#pragma once

#include "httplib.h"

#include "camera.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dfcapture {

struct BuildingPanelInfo {
    int32_t id = -1;
    std::string name;
    bool exists = false;
    bool built = false;
    bool has_jobs = false;
    bool suspended = false;
    bool marked = false;
    bool passage_control = false;
    bool passage_forbidden = false;
    bool passage_closed = false;
};

struct ZonePanelInfo {
    int32_t id = -1;
    bool exists = false;
    std::string name;
    std::string type;
    bool active = false;
    int assigned_units = 0;
    bool is_pit_pond = false;
    bool is_pen = false;
    bool filling_pond = false;
    bool can_owner = false;
    int32_t owner_id = -1;
    std::string owner_name;
    bool can_location = false;
    int32_t location_id = -1;
    std::string location_name;
    std::string location_type;
    bool is_gather = false;
    bool gather_trees = false;
    bool gather_shrubs = false;
    bool gather_fallen = false;
    bool is_tomb = false;
    bool tomb_pets = false;
    bool tomb_citizens = false;
    bool is_archery = false;
    std::string archery_dir;
};

bool building_info_on_core_thread(int32_t id, BuildingPanelInfo& out);
bool building_action_on_core_thread(int32_t id, const std::string& action, std::string* err);
std::string building_info_json(const BuildingPanelInfo& b);

bool zone_info_on_core_thread(int32_t id, ZonePanelInfo& out);
bool zone_action_on_core_thread(int32_t id, const std::string& action, std::string* err);
std::string zone_info_json(const ZonePanelInfo& z);

std::string zone_units_json_on_core_thread(int32_t zone_id, std::string* err = nullptr);
bool zone_unit_action_on_core_thread(int32_t zone_id, int32_t unit_id, bool assign,
                                     const std::string& kind, std::string* err);

std::string zone_owners_json_on_core_thread(int32_t zone_id, std::string* err = nullptr);
bool zone_owner_action_on_core_thread(int32_t zone_id, int32_t unit_id, std::string* err);
std::string zones_json_on_core_thread(const std::string& player, const Camera& camera,
                                      std::string* err = nullptr);

} // namespace dfcapture
