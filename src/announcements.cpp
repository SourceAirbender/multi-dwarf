// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// Copyright (C) 2026 Jake Taplin
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, version 3 of the License.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// Runs on DFHack (Zlib); descends from DFPlex (Zlib) and webfort (ISC).
// Full license: see LICENSE. Third-party credits: see NOTICE.
//
// SPDX-License-Identifier: AGPL-3.0-only

#include "announcements.h"

#include "announce_taxonomy.gen.h"
#include "json_util.h"
#include "render_thread_wait.h"
#include "save_barrier.h"
#include "modules/DFSDL.h"
#include "modules/Translation.h"

#include "df/announcement_alert_type.h"
#include "df/announcement_type.h"
#include "df/global_objects.h"
#include "df/historical_figure.h"
#include "df/report.h"
#include "df/report_zoom_type.h"
#include "df/unit.h"
#include "df/unit_report_type.h"
#include "df/world.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace dfcapture {
namespace {

std::recursive_mutex g_reports_mutex;

// /notifications exposes the active alert stack and a flat recent-report list. This read-only
// provider adds categorized paging and combat-log detail from the same df::report source.

int alert_type_for_report(df::report* report) {
    if (!report)
        return static_cast<int>(df::announcement_alert_type::GENERAL);
    int type = static_cast<int>(report->type);
    if (!df::enum_traits<df::announcement_type>::is_valid(type))
        return static_cast<int>(df::announcement_alert_type::GENERAL);
    return static_cast<int>(df::enum_traits<df::announcement_type>::attrs(report->type).alert_type);
}


std::string resolve_histfig_references(std::string text) {
    const std::string marker = "HF ";
    size_t pos = 0;
    while ((pos = text.find(marker, pos)) != std::string::npos) {
        const size_t digits_start = pos + marker.size();
        size_t end = digits_start;
        while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])))
            ++end;
        if (end == digits_start) {
            pos = end;
            continue;
        }
        char* parse_end = nullptr;
        const long id = std::strtol(text.c_str() + digits_start, &parse_end, 10);
        if (parse_end != text.c_str() + end || id < 0 ||
            id > std::numeric_limits<int32_t>::max()) {
            pos = end;
            continue;
        }
        auto hf = df::historical_figure::find(static_cast<int32_t>(id));
        const std::string name = hf ? DFHack::Translation::translateName(&hf->name, true) : "";
        if (name.empty()) {
            pos = end;
            continue;
        }
        text.replace(pos, end - pos, name);
        pos += name.size();
    }
    return text;
}

bool valid_pos(df::world* world, const df::coord& pos) {
    return world && pos.x >= 0 && pos.y >= 0 && pos.z >= 0 &&
        pos.x < world->map.x_count &&
        pos.y < world->map.y_count &&
        pos.z < world->map.z_count;
}

ReportEntry copy_report_entry(df::world* world, df::report* report) {
    ReportEntry out;
    if (!report)
        return out;
    out.id = report->id;
    out.type = static_cast<int>(report->type);
    out.alert_type = alert_type_for_report(report);
    out.type_key = DFHack::enum_item_key(report->type);
    out.text = resolve_histfig_references(report->text);
    out.color = report->color;
    out.bright = report->bright;
    out.duration = report->duration;
    out.repeat_count = report->repeat_count;
    out.continuation = report->flags.bits.continuation;
    out.announcement = report->flags.bits.announcement;
    out.year = report->year;
    out.time = report->time;
    out.zoom_type = static_cast<int>(report->zoom_type);
    out.has_pos = report->zoom_type != df::report_zoom_type::NONE && valid_pos(world, report->pos);
    if (out.has_pos)
        out.pos = Camera{report->pos.x, report->pos.y, report->pos.z};
    out.zoom_type2 = static_cast<int>(report->zoom_type2);
    out.has_pos2 = report->zoom_type2 != df::report_zoom_type::NONE && valid_pos(world, report->pos2);
    if (out.has_pos2)
        out.pos2 = Camera{report->pos2.x, report->pos2.y, report->pos2.z};
    out.activity_id = report->activity_id;
    out.activity_event_id = report->activity_event_id;
    out.speaker_id = report->speaker_id;
    out.section = taxonomy::section_for(out.type, out.alert_type);
    out.taxonomy_flags = taxonomy::flags_for(out.type);
    return out;
}

