#include "notifications.h"

#include "json_util.h"
#include "MiscUtils.h"
#include "modules/DFSDL.h"
#include "modules/Units.h"

#include "df/announcement_alert_type.h"
#include "df/announcement_alertst.h"
#include "df/announcement_handlerst.h"
#include "df/announcement_type.h"
#include "df/global_objects.h"
#include "df/report.h"
#include "df/unit.h"
#include "df/unit_report_type.h"
#include "df/world.h"

#include <algorithm>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace dfcapture {
namespace {

using namespace DFHack;

std::recursive_mutex g_notifications_mutex;
std::mutex g_dismissed_mutex;
std::unordered_map<std::string, std::unordered_set<std::string>> g_dismissed_alert_keys;

bool valid_pos(df::world* world, const df::coord& pos) {
    return world && pos.x >= 0 && pos.y >= 0 && pos.z >= 0 &&
        pos.x < world->map.x_count &&
        pos.y < world->map.y_count &&
        pos.z < world->map.z_count;
}

bool valid_pos(const df::coord& pos) {
    return valid_pos(df::global::world, pos);
}

std::string report_dismiss_key(int32_t report_id) {
    return "r:" + std::to_string(report_id);
}

std::string unit_report_dismiss_key(int32_t unit_id, int category) {
    return "u:" + std::to_string(unit_id) + ":" + std::to_string(category);
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
    out.type_key = DFHack::enum_item_key(report->type);
    out.text = report->text;
    out.color = report->color;
    out.bright = report->bright;
    out.duration = report->duration;
    out.repeat_count = report->repeat_count;
    out.continuation = report->flags.bits.continuation;
    out.announcement = report->flags.bits.announcement;
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
    if (type < 0 || type > df::enum_traits<df::announcement_alert_type>::last_item_value)
        type = static_cast<int>(df::announcement_alert_type::GENERAL);
    alert.type = type;
    alert.type_key = DFHack::enum_item_key(static_cast<df::announcement_alert_type>(type));
    alert.dismiss_key = "a:" + std::to_string(type);
    alerts.push_back(alert);
    return alerts.back();
}

void add_report_to_alert(NotificationAlert& alert, const NotificationReport& report) {
    if (report.id >= 0) {
        if (std::find(alert.report_ids.begin(), alert.report_ids.end(), report.id) == alert.report_ids.end())
            alert.report_ids.push_back(report.id);
        std::string dismiss_key = report_dismiss_key(report.id);
        if (std::find(alert.dismiss_keys.begin(), alert.dismiss_keys.end(), dismiss_key) == alert.dismiss_keys.end())
            alert.dismiss_keys.push_back(dismiss_key);
    }
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

bool all_alert_keys_dismissed(const NotificationAlert& alert,
                              const std::unordered_set<std::string>& dismissed) {
    if (alert.dismiss_keys.empty())
        return false;
    for (const auto& key : alert.dismiss_keys) {
        if (dismissed.find(key) == dismissed.end())
            return false;
    }
    return true;
}

bool build_notifications(const std::unordered_set<std::string>& dismissed,
                         NotificationState& state,
                         std::string* err) {
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
            if (it != reports_by_id.end()) {
                add_report_to_alert(alert, copy_report(world, it->second));
            } else {
                if (std::find(alert.report_ids.begin(), alert.report_ids.end(), report_id) == alert.report_ids.end())
                    alert.report_ids.push_back(report_id);
                std::string dismiss_key = report_dismiss_key(report_id);
                if (std::find(alert.dismiss_keys.begin(), alert.dismiss_keys.end(), dismiss_key) == alert.dismiss_keys.end())
                    alert.dismiss_keys.push_back(dismiss_key);
            }
        }

        const size_t n = std::min(alert_ptr->report_unid.size(),
                                  alert_ptr->report_unit_announcement_category.size());
        for (size_t i = 0; i < n; ++i) {
            int32_t unit_id = alert_ptr->report_unid[i];
            int category = static_cast<int>(alert_ptr->report_unit_announcement_category[i]);
            std::string dismiss_key = unit_report_dismiss_key(unit_id, category);
            if (std::find(alert.dismiss_keys.begin(), alert.dismiss_keys.end(), dismiss_key) == alert.dismiss_keys.end())
                alert.dismiss_keys.push_back(dismiss_key);
            if (std::find_if(alert.unit_refs.begin(), alert.unit_refs.end(),
                    [&](const NotificationUnitRef& ref) { return ref.dismiss_key == dismiss_key; }) != alert.unit_refs.end())
                continue;

            NotificationUnitRef ref;
            ref.unit_id = unit_id;
            ref.category = category;
            ref.category_key = DFHack::enum_item_key(alert_ptr->report_unit_announcement_category[i]);
            ref.dismiss_key = dismiss_key;
            if (auto unit = df::unit::find(unit_id)) {
                ref.unit_name = Units::getReadableName(unit);
                auto pos = Units::getPosition(unit);
                ref.has_pos = valid_pos(pos);
                if (ref.has_pos)
                    ref.pos = Camera{pos.x, pos.y, pos.z};
                if (category >= 0 && category <= df::enum_traits<df::unit_report_type>::last_item_value) {
                    auto& log = unit->reports.log[category];
                    size_t start = log.size() > 12 ? log.size() - 12 : 0;
                    for (size_t j = start; j < log.size(); ++j) {
                        auto it = reports_by_id.find(log[j]);
                        if (it == reports_by_id.end())
                            continue;
                        add_report_to_alert(alert, copy_report(world, it->second));
                        ref.reports.push_back(copy_report(world, it->second));
                    }
                }
            }
            alert.unit_refs.push_back(std::move(ref));
        }
    }

