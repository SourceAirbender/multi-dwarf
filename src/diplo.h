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

// Browser-side detector for pending petitions and diplomacy meetings.
//
// diplo_push_tick() samples all of it at <=1 Hz under a ConditionalCoreSuspender (the
// vote/popup posture) and broadcasts on change only:
//
//   {"type":"diplo","seq":N,"petitionsPending":N,"meetingsQueued":N,"open":<bool>
//    [,"by":"<player>"],"meeting":null|{...}}
//
// The latest state is sent after reconnect so the attention plaques cannot remain stale.
//
// Export-agreement priorities are writable while the Requests screen is open. Meeting advance,
// land-holder selection, and final request commit remain native-only.
//
// The camera is never touched. No ESC injection, cur_step mutation, or mm->flags
// writes -- a corrupted dipscript state could break agreements for the whole world.

// Routes: GET /diplo (current cached state) and
// POST /diplo-request-priority?player=&cat=&index=&value=0..4.
void register_diplo_routes(httplib::Server& server);

// Called once per ws_push_loop iteration after popup_push_tick to sample, diff, broadcast,
// and synchronize late joiners. Kept as a compile-time switch so this read-only detector
// can be disabled quickly during native-state diagnostics.
inline constexpr bool kDiploTickEnabled = true;

void diplo_push_tick();

// True while the native diplomacy meeting dialog is open (atomic; safe from any thread).
// The pause arbiter consults this to refuse web unpause with a clear reason while the sim
// is wedged by the meeting, and /diag exposes it as "diploBlocked".
bool diplo_meeting_open();

} // namespace dfcapture