// Classification costs two array indexes and, only for a Misc
// row) a walk of a 16-entry rescue table. No allocation, no string work, no DF call -- which is the
// entire reason the taxonomy is BAKED (announce_taxonomy.gen.h) rather than parsed at runtime.
// The EXPENSIVE part of a page is copy_report_entry (translateName + std::string), and that is
// bounded by max_reports, never by the size of the fort's log.
inline bool report_matches(df::report* report, const ReportsQuery& query) {
    const int type = static_cast<int>(report->type);
    const int alert = alert_type_for_report(report);
    if (query.category >= 0 && alert != query.category)
        return false;
    if (query.section >= 0 && taxonomy::section_for(type, alert) != query.section)
        return false;
    return true;
}

bool build_reports_page(const ReportsQuery& query, ReportsPage& page, std::string* err) {
    auto world = df::global::world;
    if (!world) {
        if (err) *err = "world unavailable";
        return false;
    }
    page = ReportsPage{};
    page.query = query;
    page.next_report_id = world->status.next_report_id;

    const auto& reports = world->status.reports;
    page.total_reports = static_cast<int32_t>(reports.size());
    page.next_before_id = query.before_id;

    // Optional counts pass. O(N) but each entry costs the two array indexes above -- no copies,
    // no strings. This is what the chips are built from, and the client asks for it ONCE (on open),
    // not on every 2s poll.
    if (query.want_counts) {
        page.has_counts = true;
        for (size_t i = 0; i < reports.size(); ++i) {
            auto report = reports[i];
            if (!report || report->flags.bits.continuation)
                continue; // count MESSAGES, not the wrapped lines that make them up
            const int type = static_cast<int>(report->type);
            const uint8_t section = taxonomy::section_for(type, alert_type_for_report(report));
            if (section < taxonomy::SECTION_COUNT)
                ++page.section_counts[section];
        }
    }

    // Reports are id-ordered ascending in this vector. Walk BACKWARD from `before_id` (or from the
    // end), collecting newest-first, then reverse. Backward is the right direction for both jobs:
    // the newest page is the one the screen opens on, and "load older" is just another step back.
    //
    // CONTINUATION TAILS. DF wraps a long message into a lead report followed by continuations.
    // Only a lead can match, and when one does we pull its whole continuation run
    // forward with it. Tails do NOT count against max_reports: a message arrives whole or not at all.
    std::vector<ReportEntry> collected; // newest-first, each element a whole message's worth
    int matched_leads = 0;
    for (size_t i = reports.size(); i-- > 0;) {
        auto report = reports[i];
        if (!report)
            continue; // a culled hole
        if (query.before_id >= 0 && report->id >= query.before_id)
            continue; // not yet inside the requested window
        if (report->id <= query.since_id)
            break;    // walked back past the caller's lower bound; nothing older can qualify
        if (page.scanned >= query.scan_budget) {
            page.budget_exhausted = true;
            break;
        }

        const bool is_candidate =
            !report->flags.bits.continuation && report_matches(report, query);

        // ORDER IS LOAD-BEARING. The full-page break happens BEFORE this entry is consumed, so
        // next_before_id is never advanced past a report we did not return. Advancing it here would
        // silently drop that report because the next `before=` page starts below it.
        if (is_candidate && matched_leads >= query.max_reports) {
            page.truncated = true;
            break;
        }

        ++page.scanned;
        page.next_before_id = report->id; // oldest CONSUMED so far -- the resume cursor
        if (i == 0)
            page.reached_oldest = true;   // we walked all the way to the front of the vector

        if (!is_candidate)
            continue;
        ++matched_leads;

        // The lead, then its continuation tail in forward order -- pushed reversed here because
        // `collected` is newest-first and gets reversed wholesale below. Tails are FREE: they do
        // not count against max_reports, so a message always arrives whole.
        std::vector<ReportEntry> message;
        message.push_back(copy_report_entry(world, report));
        for (size_t j = i + 1; j < reports.size(); ++j) {
            auto tail = reports[j];
            if (!tail || !tail->flags.bits.continuation)
                break;
            message.push_back(copy_report_entry(world, tail));
        }
        for (size_t k = message.size(); k-- > 0;)
            collected.push_back(std::move(message[k]));
    }
    if (reports.empty())
        page.reached_oldest = true;

    page.reports.assign(collected.rbegin(), collected.rend());
    return true;
}