    std::sort(state.alerts.begin(), state.alerts.end(),
        [](const NotificationAlert& a, const NotificationAlert& b) {
            return a.latest_report_id > b.latest_report_id;
        });
    if (state.alerts.size() > 12)
        state.alerts.resize(12);
    state.alerts.erase(std::remove_if(state.alerts.begin(), state.alerts.end(),
        [&](const NotificationAlert& alert) { return all_alert_keys_dismissed(alert, dismissed); }),
        state.alerts.end());

    const size_t recent_limit = 250;
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
         << ",\"typeKey\":" << json_string(report.type_key)
         << ",\"text\":" << json_string(report.text)
         << ",\"color\":" << report.color
         << ",\"bright\":" << (report.bright ? "true" : "false")
         << ",\"duration\":" << report.duration
         << ",\"repeatCount\":" << report.repeat_count
         << ",\"continuation\":" << (report.continuation ? "true" : "false")
         << ",\"announcement\":" << (report.announcement ? "true" : "false")
         << ",\"year\":" << report.year
         << ",\"time\":" << report.time
         << ",\"pos\":";
    append_camera_or_null(body, report.has_pos, report.pos);
    body << "}";
}

struct RenderThreadNotificationsRequest {
    std::unordered_set<std::string> dismissed;
    NotificationState state;
    std::string err;
    std::promise<bool> done;
};

} // namespace

bool notifications_on_render_thread(const std::unordered_set<std::string>& dismissed,
                                    NotificationState& state,
                                    std::string* err) {
    std::lock_guard<std::recursive_mutex> lock(g_notifications_mutex);

    auto request = std::make_shared<RenderThreadNotificationsRequest>();
    request->dismissed = dismissed;
    auto future = request->done.get_future();

    DFHack::runOnRenderThread([request]() {
        request->done.set_value(build_notifications(request->dismissed,
                                                    request->state,
                                                    &request->err));
    });

    bool ok = future.get();
    if (!ok) {
        if (err) *err = request->err;
        return false;
    }
    state = std::move(request->state);
    return true;
}

bool notifications_on_render_thread(NotificationState& state, std::string* err) {
    static const std::unordered_set<std::string> dismissed;
    return notifications_on_render_thread(dismissed, state, err);
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
             << ",\"typeKey\":" << json_string(alert.type_key)
             << ",\"iconIndex\":" << alert.type
             << ",\"dismissKey\":" << json_string(alert.dismiss_key)
             << ",\"latestReportId\":" << alert.latest_report_id
             << ",\"target\":";
        append_camera_or_null(body, alert.has_target, alert.target);
        body << ",\"dismissKeys\":[";
        for (size_t j = 0; j < alert.dismiss_keys.size(); ++j) {
            if (j) body << ",";
            body << json_string(alert.dismiss_keys[j]);
        }
        body << "],\"reportIds\":[";
        for (size_t j = 0; j < alert.report_ids.size(); ++j) {
            if (j) body << ",";
            body << alert.report_ids[j];
        }
        body << "],\"reports\":[";
        for (size_t j = 0; j < alert.reports.size(); ++j) {
            if (j) body << ",";
            append_report_json(body, alert.reports[j]);
        }
        body << "],\"unitReports\":[";
        for (size_t u = 0; u < alert.unit_refs.size(); ++u) {
            if (u) body << ",";
            const auto& ref = alert.unit_refs[u];
            body << "{\"unitId\":" << ref.unit_id
                 << ",\"category\":" << ref.category
                 << ",\"categoryKey\":" << json_string(ref.category_key)
                 << ",\"unitName\":" << json_string(ref.unit_name)
                 << ",\"dismissKey\":" << json_string(ref.dismiss_key)
                 << ",\"pos\":";
            append_camera_or_null(body, ref.has_pos, ref.pos);
            body << ",\"reports\":[";
            for (size_t r = 0; r < ref.reports.size(); ++r) {
                if (r) body << ",";
                append_report_json(body, ref.reports[r]);
            }
            body << "]}";
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

void remember_dismissed_alert_keys(const std::string& player, const std::string& raw_keys) {
    std::lock_guard<std::mutex> lock(g_dismissed_mutex);
    auto& dismissed = g_dismissed_alert_keys[player];
    size_t start = 0;
    while (start <= raw_keys.size()) {
        size_t comma = raw_keys.find(',', start);
        std::string key = raw_keys.substr(start, comma == std::string::npos
            ? std::string::npos
            : comma - start);
        if (!key.empty() && key.size() <= 64)
            dismissed.insert(key);
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
}

std::unordered_set<std::string> dismissed_alert_keys_for_player(const std::string& player) {
    std::lock_guard<std::mutex> lock(g_dismissed_mutex);
    auto it = g_dismissed_alert_keys.find(player);
    if (it == g_dismissed_alert_keys.end())
        return {};
    return it->second;
}

} // namespace dfcapture
