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

// Fortress-elevation vote: ephemeral plugin-memory state with native-offer detection.
//
// Detection:
// df::global::game->main_interface.diplomacy (df::diplomacy_interfacest) is the native
// diplomat/liaison conversation popup. When `open && selecting_land_holder_position` the game
// asks the overseer to pick the new land holder. land_holder_pos_id lists the offered position ids
// (resolved against land_holder_child_civ / land_holder_parent_civ positions.own), and
// entity_position.land_holder carries the tier (1=baron, 2=count, 3=duke) that names the vote
// topic. vote_push_tick() samples this at <=1 Hz under a ConditionalCoreSuspender (the
// pause_arbiter autosave-sample pattern: skips instantly while the core is save-blocked) and
// auto-opens a vote on the rising edge / auto-closes it when the native popup goes away.
//
// VOTE STATE: one active vote {topic, yes/no, per-player-name votes (one each, changeable while
// open), openedBy, openedAt}, plus the last closed result for the banner. Manual /vote-start
// covers decisions detection cannot see (e.g. the mountainhome letter). The vote only ADVISES:
// the native accept/decline still happens in DF by whoever is at the keyboard.

// Routes: GET /vote (state+detection), POST/GET /vote-start, /vote-cast, /vote-close.
void register_vote_routes(httplib::Server& server);

// Called once per ws_push_loop iteration (after pause_push_tick). Samples detection, applies
// auto open/close edges, broadcasts {"type":"vote",...} state changes, and syncs late joiners.
void vote_push_tick();

} // namespace dfcapture
