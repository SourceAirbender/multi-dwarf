#pragma once

#include "camera.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dfcapture_public {

struct NotificationReport {
    int32_t id = -1;
    int type = -1;
    int alert_type = 0;
    std::string text;
    int color = 7;
    bool bright = false;
    int32_t duration = 0;
    int32_t repeat_count = 0;
    int32_t year = 0;
    int32_t time = 0;
    bool has_pos = false;
    Camera pos;
};

struct NotificationAlert {
    int type = 0;
    int32_t latest_report_id = -1;
    bool has_target = false;
    Camera target;
    std::vector<NotificationReport> reports;
};

struct NotificationState {
    int32_t next_report_id = 0;
    int32_t report_count = 0;
    std::vector<NotificationAlert> alerts;
    std::vector<NotificationReport> recent;
};

bool notifications_on_render_thread(NotificationState& state, std::string* err = nullptr);
std::string notifications_json(const std::string& player, const NotificationState& state);

} // namespace dfcapture_public
