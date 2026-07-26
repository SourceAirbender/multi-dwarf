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

#include <string>

// DF's live 16-color curses palette (df::global::gps->uccolor[16][3]), the curses index -> RGB
// table that reflects the player's data/init/colors.txt. The same bytes ship on the /version
// handshake for every native color index the client renders
// (report colors, emotion attrs, profession/skill colors, [C:] tokens) instead of the browser
// hardcoding a copy of a palette the player is free to edit.
namespace dfcapture {
namespace curses {

constexpr int kColors = 16;

struct Rgb { int r; int g; int b; };

// True on success. Returns false when gps is unavailable (headless / early boot) or the index is
// out of range -- callers must then NOT substitute an invented color (they leave bytes alone or
// ship an empty palette), matching the burrow-swatch contract.
bool rgb(int index, Rgb& out);

// The palette as a JSON array literal: "[[r,g,b],[r,g,b],...]" (16 entries) or "[]" when gps is
// unavailable. Ready to splice into a response body.
std::string palette_json();

} // namespace curses
} // namespace dfcapture
