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
// GET/POST /standing-orders exposes df.global.plotinfo's standing_orders_*
// booleans (df::global::standing_orders_auto_butcher etc, one extern uint8_t* per toggle),
// grouped per DF's real Standing Orders screen categories (df::standing_orders_category_type:
// Workshops/Hauling/Refuse/Forbidding/Petitions/Chores/Other -- 16b-labor-standing-orders.png).

#pragma once

#include "httplib.h"

namespace dfcapture {

void register_standing_orders_routes(httplib::Server& server);

} // namespace dfcapture
