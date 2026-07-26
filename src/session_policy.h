// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <string>

namespace httplib {
class Server;
struct Request;
}

namespace dfcapture {

void session_policy_start();
void session_policy_stop();

bool session_request_is_public(const httplib::Request& req);
bool session_request_authorized(const httplib::Request& req);
bool session_request_is_host(const httplib::Request& req);
bool session_remote_audio_enabled();

void session_presence_heartbeat(const std::string& player, const std::string& name);

bool session_action_allowed(const httplib::Request& req,
                            const std::string& action,
                            std::string* err = nullptr);

void register_session_policy_routes(httplib::Server& server);

} // namespace dfcapture
