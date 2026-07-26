// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

namespace httplib {
class Server;
}

namespace dfcapture {

// Serves the installed Dwarf Fortress Ogg catalog and exposes one canonical
// fortress music clock that every authenticated browser follows.
void register_audio_stream_routes(httplib::Server& server);

} // namespace dfcapture
