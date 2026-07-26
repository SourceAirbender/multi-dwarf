// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only
//
// Shared HTTP serialization for ApiResult failures. Domain operations stay independent of
// httplib; route handlers opt into this small adapter at the boundary.
#pragma once

#include "api_result.h"
#include "json_util.h"

namespace dfcapture {

template <typename T>
void send_api_error(const ApiResult<T>& result, httplib::Response& response) {
    response.status = result.error.status;
    response.set_header("Cache-Control", "no-store");
    response.set_content("{\"ok\":false,\"code\":" + json_string(result.error.code) +
        ",\"error\":" + json_string(result.error.message) + "}\n",
        "application/json; charset=utf-8");
}

} // namespace dfcapture
