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

#pragma once

#include "camera.h"
#include "httplib.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dfcapture {

// One entry from world->status.reports (the full combat/civilian/production event
// log DF keeps -- distinct from world->status.announcements, which only holds what
// scrolled through the top-of-screen banner). The field set matches NotificationReport
// in notifications.h so the client can reuse the same render helpers (dfTextColor,
// reportText, alertIconStyle) across both /notifications and /reports payloads.
struct ReportEntry {
    int32_t id = -1;
    int type = -1;
    int alert_type = 0;
    std::string type_key;
    std::string text;
    int color = 7;
    bool bright = false;
    int32_t duration = 0;
    int32_t repeat_count = 0;
    bool continuation = false;
    bool announcement = false;
    int32_t year = 0;
    int32_t time = 0;
    int zoom_type = -1;
    bool has_pos = false;
    Camera pos;
    int zoom_type2 = -1;
    bool has_pos2 = false;
    Camera pos2;
    int32_t activity_id = -1;
    int32_t activity_event_id = -1;
    int32_t speaker_id = -1;
    // Raws-derived taxonomy from announce_taxonomy.gen.h. `section` is a
    // taxonomy::Section id, `flags` the announcements.txt behaviour flags DF ships for this type
    // (BOX / ALERT / UCR / ...). Both are O(1) array lookups keyed by `type`.
    int section = 0;            // taxonomy::SECTION_MISC
    int taxonomy_flags = 0;     // taxonomy::AnnounceFlag bitfield
};

// Query model for the full announcements and reports screen. Two cursors are used because
// the screen performs two distinct operations:
//
//   since_id  FOLLOW  -- "what is NEW since I last looked" (id > since_id). Walks the tail.
//   before_id BACKFILL-- "give me the page OLDER than what I have" (id < before_id). Walks back.
//
// Without before_id the client could only ever see the newest max_reports rows: the log was a
// recent slice pretending to be a history. `next_before_id` is the resume cursor -- it is the id
// of the OLDEST entry we EXAMINED, not the oldest we MATCHED, so a section filter that matched
// nothing in this window still advances and the client can keep paging instead of spinning.
struct ReportsQuery {
    int32_t since_id = -1;    // > this id. -1 = no lower bound.
    int32_t before_id = -1;   // < this id. -1 = no upper bound (start at the newest).
    int category = -1;        // -1 = no filter; else an announcement_alert_type value (0..36)
    int section = -1;         // -1 = no filter; else a taxonomy::Section id (0..6)
    int max_reports = 200;    // cap on matching LEAD entries (continuation tails are free)
    int scan_budget = 20000;  // hard cap on entries EXAMINED, so one request can never stall the
                              // render thread on a 100k-report fort (nothing slow under the
                              // core lock). Exhausting it sets budget_exhausted, not an error.
    bool want_counts = false; // one extra O(N) classification pass; the client asks for it once,
                              // on open, not on every 2s poll.
};

struct ReportsPage {
    int32_t next_report_id = 0;  // pass back as `since` to follow the tail, regardless of filter
    int32_t next_before_id = -1; // pass back as `before` to fetch the next OLDER page
    ReportsQuery query;
    bool truncated = false;         // hit max_reports -- there are more matches in this direction
    bool budget_exhausted = false;  // hit scan_budget -- resume from next_before_id
    bool reached_oldest = false;    // walked off the front of the vector; there is no older page
    int32_t scanned = 0;            // entries examined (perf/observability; surfaced on the wire)
    int32_t total_reports = 0;      // world.status.reports.size()
    bool has_counts = false;
    int32_t section_counts[8] = {0, 0, 0, 0, 0, 0, 0, 0}; // by taxonomy::Section id
    std::vector<ReportEntry> reports; // oldest -> newest
};

bool reports_on_render_thread(const ReportsQuery& query, ReportsPage& page,
                              std::string* err = nullptr);
std::string reports_json(const std::string& player, const ReportsPage& page);

// Resolves a `section` query param that may be a taxonomy section key ("combat", "sieges", ...)
// or its numeric id. Returns -1 (no filter) when absent, "all", or unrecognised.
int resolve_section_param(const httplib::Request& req);

// Resolves a `category` query param that may be either a numeric announcement_alert_type
// value or its enum key name (case-insensitive, e.g. "combat", "COMBAT"). Returns -1
// (no filter) when the request has no `category` param or it doesn't match anything.
int resolve_category_param(const httplib::Request& req);

// --- Per-unit combat log (COMBAT-LOG DEPTH) --------------------------------------------------
// A ReportEntry tagged with which of a unit's report logs it came from. `unit.reports.log` is a
// static-array indexed by unit_report_type {Combat=0, Sparring=1, Hunting=2}; the vector holds
// LEAD report ids only (continuation-line reports are the next consecutive ids, flagged
// continuation, and are attached by the collector so multi-line messages arrive whole).
struct UnitReportEntry {
    ReportEntry report;
    int log_type = -1;      // unit_report_type value (0 Combat / 1 Sparring / 2 Hunting)
    std::string log_key;    // "Combat" / "Sparring" / "Hunting"
};

struct UnitReportsPage {
    int32_t unit_id = -1;
    bool unit_found = false;
    int log_filter = -1;        // -1 = all logs; else a unit_report_type value
    int32_t since_id = -1;      // only leads with id > since_id are returned
    int32_t next_report_id = 0; // world.status.next_report_id, the follow cursor
    bool truncated = false;
    std::vector<UnitReportEntry> entries; // oldest -> newest by report id
};

// log query param: numeric unit_report_type OR name ("combat"/"sparring"/"hunting"); -1 = all.
int resolve_unit_log_param(const httplib::Request& req);
bool unit_reports_on_render_thread(int32_t unit_id, int log_filter, int32_t since_id,
                                   int max_reports, UnitReportsPage& page, std::string* err = nullptr);
std::string unit_reports_json(const std::string& player, const UnitReportsPage& page);

void register_reports_routes(httplib::Server& server);

} // namespace dfcapture
