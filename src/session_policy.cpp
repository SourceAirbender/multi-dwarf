// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#include "session_policy.h"

#include "diagnostics.h"
#include "httplib.h"
#include "interaction.h"
#include "json_util.h"
#include "native_popup.h"
#include "save_barrier.h"
#include "write_guards.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace dfcapture {
namespace {

constexpr const char* kPasswordFile = "dfcapture_join_password.txt";
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
std::string g_password;
Policy g_policy;
std::unordered_map<std::string, SessionPresence> g_presence;
std::atomic<bool> g_stop{true};
std::thread g_watchdog;
bool g_had_players = false;
long long g_empty_since_ms = 0;
bool g_disconnect_pause_applied = false;

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

bool constant_time_equal(const std::string& left, const std::string& right) {
    unsigned char diff = static_cast<unsigned char>((left.size() ^ right.size()) & 0xff);
    diff |= static_cast<unsigned char>(((left.size() ^ right.size()) >> 8) != 0);
    const size_t count = std::max(left.size(), right.size());
    for (size_t i = 0; i < count; ++i) {
        const unsigned char a = i < left.size() ? static_cast<unsigned char>(left[i]) : 0;
        const unsigned char b = i < right.size() ? static_cast<unsigned char>(right[i]) : 0;
        diff |= static_cast<unsigned char>(a ^ b);
    }
    return diff == 0;
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
    std::ifstream password_file(kPasswordFile);
    std::string password;
    std::getline(password_file, password);

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
    g_password = trim(password);
    g_policy = policy;
}

bool persist_password(const std::string& password, std::string* err) {
    std::ofstream file(kPasswordFile, std::ios::trunc);
    if (!file) {
        if (err) *err = std::string("cannot write ") + kPasswordFile;
        return false;
    }
    const std::string value = trim(password);
    if (!value.empty()) file << value << "\n";
    file.flush();
    if (!file.good()) {
        if (err) *err = std::string("write failed: ") + kPasswordFile;
        return false;
    }
    return true;
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

int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int high = hex_value(value[i + 1]);
            const int low = hex_value(value[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return out;
}

bool password_enabled() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_password.empty();
}

bool password_matches(const std::string& candidate) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_password.empty() && constant_time_equal(candidate, g_password);
}

void prune_presence_locked(long long now) {
    for (auto it = g_presence.begin(); it != g_presence.end();) {
        if (now - it->second.last_ms > kPresenceStaleMs) it = g_presence.erase(it);
        else ++it;
    }
}

std::string session_json(bool host) {
    const long long now = now_ms();
    std::lock_guard<std::mutex> lock(g_mutex);
    prune_presence_locked(now);
    std::ostringstream out;
    out << "{\"ok\":true,\"host\":" << (host ? "true" : "false")
        << ",\"authRequired\":" << (!g_password.empty() ? "true" : "false")
        << ",\"remoteSave\":" << (g_policy.remote_save ? "true" : "false")
        << ",\"remoteAudio\":" << (g_policy.remote_audio ? "true" : "false")
        << ",\"disconnectPause\":" << (g_policy.disconnect_pause ? "true" : "false")
        << ",\"hostUnpauseOnly\":" << (g_policy.host_unpause_only ? "true" : "false")
        << ",\"disconnectGraceMs\":" << g_policy.disconnect_grace_ms
        << ",\"players\":[";
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
            std::string err;
            if (action_on_core_thread("pause", &err))
                diagnostics_log("session-policy: paused after the last browser player disconnected");
            else
                diagnostics_log("session-policy: disconnect pause skipped: " + err);
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
    }
    g_stop = false;
    g_watchdog = std::thread(watchdog_loop);
    diagnostics_log(std::string("session-policy: join password ") +
                    (password_enabled() ? "enabled" : "disabled"));
}

void session_policy_stop() {
    g_stop = true;
    if (g_watchdog.joinable()) g_watchdog.join();
}

bool session_request_is_public(const httplib::Request& req) {
    if (req.method == "OPTIONS") return true;
    if (req.path == "/" || req.path == "/view" || req.path == "/health" ||
        req.path == "/version" || req.path == "/join")
        return true;
    return req.path.rfind("/css/", 0) == 0 || req.path.rfind("/js/", 0) == 0 ||
           req.path.rfind("/asset/", 0) == 0 || req.path == "/favicon.ico";
}

bool session_request_authorized(const httplib::Request& req) {
    if (!password_enabled()) return true;
    const std::string credential =
        url_decode(cookie_value(req.get_header_value("Cookie"), "dfcap_auth"));
    return !credential.empty() && password_matches(credential);
}

bool session_request_is_host(const httplib::Request& req) {
    return guards::request_is_host_tab(req);
}

bool session_remote_audio_enabled() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_policy.remote_audio;
}

void session_presence_heartbeat(const std::string& player, const std::string& name) {
    if (player.empty()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    SessionPresence& entry = g_presence[player.substr(0, 64)];
    entry.name = name.substr(0, 32);
    entry.last_ms = now_ms();
}

bool session_action_allowed(const httplib::Request& req,
                            const std::string& action,
                            std::string* err) {
    bool host_unpause_only = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        host_unpause_only = g_policy.host_unpause_only;
    }
    const bool is_unpause =
        action == "play" || action == "resume" || action == "unpause" ||
        action == "toggle-pause";
    if (is_unpause && native_popup_blocked()) {
        if (err) *err = "dismiss the active fortress announcement before unpausing";
        return false;
    }
    if (host_unpause_only && is_unpause && !session_request_is_host(req)) {
        if (err) *err = "only the host may unpause under the current session policy";
        return false;
    }
    return true;
}

void register_session_policy_routes(httplib::Server& server) {
    server.Get("/version", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(std::string("{\"ok\":true,\"service\":\"dfcapture\",") +
            "\"authRequired\":" + (password_enabled() ? "true" : "false") + "}\n",
            "application/json; charset=utf-8");
    });

    auto join = [](const httplib::Request& req, httplib::Response& res) {
        const std::string password =
            req.has_param("password") ? req.get_param_value("password") : std::string();
        const bool ok = !password_enabled() || password_matches(password);
        res.status = ok ? 200 : 401;
        res.set_header("Cache-Control", "no-store");
        res.set_content(std::string("{\"ok\":") + (ok ? "true" : "false") +
            ",\"authRequired\":" + (password_enabled() ? "true" : "false") + "}\n",
            "application/json; charset=utf-8");
    };
    server.Get("/join", join);
    server.Post("/join", join);

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
        std::string old_password;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            next = g_policy;
            old_password = g_password;
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

        std::string next_password = old_password;
        if (req.has_param("password")) next_password = trim(req.get_param_value("password"));
        if (bool_param(req, "passwordOff", false)) next_password.clear();

        std::string err;
        if (!persist_policy(next, &err) || !persist_password(next_password, &err)) {
            res.status = 500;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_policy = next;
            g_password = next_password;
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
