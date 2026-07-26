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

#include "httplib.h"

#include <string>

namespace dfcapture {

// Detailed missions and raids API.
//
// Mission creation requires an atomic update across army controllers, armies, squads, historical
// entities, and historical figures. DFHack does not expose a commit primitive for that object
// graph, so POST /mission-create validates input and returns native-only without mutating state.
//
// GET /missions reports active and stranded squads. POST /mission-rescue runs DFHack's
// fix/stuck-squad repair when its prerequisites are satisfied.
void register_mission_routes(httplib::Server& server);

} // namespace dfcapture
