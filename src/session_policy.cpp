// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#include "session_policy.h"

#include "build_identity.h"
#include "Core.h"
#include "diagnostics.h"
#include "httplib.h"
#include "interaction.h"
#include "json_util.h"
#include "save_barrier.h"
#include "sdl_capture.h"
#include "write_guards.h"

#include "DataDefs.h"
#include "df/global_objects.h"
#include "df/world.h"
#include "modules/World.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>

using namespace DFHack;

namespace dfcapture {
namespace {

constexpr const char* kPolicyFile = "dfhack-config/dfcapture-session.json";
constexpr long long kPresenceStaleMs = 7000;

struct Policy {
    bool remote_save = true;
    bool remote_audio = true;
    bool disconnect_pause = false;
    bool host_unpause_only = false;
    int disconnect_grace_ms = 5000;
};

struct SessionPresence {
    std::string name;
    long long last_ms = 0;
};

std::mutex g_mutex;
Policy g_policy;
std::unordered_map<std::string, SessionPresence> g_presence;
std::atomic<bool> g_stop{true};
std::thread g_watchdog;
bool g_had_players = false;
long long g_empty_since_ms = 0;
bool g_disconnect_pause_applied = false;

// Pause arbitration and busy-state metadata. Guarded by g_mutex.
struct PauseMeta {
    bool last_target_running = false;   // last target we applied (false = paused)
    std::string actor;                  // player id that last drove the pause, or "" for host/native
    std::string reason = "native";      // "player" | "host" | "disconnect" | "native"
    long long changed_ms = 0;           // when the pause state last changed on our request
    long long last_apply_ms = 0;        // timestamp of the last applied request (merge window)
};
PauseMeta g_pause;
constexpr long long kPauseMergeMs = 250;   // opposing requests within this window: the host wins
constexpr long long kPauseKeyTtlMs = 10000;
constexpr size_t kMaxPauseKeys = 128;

struct RecentPauseKey {
    long long seen_ms = 0;
    bool superseded = false;
};
std::unordered_map<std::string, RecentPauseKey> g_pause_keys;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string trim(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && static_cast<unsigned char>(value[begin]) <= ' ') ++begin;
    while (end > begin && static_cast<unsigned char>(value[end - 1]) <= ' ') --end;
    return value.substr(begin, end - begin);
}

bool scan_bool(const std::string& text, const std::string& key, bool fallback) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return fallback;
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) return fallback;
    ++pos;
    while (pos < text.size() && static_cast<unsigned char>(text[pos]) <= ' ') ++pos;
    if (text.compare(pos, 4, "true") == 0) return true;
    if (text.compare(pos, 5, "false") == 0) return false;
    return fallback;
}

int scan_int(const std::string& text, const std::string& key, int fallback) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return fallback;
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) return fallback;
    ++pos;
    while (pos < text.size() && static_cast<unsigned char>(text[pos]) <= ' ') ++pos;
    try {
        return std::stoi(text.substr(pos));
    } catch (...) {
        return fallback;
    }
}

void load_policy() {
    Policy policy;
    std::ifstream policy_file(kPolicyFile);
    std::ostringstream text;
    if (policy_file) text << policy_file.rdbuf();
    const std::string body = text.str();
    policy.remote_save = scan_bool(body, "remoteSave", true);
    policy.remote_audio = scan_bool(body, "remoteAudio", true);
    policy.disconnect_pause = scan_bool(body, "disconnectPause", false);
    policy.host_unpause_only = scan_bool(body, "hostUnpauseOnly", false);
    policy.disconnect_grace_ms =
        std::clamp(scan_int(body, "disconnectGraceMs", 5000), 1000, 60000);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_policy = policy;
}

