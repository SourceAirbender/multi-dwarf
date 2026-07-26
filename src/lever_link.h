// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// Copyright (C) 2026 Jake Taplin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "httplib.h"

namespace dfcapture {

void register_lever_link_routes(httplib::Server& server);

} // namespace dfcapture
