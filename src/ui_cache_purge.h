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

// Purge raw building pointers retained by game.main_interface before deconstruction.
// Buildings::deconstruct clears world->selected_building and ui_look_list, but not the v50
// sub-interface caches.
//
// Known failure cases include deleting a zone while native DF retains it in
// game.main_interface.civzone, and deleting or replacing a stockpile while
// game.main_interface.custom_stockpile retains its settings pointer.
//
// Call this from EVERY path that deconstructs/frees a building of ANY type, under the same
// CoreSuspender that guards the free, BEFORE Buildings::deconstruct. It is CONSERVATIVE: it only
// nulls/closes a cache that points AT the dying building; unrelated buildings and id-based caches
// (view_sheets viewing_*, stockpile_link/stockpile_tools bld_id, create_work_order forced_bld_id,
// squads_interfacest ids) are left untouched.

#pragma once

namespace df { struct building; }

namespace dfcapture {

// Null/close every raw df::building* cache in game.main_interface that references `b`. Safe on a
// null `b` or a null df::global::game. MUST run under CoreSuspender, on the core thread, BEFORE
// the building is freed.
void purge_ui_caches_for_building(df::building* b);

} // namespace dfcapture