bool persist_policy(const Policy& policy, std::string* err) {
    std::ofstream file(kPolicyFile, std::ios::trunc);
    if (!file) {
        if (err) *err = std::string("cannot write ") + kPolicyFile;
        return false;
    }
    file << "{\n"
         << "  \"remoteSave\": " << (policy.remote_save ? "true" : "false") << ",\n"
         << "  \"remoteAudio\": " << (policy.remote_audio ? "true" : "false") << ",\n"
         << "  \"disconnectPause\": " << (policy.disconnect_pause ? "true" : "false") << ",\n"
         << "  \"hostUnpauseOnly\": " << (policy.host_unpause_only ? "true" : "false") << ",\n"
         << "  \"disconnectGraceMs\": " << policy.disconnect_grace_ms << "\n"
         << "}\n";
    file.flush();
    if (!file.good()) {
        if (err) *err = std::string("write failed: ") + kPolicyFile;
        return false;
    }
    return true;
}

std::string cookie_value(const std::string& header, const std::string& name) {
    size_t pos = 0;
    while (pos < header.size()) {
        while (pos < header.size() && (header[pos] == ' ' || header[pos] == ';')) ++pos;
        size_t end = header.find(';', pos);
        if (end == std::string::npos) end = header.size();
        const std::string pair = header.substr(pos, end - pos);
        const size_t equals = pair.find('=');
        if (equals != std::string::npos && trim(pair.substr(0, equals)) == name)
            return pair.substr(equals + 1);
        pos = end + 1;
    }
    return {};
}

void prune_presence_locked(long long now) {
    for (auto it = g_presence.begin(); it != g_presence.end();) {
        if (now - it->second.last_ms > kPresenceStaleMs) it = g_presence.erase(it);
        else ++it;
    }
}

bool valid_stable_player_id(const std::string& value) {
    if (value.size() != 32) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch) || (ch >= 'a' && ch <= 'f');
    });
}

std::string random_player_id() {
    std::random_device source;
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i)
        out << std::setw(8) << static_cast<uint32_t>(source());
    return out.str();
}

void prune_pause_keys_locked(long long now) {
    for (auto it = g_pause_keys.begin(); it != g_pause_keys.end();) {
        if (now - it->second.seen_ms > kPauseKeyTtlMs) it = g_pause_keys.erase(it);
        else ++it;
    }
    while (g_pause_keys.size() > kMaxPauseKeys) {
        auto oldest = g_pause_keys.begin();
        for (auto it = g_pause_keys.begin(); it != g_pause_keys.end(); ++it) {
            if (it->second.seen_ms < oldest->second.seen_ms) oldest = it;
        }
        g_pause_keys.erase(oldest);
    }
}

std::string pause_key(const std::string& player, const std::string& request_key) {
    if (request_key.empty()) return {};
    return player.substr(0, 64) + "\n" + request_key.substr(0, 128);
}

bool pause_state_snapshot(bool& paused) {
    std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
    CoreSuspender suspend;
    if (!df::global::pause_state) return false;
    paused = *df::global::pause_state;
    return true;
}

