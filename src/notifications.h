// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios
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

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace dfcapture {

struct NotificationReport {
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
    bool has_pos = false;
    Camera pos;
};

struct NotificationUnitRef {
    int32_t unit_id = -1;
    int category = -1;
    std::string category_key;
    std::string unit_name;
    std::string dismiss_key;
    bool has_pos = false;
    Camera pos;
    std::vector<NotificationReport> reports;
};

struct NotificationAlert {
    int type = 0;
    std::string type_key;
    std::string dismiss_key;
    std::vector<std::string> dismiss_keys;
    std::vector<int32_t> report_ids;
    int32_t latest_report_id = -1;
    bool has_target = false;
    Camera target;
    std::vector<NotificationReport> reports;
    std::vector<NotificationUnitRef> unit_refs;
};

struct NotificationState {
    int32_t next_report_id = 0;
    int32_t report_count = 0;
    std::vector<NotificationAlert> alerts;
    std::vector<NotificationReport> recent;
};

bool notifications_on_render_thread(const std::unordered_set<std::string>& dismissed,
                                    NotificationState& state,
                                    std::string* err = nullptr);
bool notifications_on_render_thread(NotificationState& state, std::string* err = nullptr);
std::string notifications_json(const std::string& player, const NotificationState& state);
void remember_dismissed_alert_keys(const std::string& player, const std::string& raw_keys);
std::unordered_set<std::string> dismissed_alert_keys_for_player(const std::string& player);

} // namespace dfcapture