struct RenderThreadReportsRequest {
    ReportsQuery query;
    ReportsPage page;
    std::string err;
    std::promise<bool> done;
};

void append_camera_or_null(std::ostringstream& body, bool has_pos, const Camera& pos) {
    if (!has_pos) {
        body << "null";
        return;
    }
    body << "{\"x\":" << pos.x << ",\"y\":" << pos.y << ",\"z\":" << pos.z << "}";
}

void append_report_entry_json(std::ostringstream& body, const ReportEntry& report) {
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
         << ",\"zoomType\":" << report.zoom_type
         << ",\"hasPos\":" << (report.has_pos ? "true" : "false")
         << ",\"pos\":";
    append_camera_or_null(body, report.has_pos, report.pos);
    body << ",\"zoomType2\":" << report.zoom_type2
         << ",\"hasPos2\":" << (report.has_pos2 ? "true" : "false")
         << ",\"pos2\":";
    append_camera_or_null(body, report.has_pos2, report.pos2);
    body << ",\"activityId\":" << report.activity_id
         << ",\"activityEventId\":" << report.activity_event_id
         << ",\"speakerId\":" << report.speaker_id;
    // Resolve the section server-side so the client never has to derive it.
    // (and so `section=` filtering + paging can agree with what the rows actually say). `box` and
    // `alert` are the two flags the screen renders as badges -- DF's own "this one stopped the game"
    // and "this one lit the alert button" marks.
    const int section = (report.section >= 0 && report.section < taxonomy::SECTION_COUNT)
        ? report.section : taxonomy::SECTION_MISC;
    body << ",\"section\":" << json_string(taxonomy::SECTION_INFO[section].key)
         << ",\"sectionId\":" << section
         << ",\"taxonomyFlags\":" << report.taxonomy_flags
         << ",\"box\":" << ((report.taxonomy_flags & taxonomy::FLAG_BOX) ? "true" : "false")
         << ",\"alert\":" << ((report.taxonomy_flags & taxonomy::FLAG_ALERT) ? "true" : "false");
    body << "}";
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string unit_log_key(int log_type) {
    if (!df::enum_traits<df::unit_report_type>::is_valid(log_type))
        return "";
    return DFHack::enum_item_key(static_cast<df::unit_report_type>(log_type));
}

// COMBAT-LOG DEPTH: resolve a unit's Combat/Sparring/Hunting logs into whole (continuation-joined)
// report entries. unit.reports.log[type] holds LEAD report ids only; we walk world.status.reports
// once, opening a run at each wanted lead and attaching its continuation tail so multi-line combat
// messages arrive complete. max_reports caps the number of LEADS (tails never count against it).
bool build_unit_reports_page(int32_t unit_id, int log_filter, int32_t since_id,
                             int max_reports, UnitReportsPage& page, std::string* err) {
    auto world = df::global::world;
    if (!world) {
        if (err) *err = "world unavailable";
        return false;
    }
    page = UnitReportsPage{};
    page.unit_id = unit_id;
    page.log_filter = log_filter;
    page.since_id = since_id;
    page.next_report_id = world->status.next_report_id;

    df::unit* unit = df::unit::find(unit_id);
    if (!unit) {
        page.unit_found = false;
        return true;
    }
    page.unit_found = true;

    // Wanted lead ids from the requested log(s). unit_report_type has 3 valid values (0..2);
    // if the same id appears in two logs the first (lowest-index) log wins.
    std::unordered_map<int32_t, int> lead_log;
    const int n_logs = 3;
    for (int lt = 0; lt < n_logs; ++lt) {
        if (log_filter >= 0 && lt != log_filter)
            continue;
        const auto& ids = unit->reports.log[lt];
        for (size_t k = 0; k < ids.size(); ++k) {
            int32_t rid = ids[k];
            if (rid <= since_id)
                continue;
            lead_log.emplace(rid, lt);
        }
    }
    if (lead_log.empty())
        return true;

    const auto& reports = world->status.reports;
    bool in_run = false;
    int cur_log = -1;
    int lead_count = 0;
    for (size_t i = 0; i < reports.size(); ++i) {
        auto report = reports[i];
        if (!report)
            continue; // a culled hole -- do not close an open run (a tail may still follow)
        if (!report->flags.bits.continuation) {
            auto it = lead_log.find(report->id);
            in_run = (it != lead_log.end());
            cur_log = in_run ? it->second : -1;
            if (in_run) {
                if (lead_count >= max_reports) {
                    page.truncated = true;
                    break;
                }
                ++lead_count;
            }
        } else if (!in_run) {
            continue; // orphan continuation line (its lead wasn't wanted)
        }
        if (!in_run)
            continue;
        UnitReportEntry e;
        e.report = copy_report_entry(world, report);
        e.log_type = cur_log;
        e.log_key = unit_log_key(cur_log);
        page.entries.push_back(std::move(e));
    }
    return true;
}

} // namespace

