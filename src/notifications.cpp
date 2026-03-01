#include "notifications.h"

#include "json_util.h"
#include "modules/DFSDL.h"

#include "df/announcement_alert_type.h"
#include "df/announcement_alertst.h"
#include "df/announcement_handlerst.h"
#include "df/announcement_type.h"
#include "df/global_objects.h"
#include "df/report.h"
#include "df/world.h"

#include <algorithm>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace dfcapture_public {
namespace {

std::recursive_mutex g_notifications_mutex;

bool valid_pos(df::world* world, const df::coord& pos) {
    return world && pos.x >= 0 && pos.y >= 0 && pos.z >= 0 &&
        pos.x < world->map.x_count &&
        pos.y < world->map.y_count &&
        pos.z < world->map.z_count;
}

int alert_type_for_report(df::report* report) {
    if (!report)
        return static_cast<int>(df::announcement_alert_type::GENERAL);
    int type = static_cast<int>(report->type);
    if (!df::enum_traits<df::announcement_type>::is_valid(type))
        return static_cast<int>(df::announcement_alert_type::GENERAL);
    return static_cast<int>(df::enum_traits<df::announcement_type>::attrs(report->type).alert_type);
}

NotificationReport copy_report(df::world* world, df::report* report) {
    NotificationReport out;
    if (!report)
        return out;

    out.id = report->id;
    out.type = static_cast<int>(report->type);
    out.alert_type = alert_type_for_report(report);
    out.text = report->text;
    out.color = report->color;
    out.bright = report->bright;
    out.duration = report->duration;
    out.repeat_count = report->repeat_count;
    out.year = report->year;
    out.time = report->time;
    out.has_pos = valid_pos(world, report->pos);
    if (out.has_pos)
        out.pos = Camera{report->pos.x, report->pos.y, report->pos.z};
    return out;
}

NotificationAlert& ensure_alert(std::vector<NotificationAlert>& alerts, int type) {
    auto it = std::find_if(alerts.begin(), alerts.end(),
        [type](const NotificationAlert& alert) { return alert.type == type; });
    if (it != alerts.end())
        return *it;

    NotificationAlert alert;
    alert.type = type;
    alerts.push_back(alert);
    return alerts.back();
}

void add_report_to_alert(NotificationAlert& alert, const NotificationReport& report) {
    if (report.id >= 0) {
        auto exists = std::find_if(alert.reports.begin(), alert.reports.end(),
            [&](const NotificationReport& row) { return row.id == report.id; });
        if (exists != alert.reports.end())
            return;
    }

    alert.reports.push_back(report);
    if (report.id > alert.latest_report_id)
        alert.latest_report_id = report.id;
    if (report.has_pos && (!alert.has_target || report.id >= alert.latest_report_id)) {
        alert.has_target = true;
        alert.target = report.pos;
    }
}

bool build_notifications(NotificationState& state, std::string* err) {
    auto world = df::global::world;
    if (!world) {
        if (err) *err = "world unavailable";
        return false;
    }

    state = NotificationState{};
    state.next_report_id = world->status.next_report_id;
    state.report_count = static_cast<int32_t>(world->status.reports.size());

    std::unordered_map<int32_t, df::report*> reports_by_id;
    reports_by_id.reserve(world->status.reports.size());
    for (auto report : world->status.reports) {
        if (report)
            reports_by_id[report->id] = report;
    }

    for (auto report_id : world->status.alert_button_announcement_id) {
        auto it = reports_by_id.find(report_id);
        if (it == reports_by_id.end())
            continue;
        auto copied = copy_report(world, it->second);
        auto& alert = ensure_alert(state.alerts, copied.alert_type);
        add_report_to_alert(alert, copied);
    }

    for (auto alert_ptr : world->status.announcement_alert) {
        if (!alert_ptr || alert_ptr->type == df::announcement_alert_type::NONE)
            continue;
        auto& alert = ensure_alert(state.alerts, static_cast<int>(alert_ptr->type));
        for (auto report_id : alert_ptr->announcement_id) {
            auto it = reports_by_id.find(report_id);
            if (it == reports_by_id.end())
                continue;
            add_report_to_alert(alert, copy_report(world, it->second));
        }
    }

    std::sort(state.alerts.begin(), state.alerts.end(),
        [](const NotificationAlert& a, const NotificationAlert& b) {
            return a.latest_report_id > b.latest_report_id;
        });
    if (state.alerts.size() > 12)
        state.alerts.resize(12);

    const size_t recent_limit = 24;
    size_t start = world->status.reports.size() > recent_limit
        ? world->status.reports.size() - recent_limit
        : 0;
    for (size_t i = world->status.reports.size(); i-- > start;) {
        if (auto report = world->status.reports[i])
            state.recent.push_back(copy_report(world, report));
    }
    return true;
}

void append_camera_or_null(std::ostringstream& body, bool has_pos, const Camera& pos) {
    if (!has_pos) {
        body << "null";
        return;
    }
    body << "{\"x\":" << pos.x << ",\"y\":" << pos.y << ",\"z\":" << pos.z << "}";
}

void append_report_json(std::ostringstream& body, const NotificationReport& report) {
    body << "{\"id\":" << report.id
         << ",\"type\":" << report.type
         << ",\"alertType\":" << report.alert_type
         << ",\"text\":" << json_string(report.text)
         << ",\"color\":" << report.color
         << ",\"bright\":" << (report.bright ? "true" : "false")
         << ",\"duration\":" << report.duration
         << ",\"repeatCount\":" << report.repeat_count
         << ",\"year\":" << report.year
         << ",\"time\":" << report.time
         << ",\"pos\":";
    append_camera_or_null(body, report.has_pos, report.pos);
    body << "}";
}

struct RenderThreadNotificationsRequest {
    NotificationState state;
    std::string err;
    std::promise<bool> done;
};

} // namespace

bool notifications_on_render_thread(NotificationState& state, std::string* err) {
    std::lock_guard<std::recursive_mutex> lock(g_notifications_mutex);

    auto request = std::make_shared<RenderThreadNotificationsRequest>();
    auto future = request->done.get_future();

    DFHack::runOnRenderThread([request]() {
        request->done.set_value(build_notifications(request->state, &request->err));
    });

    bool ok = future.get();
    if (!ok) {
        if (err) *err = request->err;
        return false;
    }
    state = std::move(request->state);
    return true;
}

std::string notifications_json(const std::string& player, const NotificationState& state) {
    std::ostringstream body;
    body << "{\"player\":" << json_string(player)
         << ",\"nextReportId\":" << state.next_report_id
         << ",\"reportCount\":" << state.report_count
         << ",\"alerts\":[";
    for (size_t i = 0; i < state.alerts.size(); ++i) {
        if (i) body << ",";
        const auto& alert = state.alerts[i];
        body << "{\"type\":" << alert.type
             << ",\"latestReportId\":" << alert.latest_report_id
             << ",\"target\":";
        append_camera_or_null(body, alert.has_target, alert.target);
        body << ",\"reports\":[";
        for (size_t j = 0; j < alert.reports.size(); ++j) {
            if (j) body << ",";
            append_report_json(body, alert.reports[j]);
        }
        body << "]}";
    }
    body << "],\"recent\":[";
    for (size_t i = 0; i < state.recent.size(); ++i) {
        if (i) body << ",";
        append_report_json(body, state.recent[i]);
    }
    body << "]}\n";
    return body.str();
}

} // namespace dfcapture_public
