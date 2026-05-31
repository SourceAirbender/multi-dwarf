#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dfcapture_public {

struct LaborRow {
    int id = 0;
    int32_t portrait_texpos = -1;
    std::string name;
    bool assigned = false;
    int skill = 0;
    std::string skill_label;
    bool specialist = false;
    std::string assigned_to;
};

struct LaborDetail {
    int index = 0;
    std::string name;
    int mode = 0;
    std::string skill_name;
    std::string icon_key;
    bool no_modify = false;
};

struct LaborTask {
    int id = 0;
    std::string key;
    std::string name;
    std::string category_key;
    std::string category;
    int category_order = 0;
    std::string skill_name;
    std::string icon_key;
    bool allowed = false;
};

struct LaborState {
    std::vector<LaborDetail> details;
    int selected = -1;
    std::string selected_skill_name;
    bool selected_no_modify = false;
    std::vector<LaborTask> tasks;
    std::vector<LaborRow> rows;
};

bool build_labor_state(int selected, LaborState& out, std::string* err = nullptr);
std::string labor_json(const LaborState& state);

bool labor_toggle_impl(int detail, int unit_id, bool on, std::string* err = nullptr);
bool labor_mode_impl(int detail, int mode, std::string* err = nullptr);
bool labor_specialist_impl(int unit_id, bool on, std::string* err = nullptr);
bool labor_create_impl(const std::string& requested_name,
                       int* out_index = nullptr,
                       std::string* err = nullptr);
bool labor_rename_impl(int detail, const std::string& requested_name, std::string* err = nullptr);
bool labor_delete_impl(int detail, std::string* err = nullptr);
bool labor_task_toggle_impl(int detail, int labor, bool on, std::string* err = nullptr);

} // namespace dfcapture_public
