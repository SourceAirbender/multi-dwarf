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

#include "json_util.h"

#include "MiscUtils.h"

#include <cctype>
#include <cstdlib>
#include <iomanip>

namespace dfcapture {

bool query_int(const httplib::Request& req, const char* name, int& value) {
    if (!req.has_param(name))
        return false;
    value = std::atoi(req.get_param_value(name).c_str());
    return true;
}

bool is_safe_player_id(const std::string& player) {
    if (player.empty() || player.size() > 96)
        return false;
    for (unsigned char ch : player) {
        if (!std::isalnum(ch) && ch != '-' && ch != '_')
            return false;
    }
    return true;
}

std::string query_player(const httplib::Request& req) {
    std::string player = req.has_param("player") ? req.get_param_value("player") : "default";
    return is_safe_player_id(player) ? player : "default";
}

std::string json_escape(const std::string& raw) {
    std::ostringstream out;
    for (unsigned char ch : DF2UTF(raw)) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20)
                out << "\\u" << std::hex << std::uppercase << std::setw(4)
                    << std::setfill('0') << static_cast<int>(ch)
                    << std::dec << std::setfill(' ');
            else
                out << static_cast<char>(ch);
        }
    }
    return out.str();
}

std::string json_string(const std::string& raw) {
    return "\"" + json_escape(raw) + "\"";
}

void append_json_string_array(std::ostringstream& body, const std::vector<std::string>& values) {
    body << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) body << ",";
        body << json_string(values[i]);
    }
    body << "]";
}

} // namespace dfcapture
