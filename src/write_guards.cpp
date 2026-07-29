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

// See write_guards.h for the model. This file provides the cached reader, the host-only
// writer (dfhack_console), and its two routes (GET /write-guards, host-only /console-config).

#include "write_guards.h"

#include "diagnostics.h"
#include "httplib.h"
#include "json_util.h"

#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace dfcapture {
namespace guards {
namespace {

// The plugin working directory is the DF root, matching the Lua bindings.
constexpr const char* kHostwritesPath = "dfcapture-hostwrites.json";

std::mutex g_mu;
std::string g_text;                                   // last file text ("" = missing/unreadable)
std::chrono::steady_clock::time_point g_stamp{};      // default == "never read"
bool g_have = false;

// Re-read the file at most every 2 s. Guards must fail closed, so every failure path lands on
// an empty text (every flag scans false). Caller holds g_mu.
const std::string& cached_text_locked() {
    using clock = std::chrono::steady_clock;
    auto now = clock::now();
    if (g_have &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_stamp).count() < 2000)
        return g_text;
    std::string text;
    try {
        std::ifstream in(kHostwritesPath, std::ios::binary);
        if (in)
            text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    } catch (...) {
        text.clear();                                  // unreadable -> everything OFF
    }
    g_text = std::move(text);
    g_stamp = now;
    g_have = true;
    return g_text;
}

// Set `"<flag>": true|false` inside the (flat) hostwrites JSON text, preserving every other key.
// Missing/blank/brace-less text is replaced by a fresh flat object. Pure.
std::string set_flag_in_text(const std::string& text, const std::string& flag, bool on) {
    const std::string value = on ? "true" : "false";
    const std::string key = "\"" + flag + "\"";
    size_t k = text.find(key);
    if (k != std::string::npos) {
        size_t i = k + key.size();
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        if (i < text.size() && text[i] == ':') {
            ++i;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' ||
                                       text[i] == '\n')) ++i;
            size_t end = i;
            while (end < text.size() && text[end] != ',' && text[end] != '}' &&
                   text[end] != '\r' && text[end] != '\n') ++end;
            return text.substr(0, i) + value + text.substr(end);
        }
        // Malformed around the key: fall through and rewrite the file minimally below.
    }
    const size_t open = text.find('{');
    const size_t close = text.rfind('}');
    if (open != std::string::npos && close != std::string::npos && close > open) {
        // Insert before the closing brace; comma only if the object already has content.
        bool has_content = false;
        for (size_t i = open + 1; i < close; ++i) {
            const char c = text[i];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') { has_content = true; break; }
        }
        const std::string insert = std::string(has_content ? ",\n  " : "\n  ") + key + ": " + value;
        return text.substr(0, close) + insert + "\n" + text.substr(close);
    }
    return "{\n  " + key + ": " + value + "\n}\n";
}

// Write one explicitly allowlisted flag. Callers perform the tunnel-aware host-tab check first.
// Only dfhack_console has an HTTP route.
bool write_allowed_flag(const char* flag, bool on) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::string text;
    try {
        std::ifstream in(kHostwritesPath, std::ios::binary);
        if (in)
            text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    } catch (...) {
        text.clear();
    }
    const std::string next = set_flag_in_text(text, flag, on);
    try {
        std::ofstream out(kHostwritesPath, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << next;
        if (!out) return false;
    } catch (...) {
        return false;
    }
    g_text = next;                                     // keep the cache honest immediately
    g_stamp = std::chrono::steady_clock::now();
    g_have = true;
    return true;
}

} // namespace