bool apply_pause_transaction(const std::string& player, bool is_host,
                             const std::string& action, const std::string& request_key,
                             const std::string& reason, bool enforce_remote_policy,
                             PauseActionResult& result) {
    result = {};
    const bool explicit_pause = action == "pause";
    const bool explicit_play =
        action == "play" || action == "resume" || action == "unpause";
    if (!explicit_pause && !explicit_play && action != "toggle-pause") {
        result.error = "unsupported pause action";
        return false;
    }

    // The capture lock serializes this state transition with every other suspended world operation.
    // Resolve toggle and verify the final flag while the core is suspended: no HTTP-thread
    // pause_state read can race the mutation.
    std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
    CoreSuspender suspend;
    if (save_barrier_active()) {
        result.error = "save in progress";
        return false;
    }
    if (!df::global::pause_state) {
        result.error = "pause state unavailable";
        return false;
    }

    const bool was_paused = *df::global::pause_state;
    const bool target_running =
        explicit_play || (action == "toggle-pause" && was_paused);
    const bool target_paused = !target_running;
    const long long now = now_ms();
    const std::string key = pause_key(player, request_key);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        prune_pause_keys_locked(now);

        // Reconcile metadata if DF/native UI changed the pause flag outside this endpoint.
        if (g_pause.changed_ms == 0 || g_pause.last_target_running != !was_paused) {
            g_pause.last_target_running = !was_paused;
            g_pause.actor.clear();
            g_pause.reason = "native";
            g_pause.changed_ms = now;
            g_pause.last_apply_ms = 0;
        }

        if (!key.empty()) {
            auto found = g_pause_keys.find(key);
            if (found != g_pause_keys.end()) {
                result.ok = true;
                result.applied = false;
                result.paused = was_paused;
                result.duplicate = true;
                result.superseded = found->second.superseded;
                return true;
            }
        }

        if (enforce_remote_policy && target_running &&
                g_policy.host_unpause_only && !is_host) {
            result.error = "only the host may unpause under the current session policy";
            result.forbidden = true;
            return false;
        }

        // An explicit host target wins over an opposing remote request arriving in the merge window.
        if (!is_host && g_pause.reason == "host" &&
                now - g_pause.last_apply_ms < kPauseMergeMs &&
                g_pause.last_target_running != target_running) {
            if (!key.empty())
                g_pause_keys[key] = {now, true};
            result.ok = true;
            result.applied = false;
            result.paused = was_paused;
            result.superseded = true;
            return true;
        }
    }

    // We already own the core suspension. Calling native_popup_blocked() here would attempt a
    // nested ConditionalCoreSuspender and could report an empty snapshot, so inspect the same
    // authoritative queue directly.
    if (target_running && df::global::world &&
            !df::global::world->status.popups.empty()) {
        result.error = "dismiss the active fortress announcement before unpausing";
        result.forbidden = true;
        return false;
    }

    // Explicit pause/play requests that already match current state are successful no-ops. They do
    // not steal attribution from the actor who performed the real transition.
    if (was_paused == target_paused) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!key.empty()) g_pause_keys[key] = {now, false};
        result.ok = true;
        result.applied = false;
        result.paused = was_paused;
        return true;
    }

    World::SetPauseState(target_paused);
    if (!df::global::pause_state || *df::global::pause_state != target_paused) {
        result.error = "pause state change was not applied";
        return false;
    }

    {
        // Commit metadata only after DF confirms the mutation.
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pause.last_target_running = target_running;
        g_pause.actor = player.substr(0, 64);
        g_pause.reason = reason;
        g_pause.changed_ms = now;
        g_pause.last_apply_ms = now;
        if (!key.empty()) g_pause_keys[key] = {now, false};
    }
    result.ok = true;
    result.applied = true;
    result.paused = target_paused;
    return true;
}

std::string session_json(bool host) {
    const long long now = now_ms();
    const bool saving = save_barrier_active();
    bool paused = false;
    // This route stays available during the save/world barrier so a newly loaded browser can
    // show the blocking notice. Avoid DF pause globals while their object graph is unsafe.
    const bool pause_available = !saving && pause_state_snapshot(paused);
    std::lock_guard<std::mutex> lock(g_mutex);
    prune_presence_locked(now);
    if (pause_available && (g_pause.changed_ms == 0 ||
            g_pause.last_target_running != !paused)) {
        g_pause.last_target_running = !paused;
        g_pause.actor.clear();
        g_pause.reason = "native";
        g_pause.changed_ms = now;
        g_pause.last_apply_ms = 0;
    }
    std::ostringstream out;
    out << "{\"ok\":true,\"host\":" << (host ? "true" : "false")
        << ",\"remoteSave\":" << (g_policy.remote_save ? "true" : "false")
        << ",\"remoteAudio\":" << (g_policy.remote_audio ? "true" : "false")
        << ",\"disconnectPause\":" << (g_policy.disconnect_pause ? "true" : "false")
        << ",\"hostUnpauseOnly\":" << (g_policy.host_unpause_only ? "true" : "false")
        << ",\"disconnectGraceMs\":" << g_policy.disconnect_grace_ms;
    // Report who paused, why, when, whether saving is active, and whether this client may unpause.
    {
        out << ",\"paused\":" << (pause_available && paused ? "true" : "false")
            << ",\"pauseAvailable\":" << (pause_available ? "true" : "false")
            << ",\"pauseReason\":" << json_string(g_pause.reason)
            << ",\"pauseActor\":" << json_string(g_pause.actor)
            << ",\"pauseChangedAt\":" << g_pause.changed_ms
            << ",\"saving\":" << (saving ? "true" : "false")
            << ",\"busyReason\":" << json_string(saving ? "saving" : "")
            << ",\"canUnpause\":" << ((!g_policy.host_unpause_only || host) ? "true" : "false");
    }
    out << ",\"players\":[";
    bool first = true;
    for (const auto& entry : g_presence) {
        if (!first) out << ",";
        first = false;
        out << "{\"player\":" << json_string(entry.first)
            << ",\"name\":" << json_string(entry.second.name)
            << ",\"ageMs\":" << std::max<long long>(0, now - entry.second.last_ms) << "}";
    }
    out << "]}\n";
    return out.str();
}

