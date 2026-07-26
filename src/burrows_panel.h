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

namespace dfcapture {

void register_burrows_routes(httplib::Server& server);

// Called once per ws_push_loop iteration after popup_push_tick. If any
// burrow write route has bumped the revision since the last pass, broadcast
// {"type":"burrows","seq":N} to every connected player, plus a sticky late-join sync. A POKE, not
// state -- each player's rects are camera-z-specific, so the client refetches its own /burrows.
// Rate-limited to <=1 Hz internally. Reads no DF memory and takes no CoreSuspender.
void burrow_push_tick();

} // namespace dfcapture
