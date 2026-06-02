#pragma once

#include "camera.h"

#include <cstdint>
#include <string>

namespace dfcapture_public {

bool lua_ping(int value, int& out_value, std::string* err = nullptr);

std::string building_catalog_json_via_lua(std::string* err = nullptr);
std::string build_materials_json_via_lua(const std::string& token, std::string* err = nullptr);
bool place_building_via_lua(const Camera& camera, int px, int py, int px2, int py2,
                            int frame_w, int frame_h, const std::string& token,
                            int direction, const std::string& options,
                            int& out_count, int& out_id, std::string* err = nullptr);

bool create_stockpile_via_lua(const Camera& camera, int px, int py, int px2, int py2,
                              int frame_w, int frame_h, const std::string& preset,
                              int& out_id, std::string* err = nullptr);
bool create_zone_via_lua(const Camera& camera, int px, int py, int px2, int py2,
                         int frame_w, int frame_h, const std::string& zone_type,
                         int& out_id, std::string* err = nullptr);

std::string stockpile_groups_via_lua(const std::string& cat, std::string* err = nullptr);
std::string stockpile_items_via_lua(int32_t id, const std::string& cat,
                                    const std::string& group, std::string* err = nullptr);
bool stockpile_toggle_item_via_lua(int32_t id, const std::string& cat,
                                   const std::string& group, int idx, bool on,
                                   std::string* err = nullptr);
bool stockpile_toggle_all_via_lua(int32_t id, const std::string& cat,
                                  const std::string& group, bool on,
                                  std::string* err = nullptr);

std::string workshop_info_json_via_lua(int32_t id, std::string* err = nullptr);
bool workshop_add_job_via_lua(int32_t id, const std::string& task, std::string* err = nullptr);
bool workshop_job_action_via_lua(int32_t id, int32_t job_id, const std::string& action,
                                 std::string* err = nullptr);
bool workshop_worker_action_via_lua(int32_t id, int32_t unit_id, bool assign,
                                    std::string* err = nullptr);
bool workshop_workers_clear_via_lua(int32_t id, std::string* err = nullptr);

std::string zone_locations_json_via_lua(int32_t zone_id, std::string* err = nullptr);
bool zone_location_action_via_lua(int32_t zone_id, const std::string& action,
                                  const std::string& kind, int32_t location_id,
                                  std::string* err = nullptr);

} // namespace dfcapture_public