void watchdog_loop() {
    while (!g_stop.load()) {
        bool should_pause = false;
        {
            const long long now = now_ms();
            std::lock_guard<std::mutex> lock(g_mutex);
            prune_presence_locked(now);
            if (!g_presence.empty()) {
                g_had_players = true;
                g_empty_since_ms = 0;
                g_disconnect_pause_applied = false;
            } else if (g_had_players && g_policy.disconnect_pause &&
                       !g_disconnect_pause_applied) {
                if (!g_empty_since_ms) g_empty_since_ms = now;
                if (now - g_empty_since_ms >= g_policy.disconnect_grace_ms) {
                    g_disconnect_pause_applied = true;
                    should_pause = true;
                }
            }
        }
        if (should_pause && !save_barrier_active()) {
            PauseActionResult result;
            if (apply_pause_transaction("", true, "pause", "", "disconnect", false, result)) {
                diagnostics_log("session-policy: paused after the last browser player disconnected");
            } else {
                diagnostics_log("session-policy: disconnect pause skipped: " + result.error);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

bool bool_param(const httplib::Request& req, const char* name, bool fallback) {
    if (!req.has_param(name)) return fallback;
    const std::string value = req.get_param_value(name);
    return value == "1" || value == "true" || value == "on";
}

} // namespace

void session_policy_start() {
    session_policy_stop();
    load_policy();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_presence.clear();
        g_had_players = false;
        g_empty_since_ms = 0;
        g_disconnect_pause_applied = false;
        g_pause = {};
        g_pause.reason = "native";
        g_pause_keys.clear();
    }
    g_stop = false;
    g_watchdog = std::thread(watchdog_loop);
}

void session_policy_stop() {
    g_stop = true;
    if (g_watchdog.joinable()) g_watchdog.join();
}

bool session_request_is_host(const httplib::Request& req) {
    return guards::request_is_host_tab(req);
}

std::string session_request_player_id(const httplib::Request& req) {
    const std::string cookie =
        cookie_value(req.get_header_value("Cookie"), "dfcap_player");
    if (valid_stable_player_id(cookie)) return cookie;
    if (req.has_param("player")) {
        const std::string fallback = req.get_param_value("player");
        if (is_safe_player_id(fallback)) return fallback;
    }
    return "default";
}

bool session_remote_audio_enabled() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_policy.remote_audio;
}

std::string session_display_name(const std::string& player) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto found = g_presence.find(player);
    return found == g_presence.end() || found->second.name.empty()
        ? player : found->second.name;
}

std::vector<SessionPlayer> session_players_snapshot() {
    const long long now = now_ms();
    std::lock_guard<std::mutex> lock(g_mutex);
    prune_presence_locked(now);
    std::vector<SessionPlayer> players;
    players.reserve(g_presence.size());
    for (const auto& entry : g_presence)
        players.push_back({entry.first, entry.second.name,
                           std::max<long long>(0, now - entry.second.last_ms)});
    return players;
}

