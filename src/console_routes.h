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

// Browser DFHack command console routes.
//
//   GET  /console/commands   the helpdb catalog (name + short help) + the live deny table, served
//                            ONCE per panel open; the client filters it offline (no per-keystroke
//                            core lock).
//   POST /console/run?cmd=   run one command, return {status, output}.
//
// Both routes are available to connected players. The host setting and the server-side command
// policy in console_policy.h constrain execution uniformly for every caller.

#pragma once

#include "httplib.h"

namespace dfcapture {

void register_console_routes(httplib::Server& server);

} // namespace dfcapture
