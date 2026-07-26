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

#include "curses_palette.h"

#include <sstream>

#include "df/global_objects.h"
#include "df/graphic.h"

namespace dfcapture {
namespace curses {

bool rgb(int index, Rgb& out) {
    auto gps = df::global::gps;
    if (!gps || index < 0 || index >= kColors)
        return false;
    out.r = gps->uccolor[index][0];
    out.g = gps->uccolor[index][1];
    out.b = gps->uccolor[index][2];
    return true;
}

std::string palette_json() {
    std::ostringstream body;
    body << "[";
    for (int i = 0; i < kColors; ++i) {
        Rgb c{};
        if (!rgb(i, c))
            return "[]";   // gps unavailable -> ship nothing rather than an invented palette
        if (i)
            body << ",";
        body << "[" << c.r << "," << c.g << "," << c.b << "]";
    }
    body << "]";
    return body.str();
}

} // namespace curses
} // namespace dfcapture