bool reports_on_render_thread(const ReportsQuery& query, ReportsPage& page, std::string* err) {
    std::lock_guard<std::recursive_mutex> lock(g_reports_mutex);
    auto request = std::make_shared<RenderThreadReportsRequest>();
    request->query = query;
    auto future = request->done.get_future();
    DFHack::runOnRenderThread([request]() {
        if (save_barrier_active()) {
            request->err = "Dwarf Fortress is saving or unloading";
            request->done.set_value(false);
            return;
        }
        try {
            request->done.set_value(
                build_reports_page(request->query, request->page, &request->err));
        } catch (const std::exception& ex) {
            request->err = ex.what();
            request->done.set_value(false);
        } catch (...) {
            request->err = "unknown reports error";
            request->done.set_value(false);
        }
    });
    bool ok = false;
    if (!render_future_get(future, ok)) {
        if (err) *err = "reports render-thread request timed out or was abandoned";
        return false;
    }
    if (!ok) {
        if (err) *err = request->err;
        return false;
    }
    page = std::move(request->page);
    return true;
}

std::string reports_json(const std::string& player, const ReportsPage& page) {
    std::ostringstream body;
    body << "{\"player\":" << json_string(player)
         << ",\"nextReportId\":" << page.next_report_id
         << ",\"nextBeforeId\":" << page.next_before_id
         << ",\"sinceId\":" << page.query.since_id
         << ",\"beforeId\":" << page.query.before_id
         << ",\"category\":" << page.query.category
         << ",\"sectionId\":" << page.query.section
         << ",\"section\":"
         << (page.query.section >= 0 && page.query.section < taxonomy::SECTION_COUNT
                 ? json_string(taxonomy::SECTION_INFO[page.query.section].key)
                 : std::string("\"all\""))
         << ",\"truncated\":" << (page.truncated ? "true" : "false")
         << ",\"budgetExhausted\":" << (page.budget_exhausted ? "true" : "false")
         << ",\"reachedOldest\":" << (page.reached_oldest ? "true" : "false")
         << ",\"scanned\":" << page.scanned
         << ",\"totalReports\":" << page.total_reports
         << ",\"hasCounts\":" << (page.has_counts ? "true" : "false")
         << ",\"counts\":";
    if (!page.has_counts) {
        body << "null";
    } else {
        body << "{";
        for (int i = 0; i < taxonomy::SECTION_COUNT; ++i) {
            if (i) body << ",";
            body << json_string(taxonomy::SECTION_INFO[i].key) << ":" << page.section_counts[i];
        }
        body << "}";
    }
    body << ",\"sections\":[";
    for (int i = 0; i < taxonomy::SECTION_COUNT; ++i) {
        if (i) body << ",";
        body << "{\"id\":" << i
             << ",\"key\":" << json_string(taxonomy::SECTION_INFO[i].key)
             << ",\"label\":" << json_string(taxonomy::SECTION_INFO[i].label) << "}";
    }
    body << "],\"reports\":[";
    for (size_t i = 0; i < page.reports.size(); ++i) {
        if (i) body << ",";
        append_report_entry_json(body, page.reports[i]);
    }
    body << "]}\n";
    return body.str();
}

