#pragma once

#include <cstdint>
#include <string>

namespace dfcapture_public {

std::string stockpile_info_json_on_core_thread(int32_t id);
bool rename_stockpile_on_core_thread(int32_t id, const std::string& name);
bool remove_stockpile_on_core_thread(int32_t id);
bool set_stockpile_links_only_on_core_thread(int32_t id, bool on);
bool set_stockpile_storage_on_core_thread(int32_t id, int barrels, int bins, int wheelbarrows);
bool set_stockpile_link_on_core_thread(int32_t id, int32_t target_id, const std::string& mode,
                                       bool on, std::string* err);
bool set_stockpile_category_on_core_thread(int32_t id, const std::string& preset,
                                           const std::string& mode, std::string* err);
bool finish_stockpile_repaint_on_core_thread(int32_t old_id, int32_t new_id,
                                             int32_t& final_id, std::string* err);

} // namespace dfcapture_public
