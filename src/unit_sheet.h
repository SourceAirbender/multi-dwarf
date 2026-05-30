#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "camera.h"

namespace df {
struct unit;
}

namespace dfcapture_public {

struct UnitAction {
    std::string hotkey;
    std::string label;
    std::string value;
    bool available = false;
};

struct UnitSheet {
    bool present = false;
    int32_t id = -1;
    int32_t portrait_texpos = -1;
    int32_t sheet_icon_texpos = -1;
    std::string name;
    std::string race;
    std::string profession;
    std::string current_job;
    std::string age;
    std::string sex;
    std::string status;
    std::string training;
    std::string body_summary;
    std::vector<std::string> overview_relation_lines;
    std::vector<std::string> overview_trait_lines;
    std::vector<std::string> overview_position_lines;
    std::vector<std::string> overview_squad_lines;
    std::vector<std::string> overview_skill_lines;
    std::vector<std::string> overview_need_lines;
    std::vector<std::string> overview_memory_lines;
    std::vector<std::string> flags;
    std::vector<std::string> status_lines;
    std::vector<std::string> inventory_lines;
    std::vector<std::string> health_lines;
    std::vector<std::string> health_status_lines;
    std::vector<std::string> health_wound_lines;
    std::vector<std::string> health_treatment_lines;
    std::vector<std::string> health_history_lines;
    std::vector<std::string> health_description_lines;
    std::vector<std::string> skill_lines;
    std::vector<std::string> room_lines;
    std::vector<std::string> room_assignment_lines;
    std::vector<std::string> labor_lines;
    std::vector<std::string> labor_work_detail_lines;
    std::vector<std::string> labor_workshop_lines;
    std::vector<std::string> labor_location_lines;
    std::vector<std::string> labor_work_animal_lines;
    std::vector<std::string> relation_lines;
    std::vector<std::string> group_lines;
    std::vector<std::string> military_lines;
    std::vector<std::string> military_squad_lines;
    std::vector<std::string> military_uniform_lines;
    std::vector<std::string> military_kill_lines;
    std::vector<std::string> thought_lines;
    std::vector<std::string> personality_lines;
    std::vector<std::string> personality_trait_lines;
    std::vector<std::string> personality_value_lines;
    std::vector<std::string> personality_preference_lines;
    std::vector<std::string> personality_need_lines;
    std::vector<UnitAction> actions;
};

UnitSheet build_unit_sheet(df::unit* unit);

std::string unit_sheet_json(const std::string& player,
                            const UnitSheet& unit,
                            const Camera& tile);

bool unit_sheet_on_render_thread(int32_t unit_id,
                                 UnitSheet& unit,
                                 Camera& tile,
                                 std::string* err = nullptr);

} // namespace dfcapture_public
