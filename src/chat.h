// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, version 3 of the License.
//
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

namespace httplib {
class Server;
}

namespace dfcapture {

// Browser-only multiplayer chat. The relay deliberately owns no DF pointers and never queues
// work on the core or render threads, so chat and coordinate pings remain isolated from the game.
void register_chat_routes(httplib::Server& server);

} // namespace dfcapture
