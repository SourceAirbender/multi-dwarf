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
//
// GET/POST /stone-use exposes df.global.plotinfo.economic_stone (one flag byte
// per inorganic material index; nonzero = the player has selected this normally-restricted
// "economic" stone for use in ordinary/non-economic jobs too), the dfhack gui/stone-use
// equivalent (16d-labor-stone-use.png: Economic stone / Other stone tabs).

#pragma once

#include "httplib.h"

namespace dfcapture {

void register_stone_use_routes(httplib::Server& server);

} // namespace dfcapture
