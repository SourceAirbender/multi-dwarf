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

// C++ binding for the dfcapture-hostwrites.json guard mechanism.
//
// dfcapture-hostwrites.json (next to the DF executable, which is the plugin's working directory)
// is the runtime write-guard mechanism. A missing file, missing key, or value other than the
// literal `true` means off. C++ and Lua routes use the same file and semantics.
//
// Supported flag:
//   dfhack_console        -- browser DFHack console host policy:
//                            "let friends run DFHack commands on my PC". Default OFF for a
//                            stranger's install. The host may flip it from the host panel
//                            (POST /console-config, host-tab-only) -- the ONLY HTTP-settable key.
//                            It is now the ONLY flag: /write-guards enumerates exactly one guard.
// Gameplay actions rely on their domain safety checks and are not host-policy gated.
// No route may set any key other than dfhack_console.

#pragma once

#include <string>

namespace httplib { class Server; struct Request; }

namespace dfcapture {
namespace guards {

// C++ flag name.
constexpr const char* kConsoleFlag = "dfhack_console";

// Pure flat scan of the hostwrites JSON text for `"<flag>": true`. FAIL CLOSED: the ONLY input
// that enables is a well-formed `"<flag>"` key whose value is literally `true` (with a
// non-identifier character or end-of-text after it). Absent key, malformed colon, `false`,
// `"true"` (string), `TRUE`, `1`, `truex` -- all scan as off. This is the exact inverse default
// is fail-closed. The helper is header-only and pure so it can be tested without DF.
inline bool scan_hostwrite_flag(const std::string& text, const std::string& flag) {
    const std::string key = "\"" + flag + "\"";
    size_t k = text.find(key);
    if (k == std::string::npos) return false;                 // key absent -> OFF
    size_t i = k + key.size();
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i >= text.size() || text[i] != ':') return false;     // malformed -> OFF
    ++i;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' ||
                               text[i] == '\n')) ++i;
    const size_t after = i + 4;
    if (after < text.size()) {
        const char c = text[after];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_')
            return false;                                     // `truex` etc. -> OFF
    }
    // The ONLY enabling return: a literal, boundary-clean `true`.
    return text.compare(i, 4, "true") == 0;
}

// Read one flag from dfcapture-hostwrites.json in the DF root. Missing or unreadable files return
// false. Whole-file text is cached with a short TTL so a host toggle takes effect within seconds
// without a plugin reload, and request bursts don't re-stat the file each time.
bool hostwrite_enabled(const std::string& flag);

// Lua and C++ guarded routes share a common refusal shape:
// {"ok":false,"unsupported":true,"guarded":true,"flag":...,"error":...}\n.
// routes use the same refusal shape. `what` is the plain-English name of the refused action;
// `why` is the one-sentence host-facing reason.
std::string guarded_refusal_json(const std::string& flag, const std::string& what,
                                 const std::string& why);

// True iff the request comes from the host's OWN browser tab -- the SAME tunnel-aware test
// /console-config uses to decide who may flip a flag (loopback peer + no proxy-forwarding header +
// a loopback-ish Host, so a cloudflared-tunneled remote friend is NOT waved through as the host).
// Exposed so a mutation route can grant the host the authority it plainly already has at the DF
// keyboard, while every remote guest stays bound by the fail-closed hostwrite flag. Server-side
// and peer-address-derived, it never trusts anything the client claimed.
bool request_is_host_tab(const httplib::Request& req);

// Register GET /write-guards and the host-only /console-config toggle (the sole settable flag).
// Call above the catch-all so auth covers all.
void register_write_guard_routes(httplib::Server& server);

} // namespace guards
} // namespace dfcapture
