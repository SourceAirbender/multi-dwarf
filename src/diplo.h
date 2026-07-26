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

// Browser-side detector for petitions and diplomacy, plus the
// diplomacy-meeting mirror.
//
// Native shows two left-rail attention plaques (vanilla_interface graphics_interface.txt:
// PETITIONS_LIGHT and DIPLOMACY_LIGHT, both TILE_GRAPHICS_RECTANGLE 3x3 on INTERFACE_BITS)
// when a petition awaits a decision or a diplomat meeting is pending/underway. The browser
// Both pending petitions and host diplomacy meetings are surfaced in the browser.
//
// DETECTION (df-structures citations in the .cpp banner):
//   * petitionsPending  -- df.global.plotinfo.petitions.size() (the unapproved-agreement
//     list; the exact vector /petition-accept and /petition-deny mutate in fort_admin.cpp).
//   * meetingsQueued    -- df.global.plotinfo.dipscript_popups.size() ("cause
//     viewscreen_meetingst to pop up" per df-structures: a diplomat has reached the noble
//     and the meeting dialog is available/queued on the host).
//   * open + meeting {} -- df.global.game.main_interface.diplomacy (diplomacy_interfacest):
//     the live meeting dialog. Sim-blocking per DFHack World::ReadPauseState()
//     (library/modules/World.cpp: `game->main_interface.diplomacy.open`).
//
// diplo_push_tick() samples all of it at <=1 Hz under a ConditionalCoreSuspender (the
// vote/popup posture) and broadcasts on change only:
//
//   {"type":"diplo","seq":N,"petitionsPending":N,"meetingsQueued":N,"open":<bool>
//    [,"by":"<player>"],"meeting":null|{...}}   -- full shape in diplo.cpp's banner.
//
// Sticky for late joiners: once seq > 0, players who have not seen the current state get it
// on join/reconnect (vote.cpp g_synced pattern), so a reconnecting tab never keeps a stale
// plaque or meeting screen.
//
// CHOICES: DFHack's tradeagreement.lua overlay writes export-agreement priorities through
// dipev.sell_requests.priority[cat][i] while the native Requests screen is open. They are writable
// via POST /diplo-request-priority. Meeting advance, land-holder selection, and Requests commit run
// through the dipscript VM and are reported as native-only.
//
// The camera is never touched. No ESC injection, cur_step mutation, or mm->flags
// writes -- a corrupted dipscript state could break agreements for the whole world.

// Routes: GET /diplo (current mirrored state; mutex-only cache read) and
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