struct RenderThreadUnitReportsRequest {
    int32_t unit_id = -1;
    int log_filter = -1;
    int32_t since_id = -1;
    int max_reports = 200;
    UnitReportsPage page;
    std::string err;
    std::promise<bool> done;
};

bool unit_reports_on_render_thread(int32_t unit_id, int log_filter, int32_t since_id,
                                   int max_reports, UnitReportsPage& page, std::string* err) {
    std::lock_guard<std::recursive_mutex> lock(g_reports_mutex);
    auto request = std::make_shared<RenderThreadUnitReportsRequest>();
    request->unit_id = unit_id;
    request->log_filter = log_filter;
    request->since_id = since_id;
    request->max_reports = max_reports;
    auto future = request->done.get_future();
    DFHack::runOnRenderThread([request]() {
        if (save_barrier_active()) {
            request->err = "Dwarf Fortress is saving or unloading";
            request->done.set_value(false);
            return;
        }
        try {
            request->done.set_value(build_unit_reports_page(
                request->unit_id, request->log_filter, request->since_id,
                request->max_reports, request->page, &request->err));
        } catch (const std::exception& ex) {
            request->err = ex.what();
            request->done.set_value(false);
        } catch (...) {
            request->err = "unknown unit-reports error";
            request->done.set_value(false);
        }
    });
    bool ok = false;
    if (!render_future_get(future, ok)) {
        if (err) *err = "unit-reports render-thread request timed out or was abandoned";
        return false;
    }
    if (!ok) {
        if (err) *err = request->err;
        return false;
    }
    page = std::move(request->page);
    return true;
}

std::string unit_reports_json(const std::string& player, const UnitReportsPage& page) {
    std::ostringstream body;
    body << "{\"player\":" << json_string(player)
         << ",\"unitId\":" << page.unit_id
         << ",\"unitFound\":" << (page.unit_found ? "true" : "false")
         << ",\"logFilter\":" << page.log_filter
         << ",\"sinceId\":" << page.since_id
         << ",\"nextReportId\":" << page.next_report_id
         << ",\"truncated\":" << (page.truncated ? "true" : "false")
         << ",\"entries\":[";
    for (size_t i = 0; i < page.entries.size(); ++i) {
        if (i) body << ",";
        body << "{\"logType\":" << page.entries[i].log_type
             << ",\"logKey\":" << json_string(page.entries[i].log_key)
             << ",\"report\":";
        append_report_entry_json(body, page.entries[i].report);
        body << "}";
    }
    body << "]}\n";
    return body.str();
}

int resolve_unit_log_param(const httplib::Request& req) {
    if (!req.has_param("log"))
        return -1;
    std::string raw = req.get_param_value("log");
    if (raw.empty() || raw == "all")
        return -1;

    bool numeric = true;
    for (char c : raw) {
        if (!std::isdigit(static_cast<unsigned char>(c))) { numeric = false; break; }
    }
    if (numeric) {
        try {
            int value = std::stoi(raw);
            if (df::enum_traits<df::unit_report_type>::is_valid(value))
                return value;
        } catch (...) {}
        return -1;
    }

    std::string wanted = lower(raw);
    for (int v = 0; v <= df::enum_traits<df::unit_report_type>::last_item_value; ++v) {
        if (v < 0)
            continue;
        std::string key = lower(DFHack::enum_item_key(static_cast<df::unit_report_type>(v)));
        if (key == wanted)
            return v;
    }
    return -1;
}