// Tunnel-aware host-tab detection: loopback peer + no proxy-forwarding header + a loopback-ish
// Host header. cloudflared (and any reverse proxy) terminates on the host and dials 127.0.0.1,
// so a bare loopback test would wave every TUNNELED remote friend through as "the host" -- the
// Forwarding-header and Host checks close that. All three tests are server-derived and fail
// toward "not host". If a legitimate host setup ever trips this, the
// documented fallback is editing dfcapture-hostwrites.json directly, which stays authoritative.
namespace {

bool peer_ip_is_loopback(const std::string& addr) {
    if (addr == "::1") return true;
    if (addr.rfind("127.", 0) == 0) return true;               // 127.0.0.0/8
    if (addr.rfind("::ffff:127.", 0) == 0) return true;        // IPv4-mapped loopback
    return false;
}

bool host_header_is_local(const std::string& host) {
    // Accept "localhost", "127.x.y.z", or "[::1]", each with an optional :port suffix.
    std::string h = host;
    if (!h.empty() && h[0] == '[') {                            // bracketed IPv6 form
        size_t close = h.find(']');
        if (close == std::string::npos) return false;
        return h.compare(0, close + 1, "[::1]") == 0;
    }
    size_t colon = h.find(':');
    if (colon != std::string::npos) h = h.substr(0, colon);
    if (h == "localhost") return true;
    if (h.rfind("127.", 0) == 0) return true;
    return false;
}

} // namespace

bool request_is_host_tab(const httplib::Request& req) {
    const bool forwarded = req.has_header("X-Forwarded-For") || req.has_header("CF-Connecting-IP") ||
                           req.has_header("Forwarded") || req.has_header("X-Real-IP");
    return peer_ip_is_loopback(req.remote_addr) && !forwarded &&
           host_header_is_local(req.get_header_value("Host"));
}

bool hostwrite_enabled(const std::string& flag) {
    std::lock_guard<std::mutex> lk(g_mu);
    return scan_hostwrite_flag(cached_text_locked(), flag);
}

std::string guarded_refusal_json(const std::string& flag, const std::string& what,
                                 const std::string& why) {
    return "{\"ok\":false,\"unsupported\":true,\"guarded\":true,\"flag\":" + json_string(flag) +
           ",\"error\":" + json_string(what + " is disabled on this host. " + why +
           " (flag \"" + flag + "\" in dfcapture-hostwrites.json, next to the DF executable;" +
           " a missing file or key means OFF.)") + "}\n";
}

void register_write_guard_routes(httplib::Server& server) {
    // ---- GET /write-guards ----------------------------------------------------------------------
    // Read-only flag state for guard-aware clients:
    // locked control must never look live, and must light up on its own when the host flips a
    // flag. It is never settable through this route.
    server.Get("/write-guards", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        std::ostringstream body;
        body << "{\"ok\":true,\"guards\":{";
        const char* flags[] = { kConsoleFlag };
        bool first = true;
        for (const char* f : flags) {
            if (!first) body << ",";
            first = false;
            body << json_string(f) << ":" << (hostwrite_enabled(f) ? "true" : "false");
        }
        body << "}}\n";
        res.set_content(body.str(), "application/json; charset=utf-8");
    });

    // ---- GET|POST /console-config[?enabled=on|off] ------------------------------------------------
    // The host-panel toggle for the dfhack_console POLICY flag -- the only key any route may
    // write. Reading state is open to any authed player (it is the same bit /write-guards serves);
    // WRITING requires the host tab (tunnel-aware), so a remote friend can never enable the console
    // for themselves.
    auto console_config_handler = [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        const bool host = request_is_host_tab(req);
        if (req.has_param("enabled")) {
            if (!host) {
                diagnostics_log("write-guards: non-host tried to set dfhack_console from " +
                                req.remote_addr);
                res.status = 403;
                res.set_content("{\"ok\":false,\"err\":\"only the host tab may change this "
                                "setting\"}\n", "application/json; charset=utf-8");
                return;
            }
            const std::string v = req.get_param_value("enabled");
            const bool on = (v == "on" || v == "true" || v == "1");
            if (!write_allowed_flag(kConsoleFlag, on)) {
                res.status = 500;
                res.set_content("{\"ok\":false,\"err\":\"could not write "
                                "dfcapture-hostwrites.json\"}\n",
                                "application/json; charset=utf-8");
                return;
            }
            diagnostics_log(std::string("write-guards: host set dfhack_console=") +
                            (on ? "on" : "off"));
        }
        res.set_content(std::string("{\"ok\":true,\"enabled\":") +
                            (hostwrite_enabled(kConsoleFlag) ? "true" : "false") +
                            ",\"host\":" + (host ? "true" : "false") + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/console-config", console_config_handler);
    server.Post("/console-config", console_config_handler);
}

} // namespace guards
} // namespace dfcapture
