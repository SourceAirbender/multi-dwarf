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

#include <cstdint>
#include <string>
#include <vector>

namespace dfcapture {

struct InfoTab {
    std::string id;
    std::string label;
};

struct InfoRow {
    int32_t unit_id = -1;
    int32_t item_id = -1;
    int32_t portrait_texpos = -1;
    int32_t building_id = -1;
    int32_t location_id = -1;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    bool has_pos = false;
    std::string name;
    std::string subtitle;
    std::string category;
    std::string profession;
    std::string job;
    std::string status;
    std::string kind;
    std::string icon_key;
    std::string icon_sheet;
    int32_t icon_x = -1;
    int32_t icon_y = -1;
    int32_t icon_row = -1;
    std::vector<std::string> badges;
    bool muted = false;
};

struct StockItemRow {
    int32_t item_id = -1;
    int32_t count = 1;
    std::string name;
    std::string subtitle;
    std::string status;
    bool muted = false;
};

struct InfoPanel {
    std::string panel;
    std::string section;
    std::string detail;
    std::string title;
    std::vector<InfoTab> primary_tabs;
    std::vector<InfoTab> section_tabs;
    std::vector<InfoTab> detail_tabs;
    std::vector<std::string> messages;
    std::vector<std::string> side_items;
    std::vector<InfoRow> rows;
    std::vector<StockItemRow> stock_items;
    std::string footer;
};

InfoPanel build_info_panel(const std::string& panel,
                           const std::string& section,
                           const std::string& detail);

std::string info_panel_json(const InfoPanel& panel);

bool info_panel_on_render_thread(const std::string& panel_name,
                                 const std::string& section,
                                 const std::string& detail,
                                 InfoPanel& panel,
                                 std::string* err = nullptr);

} // namespace dfcapture
