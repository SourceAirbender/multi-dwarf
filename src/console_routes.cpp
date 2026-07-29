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

// Browser DFHack command console.
//
// The command policy owns the security model and deny table; this file handles HTTP plumbing.
//
// The entire console is gated by a host setting,
// DEFAULT OFF -- flag `dfhack_console` in dfcapture-hostwrites.json ("just put it in the host
// settings to allow people to use dfhack commands or not and have it default off"). The gate is
// on the ROUTES, not the button: hiding the UI while leaving POST /console/run live would be
// security theatre, since any player who knows the URL can still POST. Both handlers refuse 403
// with a guarded reason when the flag is off or the file is absent (fail closed). The host flips
// it from the host panel (POST /console-config, host-tab-only) or by editing the file.
//
// When enabled, every command is checked against the shared containment policy before execution.

#include "console_routes.h"

#include "console_policy.h"
#include "diagnostics.h"
#include "json_util.h"
#include "lua_bridge.h"
#include "write_guards.h"

#include <sstream>
#include <string>

namespace dfcapture {

namespace {

// Longest command line we will even look at. A console line is a command + args, not a payload;
// anything past this is a client bug or an attack, and it is refused before the gate runs.
constexpr size_t kMaxCommandLen = 512;

// Serialize the LIVE deny table so the client can grey out blocked commands in the palette.
// DISPLAY ONLY -- the server re-checks every run against the same table, so a client that ignores
// this (or is patched to) gains nothing. Shipping the rules (rather than a hardcoded client copy)
// is what keeps the two ends from drifting: there is one table, and it is this one.
std::string deny_rules_json() {
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto& rule : console::deny_table()) {
        if (!first) out << ",";
        first = false;
        out << "{\"kind\":"
            << json_string(rule.kind == console::DenyRule::Prefix ? "prefix" : "exact")
            << ",\"token\":" << json_string(rule.token)
            << ",\"reason\":" << json_string(rule.reason) << "}";
    }
    out << "]";
    return out.str();
}

// Check the host setting live per request with a two-second file cache.
// effect without restart). One helper so both routes refuse identically, with one reason string.
bool console_enabled() {
    return guards::hostwrite_enabled(guards::kConsoleFlag);
}

void refuse_console_off(httplib::Response& res) {
    res.status = 403;
    res.set_content(guards::guarded_refusal_json(
                        guards::kConsoleFlag, "The DFHack command console",
                        "The host has not enabled remote DFHack commands; commands from the "
                        "console can affect the host's machine, so it ships off."),
                    "application/json; charset=utf-8");
}

} // namespace

void register_console_routes(httplib::Server& server) {
    // ---- GET /console/commands -----------------------------------------------------------------
    // The autocomplete corpus: helpdb.get_commands() + get_entry_short_help() per command, plus the
    // deny table. Read-only and static for a play session, so the client fetches it once at panel
    // open and does search-as-you-type entirely offline -- no per-keystroke round trip and no
    // per-keystroke CoreSuspender.
    // All connected players receive the same catalog; command execution remains constrained by
    // the host setting and server-side command policy.
    server.Get("/console/commands", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        // Gate before listing commands. When the
        // host setting is off, the catalog refuses like the run route -- no live-looking surface.
        if (!console_enabled()) { refuse_console_off(res); return; }
        std::string err;
        std::string catalog = console_catalog_json_via_lua(&err);
        if (catalog.empty()) {
            res.status = 500;
            res.set_content("{\"ok\":false,\"err\":" + json_string(err.empty() ? "catalog unavailable" : err) +
                            "}\n", "application/json; charset=utf-8");
            return;
        }
        // The lua fn returns {"ok":true,"commands":[...]}\n -- splice the deny table in beside it
        // rather than re-parsing (same "lua owns the JSON" convention as every other bridge route).
        const size_t close = catalog.find_last_of('}');
        if (close == std::string::npos) {
            res.status = 500;
            res.set_content("{\"ok\":false,\"err\":\"malformed catalog\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        std::string body = catalog.substr(0, close) + ",\"denyRules\":" + deny_rules_json() + "}\n";
        res.set_content(body, "application/json; charset=utf-8");
    });

    // ---- POST /console/run?cmd=<command line> --------------------------------------------------
    // Runs ONE DFHack command and returns {ok, status, output}.
    //
    // Security checks run in this order:
    //   1. host setting -- dfhack_console must be explicitly enabled.
    //   2. length       -- reject a missing or oversized command.
    //   3. blocklist    -- reject unsafe commands for every caller, including the host.
    //   4. execute      -- the Lua bridge rechecks the same policy before execution.
    //
    // A denied command is logged with the reason so the host can audit attempted commands.
    auto run_handler = [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");

        // Check the host setting before inspecting the requested command.
        if (!console_enabled()) {
            diagnostics_log("console: REFUSED (dfhack_console is off) from " + req.remote_addr);
            refuse_console_off(res);
            return;
        }

        const std::string cmd = req.has_param("cmd") ? req.get_param_value("cmd") : std::string();
        if (cmd.empty()) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"err\":\"missing cmd\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        if (cmd.size() > kMaxCommandLen) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"err\":\"command too long\"}\n",
                            "application/json; charset=utf-8");
            return;
        }

        // *** THE BLOCKLIST. The sole containment, applied before anything runs. ***
        console::Denial gate = console::command_denied(cmd);
        if (gate.denied) {
            diagnostics_log("console: DENIED '" + cmd + "': " + gate.reason);
            res.status = 403;
            res.set_content("{\"ok\":false,\"blocked\":true,\"err\":" + json_string(gate.reason) +
                            "}\n", "application/json; charset=utf-8");
            return;
        }

        int status = -1;
        std::string text;
        std::string err;
        if (!console_run_via_lua(cmd, status, text, &err)) {
            res.status = 500;
            res.set_content("{\"ok\":false,\"err\":" + json_string(err.empty() ? "command failed" : err) +
                            "}\n", "application/json; charset=utf-8");
            return;
        }

        std::ostringstream body;
        body << "{\"ok\":true,\"status\":" << status
             << ",\"output\":" << json_string(text) << "}\n";
        res.set_content(body.str(), "application/json; charset=utf-8");
    };
    server.Post("/console/run", run_handler);
}

} // namespace dfcapture