int resolve_category_param(const httplib::Request& req) {
    if (!req.has_param("category"))
        return -1;
    std::string raw = req.get_param_value("category");
    if (raw.empty() || raw == "all")
        return -1;

    // Numeric form: category=34.
    bool numeric = !raw.empty();
    for (char c : raw) {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-') { numeric = false; break; }
    }
    if (numeric) {
        try {
            int value = std::stoi(raw);
            if (df::enum_traits<df::announcement_alert_type>::is_valid(value))
                return value;
        } catch (...) {}
        return -1;
    }

    // Name form: category=combat (case-insensitive match against the enum key).
    std::string wanted = lower(raw);
    for (int v = 0; v <= df::enum_traits<df::announcement_alert_type>::last_item_value; ++v) {
        std::string key = lower(DFHack::enum_item_key(static_cast<df::announcement_alert_type>(v)));
        if (key == wanted)
            return v;
    }
    return -1;
}

int resolve_section_param(const httplib::Request& req) {
    if (!req.has_param("section"))
        return -1;
    const std::string raw = req.get_param_value("section");
    if (raw.empty() || raw == "all")
        return -1;

    bool numeric = true;
    for (char c : raw) {
        if (!std::isdigit(static_cast<unsigned char>(c))) { numeric = false; break; }
    }
    if (numeric) {
        try {
            const int value = std::stoi(raw);
            if (value >= 0 && value < taxonomy::SECTION_COUNT)
                return value;
        } catch (...) {}
        return -1;
    }
    const std::string wanted = lower(raw);
    return taxonomy::section_from_key(wanted.c_str(), wanted.size());
}

void register_reports_routes(httplib::Server& server) {
    // Full announcements and reports route:
    //   since=<id>      follow the tail
    //   before=<id>     backfill the page older than this id
    //   section=<key>   combat|sieges|artifacts|trade|nobles|deaths|misc|all
    //   category=<n|k>  announcement_alert_type filter
    //   counts=1        include per-section totals
    //   max=<n>         cap on MESSAGES (1..500); continuation tails are free
    server.Get("/reports", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        ReportsQuery query;
        query_int(req, "since", query.since_id);
        query_int(req, "before", query.before_id);
        query_int(req, "max", query.max_reports);
        query.max_reports = std::max(1, std::min(500, query.max_reports));
        query.category = resolve_category_param(req);
        query.section = resolve_section_param(req);
        int counts = 0;
        query_int(req, "counts", counts);
        query.want_counts = counts != 0;
        int budget = query.scan_budget;
        query_int(req, "scan", budget);
        query.scan_budget = std::max(100, std::min(200000, budget));

        ReportsPage page;
        std::string err;
        if (!reports_on_render_thread(query, page, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(reports_json(player, page), "application/json; charset=utf-8");
    });

    // COMBAT-LOG DEPTH: per-unit Combat/Sparring/Hunting log, continuation-joined, live-followable.
    server.Get("/combat-reports", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int unit_id = -1;
        if (!query_int(req, "unit", unit_id) || unit_id < 0) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing or invalid unit param\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        int since = -1;
        int max_reports = 200;
        query_int(req, "since", since);
        query_int(req, "max", max_reports);
        max_reports = std::max(1, std::min(500, max_reports));
        int log_filter = resolve_unit_log_param(req);

        UnitReportsPage page;
        std::string err;
        if (!unit_reports_on_render_thread(unit_id, log_filter, since, max_reports, page, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(unit_reports_json(player, page), "application/json; charset=utf-8");
    });
}

} // namespace dfcapture