void session_presence_heartbeat(const std::string& player, const std::string& name) {
    if (player.empty()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    SessionPresence& entry = g_presence[player.substr(0, 64)];
    entry.name = name.substr(0, 32);
    entry.last_ms = now_ms();
}

bool session_apply_pause_request(const std::string& player, bool is_host,
                                 const std::string& action, const std::string& request_key,
                                 PauseActionResult& result) {
    return apply_pause_transaction(player, is_host, action, request_key,
                                   is_host ? "host" : "player", true, result);
}

void register_session_policy_routes(httplib::Server& server) {
    server.Get("/version", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(build_identity_json(),
                        "application/json; charset=utf-8");
    });

    auto identity = [](const httplib::Request& req, httplib::Response& res) {
        std::string id =
            cookie_value(req.get_header_value("Cookie"), "dfcap_player");
        if (!valid_stable_player_id(id) && req.has_param("candidate")) {
            const std::string candidate = req.get_param_value("candidate");
            if (valid_stable_player_id(candidate)) id = candidate;
        }
        if (!valid_stable_player_id(id)) id = random_player_id();
        const std::string name = req.has_param("name")
            ? trim(req.get_param_value("name")).substr(0, 32) : std::string();
        res.set_header("Cache-Control", "no-store");
        res.set_header("Set-Cookie", "dfcap_player=" + id +
            "; Path=/; Max-Age=31536000; SameSite=Strict; HttpOnly");
        res.set_content("{\"ok\":true,\"playerId\":" + json_string(id) +
            ",\"displayName\":" + json_string(name.empty() ? id : name) + "}\n",
            "application/json; charset=utf-8");
    };
    server.Get("/identity", identity);
    server.Post("/identity", identity);

    server.Get("/session", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(session_json(session_request_is_host(req)),
                        "application/json; charset=utf-8");
    });
    server.Get("/session-config", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(session_json(session_request_is_host(req)),
                        "application/json; charset=utf-8");
    });

    server.Post("/session-config", [](const httplib::Request& req, httplib::Response& res) {
        if (!session_request_is_host(req)) {
            res.status = 403;
            res.set_content("{\"ok\":false,\"error\":\"host only\"}\n",
                            "application/json; charset=utf-8");
            return;
        }

        Policy next;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            next = g_policy;
        }
        next.remote_save = bool_param(req, "remoteSave", next.remote_save);
        next.remote_audio = bool_param(req, "remoteAudio", next.remote_audio);
        next.disconnect_pause = bool_param(req, "disconnectPause", next.disconnect_pause);
        next.host_unpause_only = bool_param(req, "hostUnpauseOnly", next.host_unpause_only);
        if (req.has_param("disconnectGraceMs")) {
            try {
                next.disconnect_grace_ms =
                    std::clamp(std::stoi(req.get_param_value("disconnectGraceMs")), 1000, 60000);
            } catch (...) {
                res.status = 400;
                res.set_content("{\"ok\":false,\"error\":\"invalid disconnect grace\"}\n",
                                "application/json; charset=utf-8");
                return;
            }
        }

        std::string err;
        if (!persist_policy(next, &err)) {
            res.status = 500;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_policy = next;
            if (!g_policy.disconnect_pause) {
                g_empty_since_ms = 0;
                g_disconnect_pause_applied = false;
            }
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(session_json(true), "application/json; charset=utf-8");
    });

    server.Post("/save", [](const httplib::Request& req, httplib::Response& res) {
        bool remote_save = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            remote_save = g_policy.remote_save;
        }
        if (!session_request_is_host(req) && !remote_save) {
            res.status = 403;
            res.set_content("{\"ok\":false,\"error\":\"remote saves are disabled\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        std::string err;
        if (!save_world_on_core_thread(&err)) {
            res.status = 409;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"queued\":true}\n",
                        "application/json; charset=utf-8");
    });
}

} // namespace dfcapture
