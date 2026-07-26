// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "httplib.h"

namespace dfcapture {

void register_native_popup_routes(httplib::Server& server);
bool native_popup_blocked();

} // namespace dfcapture
