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

#include "httplib.h"

#include <sstream>
#include <string>
#include <vector>

namespace dfcapture {

bool query_int(const httplib::Request& req, const char* name, int& value);
bool is_safe_player_id(const std::string& player);
std::string query_player(const httplib::Request& req);

std::string json_escape(const std::string& raw);
std::string json_string(const std::string& raw);
void append_json_string_array(std::ostringstream& body, const std::vector<std::string>& values);

} // namespace dfcapture
