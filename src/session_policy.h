// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <vector>
#include <string>

namespace httplib {
class Server;
struct Request;
}

namespace dfcapture {

void session_policy_start();
void session_policy_stop();

bool session_request_is_host(const httplib::Request& req);
std::string session_request_player_id(const httplib::Request& req);
bool session_remote_audio_enabled();

void session_presence_heartbeat(const std::string& player, const std::string& name);
std::string session_display_name(const std::string& player);

struct SessionPlayer {
    std::string player_id;
    std::string name;
    long long age_ms = 0;
};

std::vector<SessionPlayer> session_players_snapshot();

struct PauseActionResult {
    bool ok = false;
    bool applied = false;
    bool paused = false;
    bool duplicate = false;
    bool superseded = false;
    bool forbidden = false;
    std::string error;
};

// Resolve policy, arbitration, idempotency, mutation, verification, and metadata as one suspended
// core-thread transaction. No caller may pre-read pause_state or record a transition separately.
bool session_apply_pause_request(const std::string& player, bool is_host,
                                 const std::string& action, const std::string& request_key,
                                 PauseActionResult& result);

void register_session_policy_routes(httplib::Server& server);

} // namespace dfcapture
