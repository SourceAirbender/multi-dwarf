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
// Authentication, not loopback identity: both routes are
// available to ANY player holding a valid join-auth cookie. Neither handler calls
// peer_ip_is_loopback -- there is deliberately NO host gate. Anonymous/unauthed callers are already
// refused upstream by the auth pre-routing handler (http_server.cpp), because neither path carries a
// static-asset extension and neither is in join_public_path -- so with a join passphrase set, an
// unauthed request never reaches these handlers at all.
//
// CONTAINMENT is therefore entirely the server-side blocklist in src/console_policy.h, which takes
// no caller identity and so binds the host exactly as it binds a friend. See that header.

#pragma once

#include "httplib.h"

namespace dfcapture {

void register_console_routes(httplib::Server& server);

} // namespace dfcapture
