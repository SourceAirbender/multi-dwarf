// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
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

// Compatibility seam for modules that can use a WebSocket push layer. On this pixel/polled
// build there are no WS connections. Every module also serves its full state over plain GET
// routes, so the "push a frame to every
// connected player" calls become no-ops and clients simply poll the GET while their panel is
// open (the same model as /presence). Domain modules remain independent of the transport.

#pragma once

#include <string>
#include <vector>

namespace dfcapture {

// No WS connections on this build: nobody to enumerate, nothing to push.
inline std::vector<std::string> ws_connected_players() { return {}; }
inline size_t broadcast_to_player(const std::string& /*player*/, const std::string& /*msg*/) {
    return 0;
}

} // namespace dfcapture
