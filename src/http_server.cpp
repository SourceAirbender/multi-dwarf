// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2025 - 2026 Gabriel Rios <grios019@gmail.com>
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

#include "http_server.h"

#include "building_zone.h"
#include "attribution.h"
#include "chat.h"
#include "client_state.h"
#include "diagnostics.h"
#include "hud.h"
#include "sdl_capture.h"
#include "httplib.h"
#include "image_encoder.h"
#include "info_panel.h"
#include "interaction.h"
#include "json_util.h"
#include "labor.h"
#include "lua_bridge.h"
#include "missions.h"
#include "notifications.h"
#include "native_popup.h"
#include "placement.h"
#include "player_ownership.h"
#include "save_barrier.h"
#include "session_policy.h"
#include "unit_sheet.h"
#include "unit_portrait.h"
#include "squads.h"
#include "standing_orders.h"
#include "stockpile_panel.h"
#include "stone_use.h"
#include "web_assets.h"
#include "work_orders.h"
#include "announcements.h"
#include "audio_stream.h"
#include "burrows_panel.h"
#include "console_routes.h"
#include "diplo.h"
#include "fort_admin.h"
#include "fortress_utilities.h"
#include "frame_delta.h"
#include "hauling.h"
#include "hospital.h"
#include "kitchen_panel.h"
#include "lever_link.h"
#include "trade_depot.h"
#include "vote.h"
#include "worldmap_panel.h"
#include "write_guards.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace dfcapture {
namespace {

std::mutex g_server_mutex;
std::unique_ptr<httplib::Server> g_server;
std::thread g_server_thread;
std::atomic<bool> g_running(false);
int g_port = DEFAULT_STREAM_PORT;
std::string g_bind_address = DEFAULT_BIND_ADDRESS;
std::mutex g_frame_wake_mutex;
std::condition_variable g_frame_wake_condition;
uint64_t g_frame_wake_next_generation = 1;
uint64_t g_world_frame_wake_generation = 1;
std::unordered_map<std::string, uint64_t> g_player_frame_wake_generations;

uint64_t frame_wake_generation_locked(const std::string& player) {
    const auto found = g_player_frame_wake_generations.find(player);
    return found == g_player_frame_wake_generations.end()
        ? g_world_frame_wake_generation
        : std::max(g_world_frame_wake_generation, found->second);
}

uint64_t wait_for_frame_wake(const std::string& player,
                             uint64_t known_generation, int hold_ms) {
    std::unique_lock<std::mutex> lock(g_frame_wake_mutex);
    if (hold_ms > 0 &&
            known_generation == frame_wake_generation_locked(player)) {
        g_frame_wake_condition.wait_for(
            lock, std::chrono::milliseconds(hold_ms),
            [&player, known_generation] {
                return !g_running.load() ||
                       frame_wake_generation_locked(player) != known_generation;
            });
    }
    return frame_wake_generation_locked(player);
}

void notify_player_camera_input(const std::string& player) {
    {
        std::lock_guard<std::mutex> lock(g_frame_wake_mutex);
        if (g_player_frame_wake_generations.size() >= 64 &&
                !g_player_frame_wake_generations.count(player)) {
            g_player_frame_wake_generations.clear();
            g_world_frame_wake_generation = ++g_frame_wake_next_generation;
        }
        g_player_frame_wake_generations[player] =
            ++g_frame_wake_next_generation;
    }
    g_frame_wake_condition.notify_all();
}

// --- Player presence (cursors + camera) -----------------------------------------------------
// A pure browser-to-browser relay: each client POSTs its cursor world-tile + camera, and every
// client GETs everyone else's. This touches NO DF state -- no CoreSuspender, no df:: access --
// so it adds zero sim interaction and zero crash surface. Stale entries are pruned on read.
struct PresenceEntry {
    bool has = false;            // is the cursor currently over the map?
    int x = 0, y = 0, z = 0;     // world tile under the cursor
    bool has_cam = false;
    int cx = 0, cy = 0, cz = 0;  // this player's camera origin
    int vw = 0, vh = 0;          // this player's viewport size in tiles (for minimap boxes)
    std::string tool;            // the tool/mode this player is holding, or ""
    std::string focus;           // what panel/entity this player has open, or ""
    bool has_drag = false;       // is this player mid-designation-drag?
    int dax = 0, day = 0, dbx = 0, dby = 0, daz = 0;  // drag rectangle (world tiles) + its z
    std::string name;
    std::string color;
    long long ts = 0;            // last update, steady-clock ms
};
std::mutex g_presence_mutex;     // guards both g_presence and g_pings below
std::unordered_map<std::string, PresenceEntry> g_presence;
constexpr long long PRESENCE_STALE_MS = 5000;

// Transient "look here" pings: a bounded recent-events buffer, folded into the /presence read.
struct PingEntry {
    long long id = 0;
    int x = 0, y = 0, z = 0;
    std::string name;
    std::string color;
    long long ts = 0;
};
std::vector<PingEntry> g_pings;
long long g_ping_next_id = 1;
constexpr long long PING_STALE_MS = 6000;

long long presence_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string presence_json(const std::string& exclude) {
    const long long now = presence_now_ms();
    std::lock_guard<std::mutex> lock(g_presence_mutex);
    for (auto it = g_presence.begin(); it != g_presence.end();) {
        if (now - it->second.ts > PRESENCE_STALE_MS) it = g_presence.erase(it);
        else ++it;
    }
    for (auto it = g_pings.begin(); it != g_pings.end();) {
        if (now - it->ts > PING_STALE_MS) it = g_pings.erase(it);
        else ++it;
    }
    std::ostringstream body;
    body << "{\"peers\":[";
    bool first = true;
    for (const auto& kv : g_presence) {
        if (kv.first == exclude) continue;
        const PresenceEntry& e = kv.second;
        if (!first) body << ",";
        first = false;
        body << "{\"player\":" << json_string(kv.first)
             << ",\"name\":" << json_string(e.name)
             << ",\"color\":" << json_string(e.color)
             << ",\"has\":" << (e.has ? "true" : "false")
             << ",\"x\":" << e.x << ",\"y\":" << e.y << ",\"z\":" << e.z
             << ",\"hasCam\":" << (e.has_cam ? "true" : "false")
             << ",\"cx\":" << e.cx << ",\"cy\":" << e.cy << ",\"cz\":" << e.cz
             << ",\"vw\":" << e.vw << ",\"vh\":" << e.vh
             << ",\"tool\":" << json_string(e.tool)
             << ",\"focus\":" << json_string(e.focus)
             << ",\"hasDrag\":" << (e.has_drag ? "true" : "false")
             << ",\"dax\":" << e.dax << ",\"day\":" << e.day
             << ",\"dbx\":" << e.dbx << ",\"dby\":" << e.dby << ",\"daz\":" << e.daz << "}";
    }
    body << "],\"pings\":[";
    first = true;
    for (const auto& p : g_pings) {
        if (!first) body << ",";
        first = false;
        body << "{\"id\":" << p.id
             << ",\"name\":" << json_string(p.name)
             << ",\"color\":" << json_string(p.color)
             << ",\"x\":" << p.x << ",\"y\":" << p.y << ",\"z\":" << p.z << "}";
    }
    body << "]}\n";
    return body.str();
}

std::string camera_json(const std::string& player, const Camera& camera) {
    return "{\"player\":" + json_string(player) +
           ",\"x\":" + std::to_string(camera.x) +
           ",\"y\":" + std::to_string(camera.y) +
           ",\"z\":" + std::to_string(camera.z) +
           ",\"zoom\":" + std::to_string(camera.zoom_factor >= 0 ? camera.zoom_factor : 100) +
           ",\"zoomExplicit\":" + (camera.zoom_factor >= 0 ? std::string("true") : std::string("false")) +
           "}\n";
}

std::string clients_json() {
    std::ostringstream body;
    auto clients = client_camera_snapshot();
    body << "{\"count\":" << clients.size() << ",\"clients\":[";
    for (size_t i = 0; i < clients.size(); ++i) {
        if (i) body << ",";
        body << "{\"player\":" << json_string(clients[i].player)
             << ",\"camera\":{\"x\":" << clients[i].camera.x
             << ",\"y\":" << clients[i].camera.y
             << ",\"z\":" << clients[i].camera.z
             << ",\"zoom\":" << (clients[i].camera.zoom_factor >= 0 ? clients[i].camera.zoom_factor : 100)
             << ",\"zoomExplicit\":" << (clients[i].camera.zoom_factor >= 0 ? "true" : "false")
             << "}}";
    }
    body << "]}\n";
    return body.str();
}

std::string build_options_from_request(const httplib::Request& req) {
    static const char* option_names[] = {
        "hollow", "weapon_count",
        "plate_units", "plate_water", "plate_magma", "plate_track", "plate_citizens",
        "plate_resets", "unit_min", "unit_max", "water_min", "water_max", "magma_min",
        "magma_max", "track_min", "track_max", "track_dump", "dump_x", "dump_y",
        "friction", "speed",
    };
    std::ostringstream out;
    for (auto name : option_names) {
        int value = 0;
        if (query_int(req, name, value))
            out << name << "=" << value << ";";
    }
    for (int i = 0; i < 4; ++i) {
        std::string key = "mat" + std::to_string(i);
        if (!req.has_param(key.c_str()))
            continue;
        std::string value = req.get_param_value(key.c_str());
        bool clean = value == "closest";
        if (!clean) {
            clean = !value.empty() && value.size() < 32;
            for (char c : value) {
                if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == ':')) {
                    clean = false;
                    break;
                }
            }
        }
        if (clean)
            out << key << "=" << value << ";";
    }
    // item0..item3 carry a specific item id per requirement. The Lua side
    // revalidates the item at commit; here we only forward a plausible non-negative integer.
    for (int i = 0; i < 4; ++i) {
        std::string key = "item" + std::to_string(i);
        int value = 0;
        if (query_int(req, key.c_str(), value) && value >= 0)
            out << key << "=" << value << ";";
    }
    return out.str();
}

constexpr int kMaxClientFrameDimension = 16384;

bool validate_frame_rect(int px, int py, int px2, int py2, int frame_w, int frame_h,
                         std::string& err) {
    if (frame_w <= 0 || frame_h <= 0 ||
        frame_w > kMaxClientFrameDimension || frame_h > kMaxClientFrameDimension) {
        err = "invalid frame dimensions";
        return false;
    }
    if (px < 0 || py < 0 || px2 < 0 || py2 < 0 ||
        px >= frame_w || px2 >= frame_w || py >= frame_h || py2 >= frame_h) {
        err = "selection lies outside the captured frame";
        return false;
    }
    return true;
}

void register_routes(httplib::Server& server) {
    // Saving, world teardown, and plugin shutdown invalidate broad portions of DF's object graph.
    // Stop every newly routed browser operation before static dispatch or a route can queue work
    // against those structures. Existing clients treat the temporary 503 like any missed poll.
    server.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        const auto starts_with = [&](const char* prefix) {
            return req.path.rfind(prefix, 0) == 0;
        };
        // Keep the page shell and read-only session status reachable during a save. This lets a
        // refreshed/new tab render the blocking save notice and automatically recover, while all
        // DF-world routes remain behind the barrier.
        const bool barrier_safe_get = req.method == "GET" && (
            req.path == "/" || req.path == "/view" || req.path == "/health" ||
            req.path == "/version" || req.path == "/session" ||
            starts_with("/js/") || starts_with("/css/") || starts_with("/fonts/") ||
            starts_with("/asset/") ||
            (req.path.size() >= 5 && req.path.ends_with(".json")));
        if (save_barrier_active() && !barrier_safe_get) {
            res.status = 503;
            res.set_header("Cache-Control", "no-store");
            res.set_header("Retry-After", "1");
            res.set_content(
                "{\"ok\":false,\"busy\":true,\"error\":\"Dwarf Fortress is saving, loading, or "
                "shutting down; retry shortly\"}\n",
                "application/json; charset=utf-8");
            return true;
        }
        return false;
    });

    server.set_mount_point("/asset", "data/vanilla/vanilla_interface/graphics/images");
    server.set_mount_point("/", web_root());

    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/view");
    });

    server.Get("/view", [](const httplib::Request&, httplib::Response& res) {
        // Never cache the page shell. Versioned <script>/<link> URLs handle asset freshness.
        res.set_header("Cache-Control", "no-store, must-revalidate");
        res.set_content(index_html(), "text/html; charset=utf-8");
    });

    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"ok\":true,\"service\":\"dfcapture\"}\n",
                        "application/json; charset=utf-8");
    });

    register_session_policy_routes(server);
    register_player_ownership_routes(server);
    register_audio_stream_routes(server);
    register_native_popup_routes(server);

    server.Get("/attrib", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(attrib_json(), "application/json; charset=utf-8");
    });

    server.Get("/state", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(diagnostics_json(player, camera, diagnostics_snapshot()),
                        "application/json; charset=utf-8");
    });

    server.Get("/frame-diagnostics", [](const httplib::Request& req, httplib::Response& res) {
        const std::string player = query_player(req);
        res.set_header("Cache-Control", "no-store");
        res.set_content(frame_pipeline_diagnostics_json(player),
                        "application/json; charset=utf-8");
    });

    server.Post("/frame-client-metrics", [](const httplib::Request& req,
                                             httplib::Response& res) {
        const auto metric = [&](const char* name, double fallback = 0.0) {
            if (!req.has_param(name)) return fallback;
            try {
                const double value = std::stod(req.get_param_value(name));
                return std::isfinite(value) ? std::max(-1.0, std::min(60000.0, value)) : fallback;
            } catch (...) {
                return fallback;
            }
        };
        int bytes = 0;
        query_int(req, "bytes", bytes);
        diagnostics_frame_client(
            query_player(req), metric("fetch"), metric("blob"), metric("decode"),
            metric("paint"), metric("total"), metric("inputVisible", -1.0),
            static_cast<uint64_t>(std::max(0, bytes)));
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    });

    server.Get("/host-state", [](const httplib::Request&, httplib::Response& res) {
        HostState state;
        std::string err;
        if (!host_state_on_render_thread(state, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + ",\"state\":" +
                                host_state_json(state) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(host_state_json(state), "application/json; charset=utf-8");
    });

    auto reset_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        forget_player_camera(player);
        diagnostics_reset();

        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(camera_json(player, camera), "application/json; charset=utf-8");
        notify_player_camera_input(player);
    };
    server.Get("/reset", reset_handler);
    server.Post("/reset", reset_handler);

    server.Get("/camera", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":\"" + err + "\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_content(camera_json(player, camera), "application/json; charset=utf-8");
    });

    server.Post("/camera", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera failed: " + err + "\n",
                            "text/plain; charset=utf-8");
            return;
        }

        bool has_absolute = req.has_param("x") || req.has_param("y") || req.has_param("z");
        if (has_absolute) {
            query_int(req, "x", camera.x);
            query_int(req, "y", camera.y);
            query_int(req, "z", camera.z);
        } else {
            int dx = 0;
            int dy = 0;
            int dz = 0;
            query_int(req, "dx", dx);
            query_int(req, "dy", dy);
            query_int(req, "dz", dz);
            camera.x += dx;
            camera.y += dy;
            camera.z += dz;
        }

        if (camera.z < 0)
            camera.z = 0;
        if (!clamp_camera(camera, &err)) {
            res.status = 503;
            res.set_content("camera failed: " + err + "\n",
                            "text/plain; charset=utf-8");
            return;
        }

        set_player_camera(player, camera);
        res.set_header("Cache-Control", "no-store");
        res.set_content(camera_json(player, camera), "application/json; charset=utf-8");
        notify_player_camera_input(player);
    });

    // Presence relay (see PresenceEntry above). No DF access; safe to serve at high frequency.
    server.Post("/presence", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        if (player.empty()) {
            res.status = 400;
            res.set_content("{\"ok\":false}\n", "application/json; charset=utf-8");
            return;
        }
        PresenceEntry e;
        e.ts = presence_now_ms();
        int has = 0;
        query_int(req, "has", has);
        e.has = (has != 0);
        query_int(req, "x", e.x);
        query_int(req, "y", e.y);
        query_int(req, "z", e.z);
        e.has_cam = req.has_param("cx") || req.has_param("cy") || req.has_param("cz");
        query_int(req, "cx", e.cx);
        query_int(req, "cy", e.cy);
        query_int(req, "cz", e.cz);
        query_int(req, "vw", e.vw);
        query_int(req, "vh", e.vh);
        e.tool = req.has_param("tool") ? req.get_param_value("tool").substr(0, 24) : std::string();
        e.focus = req.has_param("focus") ? req.get_param_value("focus").substr(0, 48) : std::string();
        int hasdrag = 0;
        query_int(req, "hasdrag", hasdrag);
        e.has_drag = (hasdrag != 0);
        query_int(req, "dax", e.dax);
        query_int(req, "day", e.day);
        query_int(req, "dbx", e.dbx);
        query_int(req, "dby", e.dby);
        query_int(req, "daz", e.daz);
        e.name = req.has_param("name") ? req.get_param_value("name").substr(0, 32) : player;
        e.color = req.has_param("color") ? req.get_param_value("color").substr(0, 32) : std::string();
        session_presence_heartbeat(player, e.name.empty() ? player : e.name);
        {
            std::lock_guard<std::mutex> lock(g_presence_mutex);
            g_presence[player] = std::move(e);
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    });

    server.Get("/presence", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        res.set_header("Cache-Control", "no-store");
        res.set_content(presence_json(player), "application/json; charset=utf-8");
    });

    server.Post("/ping", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        PingEntry p;
        p.ts = presence_now_ms();
        query_int(req, "x", p.x);
        query_int(req, "y", p.y);
        query_int(req, "z", p.z);
        p.name = req.has_param("name") ? req.get_param_value("name").substr(0, 32) : player;
        p.color = req.has_param("color") ? req.get_param_value("color").substr(0, 32) : std::string();
        {
            std::lock_guard<std::mutex> lock(g_presence_mutex);
            p.id = g_ping_next_id++;
            g_pings.push_back(std::move(p));
            if (g_pings.size() > 64)
                g_pings.erase(g_pings.begin(), g_pings.begin() + (g_pings.size() - 64));
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    });

    auto zoom_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        std::string direction = req.has_param("dir") ? req.get_param_value("dir") : "reset";
        Camera camera;
        std::string err;
        if (!zoom_player_camera(player, direction, camera, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(camera_json(player, camera), "application/json; charset=utf-8");
        notify_player_camera_input(player);
    };
    server.Get("/zoom", zoom_handler);
    server.Post("/zoom", zoom_handler);

    server.Get("/zoom-probe", [](const httplib::Request&, httplib::Response& res) {
        ViewportProbe probe;
        std::string err;
        if (!viewport_probe_on_render_thread(probe, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) +
                                ",\"probe\":" + viewport_probe_json(probe) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(viewport_probe_json(probe), "application/json; charset=utf-8");
    });

    server.Get("/grid-probe", [](const httplib::Request&, httplib::Response& res) {
        std::string json;
        std::string err;
        bool ok = grid_probe_on_render_thread(json, &err);
        res.set_header("Cache-Control", "no-store");
        res.status = ok ? 200 : 500;
        res.set_content(json + "\n", "application/json; charset=utf-8");
    });

    server.Get("/build-probe", [](const httplib::Request&, httplib::Response& res) {
        std::string json;
        std::string err;
        bool ok = build_probe_on_render_thread(json, &err);
        res.set_header("Cache-Control", "no-store");
        res.status = ok ? 200 : 500;
        res.set_content(json + "\n", "application/json; charset=utf-8");
    });

    auto placement_mode_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "none";
        bool active = !(mode.empty() || mode == "none" || mode == "0" || mode == "off");
        Camera camera;
        std::string err;
        if (!set_player_placement_mode(player, active, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"placementMode\":" +
                            std::string(camera.placement_mode ? "true" : "false") + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/placement-mode", placement_mode_handler);
    server.Post("/placement-mode", placement_mode_handler);

    auto placement_cursor_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int hx = -1;
        int hy = -1;
        int frame_w = 0;
        int frame_h = 0;
        int drag = 0;
        int drag_x = -1;
        int drag_y = -1;
        int build_w = 0;
        int build_h = 0;
        query_int(req, "hx", hx);
        query_int(req, "hy", hy);
        query_int(req, "w", frame_w);
        query_int(req, "h", frame_h);
        query_int(req, "drag", drag);
        query_int(req, "dx", drag_x);
        query_int(req, "dy", drag_y);
        query_int(req, "bw", build_w);
        query_int(req, "bh", build_h);

        Camera camera;
        std::string err;
        if (!set_player_placement_cursor(player, hx, hy, frame_w, frame_h, drag != 0,
                                         drag_x, drag_y, build_w, build_h, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/placement-cursor", placement_cursor_handler);
    server.Post("/placement-cursor", placement_cursor_handler);

    auto designate_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        DesignationRequest desig;
        if (!query_int(req, "px", desig.px) ||
                !query_int(req, "py", desig.py) ||
                !query_int(req, "w", desig.frame_w) ||
                !query_int(req, "h", desig.frame_h)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing px/py/w/h\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        desig.px2 = desig.px;
        desig.py2 = desig.py;
        query_int(req, "px2", desig.px2);
        query_int(req, "py2", desig.py2);
        std::string err;
        if (!validate_frame_rect(desig.px, desig.py, desig.px2, desig.py2,
                                 desig.frame_w, desig.frame_h, err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        desig.tool = req.has_param("tool") ? req.get_param_value("tool") : "dig";
        int marker = 0;
        int warm_damp = 0;
        query_int(req, "priority", desig.priority);
        query_int(req, "marker", marker);
        query_int(req, "warmdamp", warm_damp);
        query_int(req, "minemode", desig.mine_mode);
        desig.marker = marker != 0;
        desig.warm_damp = warm_damp != 0;
        // "traffic", "traffic-low|normal|high|restricted" -> level 0..3 for the traffic tool.
        if (desig.tool.rfind("traffic", 0) == 0) {
            desig.traffic_level = desig.tool.find("restricted") != std::string::npos ? 3
                : desig.tool.find("high") != std::string::npos ? 2
                : desig.tool.find("low") != std::string::npos ? 1 : 0;
        }

        Camera camera;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        DesignationResult result;
        if (!designate_on_render_thread(camera, desig, result, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"count\":" + std::to_string(result.count) +
                            ",\"tool\":" + json_string(result.tool) + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/designate", designate_handler);
    server.Post("/designate", designate_handler);

    server.Get("/lua-ping", [](const httplib::Request& req, httplib::Response& res) {
        int value = 41;
        query_int(req, "n", value);
        int out = 0;
        std::string err;
        if (!lua_ping(value, out, &err)) {
            res.status = 500;
            res.set_content("lua ping failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"value\":" + std::to_string(out) + "}\n",
                        "application/json; charset=utf-8");
    });

    server.Get("/build-catalog", [](const httplib::Request&, httplib::Response& res) {
        std::string err;
        std::string json = building_catalog_json_via_lua(&err);
        if (json.empty()) {
            res.status = 500;
            res.set_content("catalog failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    server.Get("/build-materials", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("token")) {
            res.status = 400;
            res.set_content("missing token\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = build_materials_json_via_lua(req.get_param_value("token"), &err);
        if (json.empty()) {
            res.status = 500;
            res.set_content("materials failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    // Return the specific items, not just materials, that satisfy each building requirement,
    // so the browser can offer exact finished-item selection. Read-only; the pick is passed back as
    // item0..item3 on /build-place and revalidated at commit.
    server.Get("/place-candidates", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("token")) {
            res.status = 400;
            res.set_content("missing token\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = build_candidates_json_via_lua(req.get_param_value("token"), &err);
        if (json.empty()) {
            res.status = 500;
            res.set_content("candidates failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    auto build_place_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int px = 0, py = 0, frame_w = 0, frame_h = 0;
        if (!query_int(req, "px", px) || !query_int(req, "py", py) ||
                !query_int(req, "w", frame_w) || !query_int(req, "h", frame_h) ||
                !req.has_param("token")) {
            res.status = 400;
            res.set_content("missing px/py/w/h/token\n", "text/plain; charset=utf-8");
            return;
        }
        int px2 = px, py2 = py, direction = -1;
        query_int(req, "px2", px2);
        query_int(req, "py2", py2);
        query_int(req, "direction", direction);

        Camera camera;
        std::string err;
        if (!validate_frame_rect(px, py, px2, py2, frame_w, frame_h, err)) {
            res.status = 400;
            res.set_content("invalid placement: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        int count = 0;
        int id = -1;
        std::string options = build_options_from_request(req);
        if (!place_building_via_lua(camera, px, py, px2, py2, frame_w, frame_h,
                                    req.get_param_value("token"), direction, options,
                                    count, id, &err)) {
            res.status = 400;
            res.set_content("building failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        attrib_stamp(AttribKind::Building, id, player);
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"count\":" + std::to_string(count) +
                        ",\"id\":" + std::to_string(id) + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/build-place", build_place_handler);
    server.Post("/build-place", build_place_handler);

    auto stockpile_create_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int px = 0, py = 0, frame_w = 0, frame_h = 0;
        if (!query_int(req, "px", px) || !query_int(req, "py", py) ||
                !query_int(req, "w", frame_w) || !query_int(req, "h", frame_h)) {
            res.status = 400;
            res.set_content("missing px/py/w/h\n", "text/plain; charset=utf-8");
            return;
        }
        int px2 = px, py2 = py;
        query_int(req, "px2", px2);
        query_int(req, "py2", py2);
        std::string preset = req.has_param("preset") ? req.get_param_value("preset") : "all";

        Camera camera;
        std::string err;
        if (!validate_frame_rect(px, py, px2, py2, frame_w, frame_h, err)) {
            res.status = 400;
            res.set_content("invalid stockpile: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        int id = -1;
        if (!create_stockpile_via_lua(camera, px, py, px2, py2, frame_w, frame_h,
                                      preset, id, &err)) {
            res.status = 400;
            res.set_content("stockpile failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        attrib_stamp(AttribKind::Stockpile, id, player);
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"id\":" + std::to_string(id) + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/stockpile", stockpile_create_handler);
    server.Post("/stockpile", stockpile_create_handler);

    auto zone_create_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int px = 0, py = 0, frame_w = 0, frame_h = 0;
        if (!query_int(req, "px", px) || !query_int(req, "py", py) ||
                !query_int(req, "w", frame_w) || !query_int(req, "h", frame_h)) {
            res.status = 400;
            res.set_content("missing px/py/w/h\n", "text/plain; charset=utf-8");
            return;
        }
        int px2 = px, py2 = py;
        query_int(req, "px2", px2);
        query_int(req, "py2", py2);
        // The web sends the zone kind as the short key `zone` (e.g. zone=pen); Lua create_zone maps
        // it (meeting->MeetingHall, pen->Pen, ...). The refactor read "type" here, which the web
        // never sends -> it always fell back to the default -> every zone became a Meeting Area.
        std::string zonetype = req.has_param("zone") ? req.get_param_value("zone") : "meeting";

        Camera camera;
        std::string err;
        if (!validate_frame_rect(px, py, px2, py2, frame_w, frame_h, err)) {
            res.status = 400;
            res.set_content("invalid zone: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        int id = -1;
        if (!create_zone_via_lua(camera, px, py, px2, py2, frame_w, frame_h, zonetype, id, &err)) {
            res.status = 400;
            res.set_content("zone failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        attrib_stamp(AttribKind::Zone, id, player);
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"id\":" + std::to_string(id) + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/zone", zone_create_handler);
    server.Post("/zone", zone_create_handler);

    register_work_order_routes(server);
    register_squad_routes(server);
    register_worldmap_routes(server);
    register_mission_routes(server);
    register_hospital_routes(server);
    register_kitchen_routes(server);
    register_lever_link_routes(server);
    register_trade_depot_routes(server);
    register_console_routes(server);
    guards::register_write_guard_routes(server);
    register_reports_routes(server);
    register_fort_admin_routes(server);
    register_fortress_utility_routes(server);
    register_diplo_routes(server);
    register_vote_routes(server);
    register_stone_use_routes(server);
    register_burrows_routes(server);
    register_hauling_routes(server);
    register_standing_orders_routes(server);

    server.Get("/frame.jpg", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera unavailable: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        std::vector<uint8_t> jpeg;
        CaptureGeometry geometry;
        FramePipelineTiming timing;
        if (!capture_camera_jpeg(camera, jpeg, &geometry, &err, &timing)) {
            res.status = 503;
            res.set_header("Retry-After", "1");
            res.set_content("capture failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        diagnostics_frame_pipeline(
            player, timing.capture_queue_ms, timing.render_wait_ms,
            timing.capture_ms, timing.target_setup_ms, timing.viewport_draw_ms,
            timing.readback_ms, timing.host_restore_ms, timing.encode_ms,
            timing.total_ms, jpeg.size(), timing.width, timing.height,
            timing.lower_viewports, timing.auxiliary_renders, timing.reused);

        res.set_header("Cache-Control", "no-store");
        res.set_header("X-DFCapture-Camera",
                       std::to_string(camera.x) + "," + std::to_string(camera.y) + "," +
                           std::to_string(camera.z));
        if (geometry.valid) {
            res.set_header("X-DFCapture-Grid",
                           std::to_string(geometry.origin_x) + "," +
                               std::to_string(geometry.origin_y) + "," +
                               std::to_string(geometry.zoom_factor) + "," +
                               std::to_string(geometry.viewport_width) + "," +
                               std::to_string(geometry.viewport_height));
        }
        res.set_content(reinterpret_cast<const char*>(jpeg.data()), jpeg.size(), "image/jpeg");
    });

    auto delta_handler = [](const httplib::Request& req, httplib::Response& res,
                            bool long_poll) {
        const std::string player = query_player(req);
        uint64_t base_sequence = 0;
        if (req.has_param("base")) {
            try {
                const std::string value = req.get_param_value("base");
                size_t consumed = 0;
                base_sequence = std::stoull(value, &consumed);
                if (consumed != value.size()) throw std::invalid_argument("trailing data");
            } catch (...) {
                res.status = 400;
                res.set_content("invalid base sequence\n", "text/plain; charset=utf-8");
                return;
            }
        }
        uint64_t wake_generation = 0;
        if (req.has_param("wake")) {
            try {
                const std::string value = req.get_param_value("wake");
                size_t consumed = 0;
                wake_generation = std::stoull(value, &consumed);
                if (consumed != value.size()) throw std::invalid_argument("trailing data");
            } catch (...) {
                res.status = 400;
                res.set_content("invalid wake generation\n",
                                "text/plain; charset=utf-8");
                return;
            }
        }
        int hold_ms = 0;
        if (long_poll && req.has_param("hold") &&
                !query_int(req, "hold", hold_ms)) {
            res.status = 400;
            res.set_content("invalid hold duration\n",
                            "text/plain; charset=utf-8");
            return;
        }
        hold_ms = std::clamp(hold_ms, 0, 250);
        const uint64_t acknowledged_wake =
            long_poll ? wait_for_frame_wake(
                player, wake_generation, hold_ms) : 0;
        const bool content_wake =
            long_poll && wake_generation != 0 &&
            acknowledged_wake != wake_generation;

        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera unavailable: " + err + "\n",
                            "text/plain; charset=utf-8");
            return;
        }
        const bool force_keyframe =
            req.has_param("force") && req.get_param_value("force") != "0";

        FrameDeltaResult delta;
        if (!capture_camera_delta(player, camera, base_sequence, force_keyframe,
                                  content_wake,
                                  delta, &err)) {
            res.status = 503;
            res.set_header("Retry-After", "1");
            res.set_content("capture failed: " + err + "\n",
                            "text/plain; charset=utf-8");
            return;
        }
        diagnostics_frame_pipeline(
            player, delta.timing.capture_queue_ms, delta.timing.render_wait_ms,
            delta.timing.capture_ms, delta.timing.target_setup_ms,
            delta.timing.viewport_draw_ms, delta.timing.readback_ms,
            delta.timing.host_restore_ms, delta.timing.encode_ms,
            delta.timing.total_ms, delta.packet.size(),
            delta.timing.width, delta.timing.height,
            delta.timing.lower_viewports, delta.timing.auxiliary_renders,
            delta.timing.reused, "delta", delta.keyframe,
            delta.rectangle_count, delta.changed_ratio, delta.keyframe_reason,
            delta.has_scroll);

        res.set_header("Cache-Control", "no-store");
        res.set_header("X-DFCapture-Transport",
                       "delta-v" + std::to_string(kFrameDeltaProtocol));
        res.set_header("X-DFCapture-Sequence", std::to_string(delta.sequence));
        res.set_header("X-DFCapture-Base", std::to_string(delta.base_sequence));
        res.set_header("X-DFCapture-Keyframe", delta.keyframe ? "1" : "0");
        res.set_header("X-DFCapture-Rects", std::to_string(delta.rectangle_count));
        res.set_header("X-DFCapture-Changed", std::to_string(delta.changed_ratio));
        if (long_poll)
            res.set_header("X-DFCapture-Wake", std::to_string(acknowledged_wake));
        res.set_header("X-DFCapture-Camera",
                       std::to_string(camera.x) + "," + std::to_string(camera.y) + "," +
                           std::to_string(camera.z));
        if (delta.geometry.valid) {
            res.set_header("X-DFCapture-Grid",
                           std::to_string(delta.geometry.origin_x) + "," +
                               std::to_string(delta.geometry.origin_y) + "," +
                               std::to_string(delta.geometry.zoom_factor) + "," +
                               std::to_string(delta.geometry.viewport_width) + "," +
                               std::to_string(delta.geometry.viewport_height));
        }
        res.set_content(reinterpret_cast<const char*>(delta.packet.data()),
                        delta.packet.size(), "application/x-dfcapture-delta");
    };
    server.Get("/frame.delta",
        [delta_handler](const httplib::Request& req, httplib::Response& res) {
            delta_handler(req, res, false);
        });
    server.Get("/frame.next",
        [delta_handler](const httplib::Request& req, httplib::Response& res) {
            delta_handler(req, res, true);
    });

    auto stream_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        auto last_frame = std::make_shared<std::chrono::steady_clock::time_point>(
            std::chrono::steady_clock::now() - std::chrono::milliseconds(1000));
        auto interval = std::chrono::milliseconds(1000 / DEFAULT_STREAM_FPS);

        res.set_header("Cache-Control", "no-store");
        res.set_header("Connection", "close");
        res.set_header("Content-Type", "multipart/x-mixed-replace; boundary=dfcapture");
        res.set_chunked_content_provider(
            [player, last_frame, interval](size_t, httplib::DataSink& sink) mutable {
                if (!g_running.load() || !sink.is_writable()) {
                    sink.done();
                    return;
                }

                auto now = std::chrono::steady_clock::now();
                auto elapsed = now - *last_frame;
                if (elapsed < interval)
                    std::this_thread::sleep_for(interval - elapsed);

                Camera camera;
                std::string err;
                if (!camera_for_player(player, camera, &err)) {
                    sink.done();
                    return;
                }

                std::vector<uint8_t> jpeg;
                CaptureGeometry geometry;
                FramePipelineTiming timing;
                if (!capture_camera_jpeg(camera, jpeg, &geometry, &err, &timing)) {
                    sink.done();
                    return;
                }
                diagnostics_frame_pipeline(
                    player, timing.capture_queue_ms, timing.render_wait_ms,
                    timing.capture_ms, timing.target_setup_ms,
                    timing.viewport_draw_ms, timing.readback_ms,
                    timing.host_restore_ms, timing.encode_ms,
                    timing.total_ms, jpeg.size(), timing.width, timing.height,
                    timing.lower_viewports, timing.auxiliary_renders, timing.reused);

                std::ostringstream header;
                header << "--dfcapture\r\n"
                       << "Content-Type: image/jpeg\r\n"
                       << "Content-Length: " << jpeg.size() << "\r\n"
                       << "X-DFCapture-Camera: " << camera.x << "," << camera.y << "," << camera.z
                       << "\r\n";
                if (geometry.valid) {
                    header << "X-DFCapture-Grid: " << geometry.origin_x << ","
                           << geometry.origin_y << "," << geometry.zoom_factor << ","
                           << geometry.viewport_width << "," << geometry.viewport_height << "\r\n";
                }
                header << "\r\n";
                std::string h = header.str();
                sink.write(h.data(), h.size());
                sink.write(reinterpret_cast<const char*>(jpeg.data()), jpeg.size());
                sink.write("\r\n", 2);
                *last_frame = std::chrono::steady_clock::now();
            });
    };

    server.Get("/stream", stream_handler);

    server.Get("/hud", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        HudState hud;
        if (!hud_on_render_thread(camera, hud, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(hud_json(player, hud), "application/json; charset=utf-8");
    });

    server.Get("/notifications", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        NotificationState state;
        std::string err;
        auto dismissed = dismissed_alert_keys_for_player(player);
        if (!notifications_on_render_thread(dismissed, state, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(notifications_json(player, state), "application/json; charset=utf-8");
    });

    auto notification_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        std::string action = req.has_param("action") ? req.get_param_value("action") : "";
        if (action == "dismiss") {
            if (!req.has_param("keys")) {
                res.status = 400;
                res.set_content("missing keys\n", "text/plain; charset=utf-8");
                return;
            }
            remember_dismissed_alert_keys(player, req.get_param_value("keys"));
            res.set_header("Cache-Control", "no-store");
            res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
            return;
        }
        res.status = 400;
        res.set_content("bad notification action\n", "text/plain; charset=utf-8");
    };
    server.Get("/notification-action", notification_action_handler);
    server.Post("/notification-action", notification_action_handler);

    server.Get("/zones", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        std::string json = zones_json_on_core_thread(player, camera, &err);
        if (json.empty()) {
            res.status = 500;
            res.set_content("zones failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    server.Get("/panel", [](const httplib::Request& req, httplib::Response& res) {
        std::string panel_name = req.has_param("panel") ? req.get_param_value("panel") : "citizens";
        std::string section = req.has_param("section") ? req.get_param_value("section") : "";
        std::string detail = req.has_param("detail") ? req.get_param_value("detail") : "";

        InfoPanel panel;
        std::string err;
        if (!info_panel_on_render_thread(panel_name, section, detail, panel, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(info_panel_json(panel), "application/json; charset=utf-8");
    });

    auto livestock_action_handler = [](const httplib::Request& req,
                                        httplib::Response& res) {
        int unit_id = -1, trainer_id = -1;
        if (!query_int(req, "unit", unit_id) || !req.has_param("action")) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing unit/action\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        query_int(req, "trainer", trainer_id);
        LivestockState state;
        std::string err;
        if (!livestock_action_on_core_thread(
                unit_id, req.get_param_value("action"), state, &err, trainer_id)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(livestock_state_json(unit_id, state),
                        "application/json; charset=utf-8");
    };
    server.Get("/livestock-action", livestock_action_handler);
    server.Post("/livestock-action", livestock_action_handler);

    server.Get("/unit", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int unit_id = -1;
        if (!query_int(req, "id", unit_id)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing id\"}\n",
                            "application/json; charset=utf-8");
            return;
        }

        UnitSheet unit;
        Camera tile;
        std::string err;
        if (!unit_sheet_on_render_thread(unit_id, unit, tile, &err)) {
            res.status = 404;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(unit_sheet_json(player, unit, tile), "application/json; charset=utf-8");
    });

    auto unit_nickname_handler = [](const httplib::Request& req, httplib::Response& res) {
        int unit_id = -1;
        if (!query_int(req, "id", unit_id) || !req.has_param("nickname")) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing id/nickname\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        std::string err;
        if (!set_unit_nickname_on_core_thread(
                unit_id, req.get_param_value("nickname"), &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Post("/unit-nickname", unit_nickname_handler);

    auto task_cancel_handler = [](const httplib::Request& req, httplib::Response& res) {
        int job_id = -1;
        if (!query_int(req, "job", job_id)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing job\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        std::string err;
        if (!cancel_job_on_core_thread(job_id, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Post("/task-cancel", task_cancel_handler);

    server.Get("/unit-portrait", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        int unit_id = -1;
        if (!query_int(req, "id", unit_id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }

        CapturedFrame frame;
        int32_t texpos = -1;
        std::string source;
        std::string err;
        bool icon_mode = req.has_param("mode") && req.get_param_value("mode") == "icon";
        bool generate = req.has_param("generate") &&
            (req.get_param_value("generate") == "1" ||
             req.get_param_value("generate") == "true" ||
             req.get_param_value("generate") == "yes");
        if (!unit_portrait_on_render_thread(unit_id, icon_mode, generate,
                                            frame, texpos, source, &err)) {
            res.status = 404;
            res.set_content("portrait failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        std::vector<uint8_t> png;
        if (!encode_png(frame, png, &err)) {
            res.status = 503;
            res.set_content("portrait encode failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        res.set_header("X-DFCapture-Texpos", std::to_string(texpos));
        res.set_header("X-DFCapture-Portrait-Source", source);
        res.set_content(reinterpret_cast<const char*>(png.data()), png.size(), "image/png");
    });

    auto action_handler = [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("action")) {
            res.status = 400;
            res.set_content("missing action\n", "text/plain; charset=utf-8");
            return;
        }

        const std::string action = req.get_param_value("action");
        const bool is_pause = action == "pause" || action == "play" || action == "resume" ||
                              action == "unpause" || action == "toggle-pause";
        if (is_pause) {
            const std::string key = req.has_param("key") ? req.get_param_value("key") : "";
            PauseActionResult result;
            if (!session_apply_pause_request(query_player(req), session_request_is_host(req),
                                             action, key, result)) {
                res.status = result.forbidden ? 403 : 400;
                res.set_content("{\"ok\":false,\"error\":" + json_string(result.error) + "}\n",
                                "application/json; charset=utf-8");
                return;
            }
            res.set_header("Cache-Control", "no-store");
            res.set_content("{\"ok\":true,\"applied\":" +
                                std::string(result.applied ? "true" : "false") +
                                ",\"paused\":" + (result.paused ? "true" : "false") +
                                ",\"duplicate\":" + (result.duplicate ? "true" : "false") +
                                ",\"superseded\":" + (result.superseded ? "true" : "false") +
                                "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        std::string err;
        if (!action_on_core_thread(action, &err)) {
            res.status = 400;
            res.set_content("action failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/action", action_handler);
    server.Post("/action", action_handler);

    auto stock_item_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int item_id = -1;
        if (!query_int(req, "id", item_id) || !req.has_param("action")) {
            res.status = 400;
            res.set_content("missing id/action\n", "text/plain; charset=utf-8");
            return;
        }

        StockItemActionResult result;
        if (!stock_item_action_on_core_thread(item_id, req.get_param_value("action"), result)) {
            res.status = 400;
            res.set_content("item action failed: " + result.err + "\n", "text/plain; charset=utf-8");
            return;
        }

        if (result.has_camera) {
            Camera camera = result.camera;
            std::string err;
            if (clamp_camera(camera, &err)) {
                result.camera = camera;
                set_player_camera(player, camera);
            }
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(stock_item_action_json(item_id, result), "application/json; charset=utf-8");
    };
    server.Get("/stock-item-action", stock_item_action_handler);
    server.Post("/stock-item-action", stock_item_action_handler);

    server.Get("/inspect", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int px = 0;
        int py = 0;
        int frame_w = 0;
        int frame_h = 0;
        if (!query_int(req, "px", px) || !query_int(req, "py", py) ||
            !query_int(req, "w", frame_w) || !query_int(req, "h", frame_h)) {
            res.status = 400;
            res.set_content("missing px/py/w/h\n", "text/plain; charset=utf-8");
            return;
        }

        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        InspectResult result;
        if (!inspect_on_core_thread(camera, px, py, frame_w, frame_h, result, &err)) {
            res.status = 503;
            res.set_content("inspect failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(inspect_json(player, result), "application/json; charset=utf-8");
    });

    server.Get("/tile-occupants", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int x = 0, y = 0, z = 0;
        std::string err;
        if (query_int(req, "x", x) && query_int(req, "y", y) && query_int(req, "z", z)) {
            std::string json = tile_occupants_at_json_on_core_thread(x, y, z, &err);
            if (json.empty()) {
                res.status = 503;
                res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                                "application/json; charset=utf-8");
                return;
            }
            res.set_header("Cache-Control", "no-store");
            res.set_content(json, "application/json; charset=utf-8");
            return;
        }
        int px = 0, py = 0, frame_w = 0, frame_h = 0;
        if (!query_int(req, "px", px) || !query_int(req, "py", py) ||
                !query_int(req, "w", frame_w) || !query_int(req, "h", frame_h)) {
            res.status = 400;
            res.set_content("missing x/y/z or px/py/w/h\n", "text/plain; charset=utf-8");
            return;
        }
        Camera camera;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        std::string json = tile_occupants_json_on_core_thread(
            camera, px, py, frame_w, frame_h, &err);
        if (json.empty()) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    server.Get("/engraving-info", [](const httplib::Request& req, httplib::Response& res) {
        int x = 0, y = 0, z = 0;
        if (!query_int(req, "x", x) || !query_int(req, "y", y) ||
                !query_int(req, "z", z)) {
            res.status = 400;
            res.set_content("missing x/y/z\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = engraving_info_json_on_core_thread(x, y, z, &err);
        if (json.empty()) {
            res.status = 404;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    server.Get("/hover", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int px = 0;
        int py = 0;
        int frame_w = 0;
        int frame_h = 0;
        if (!query_int(req, "px", px) || !query_int(req, "py", py) ||
            !query_int(req, "w", frame_w) || !query_int(req, "h", frame_h)) {
            res.status = 400;
            res.set_content("missing px/py/w/h\n", "text/plain; charset=utf-8");
            return;
        }

        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        HoverResult result;
        if (!hover_on_core_thread(camera, px, py, frame_w, frame_h, result, &err)) {
            res.status = 503;
            res.set_content("hover failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(hover_json(player, result), "application/json; charset=utf-8");
    });

    server.Get("/labor", [](const httplib::Request& req, httplib::Response& res) {
        int detail = -1;
        query_int(req, "detail", detail);
        LaborState state;
        std::string err;
        if (!build_labor_state(detail, state, &err)) {
            res.status = 503;
            res.set_content("labor failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(labor_json(state), "application/json; charset=utf-8");
    });

    auto labor_toggle_handler = [](const httplib::Request& req, httplib::Response& res) {
        int detail = -1;
        int unit_id = -1;
        int on = 0;
        if (!query_int(req, "detail", detail) || !query_int(req, "unit", unit_id) ||
            !query_int(req, "on", on)) {
            res.status = 400;
            res.set_content("missing detail/unit/on\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!labor_toggle_impl(detail, unit_id, on != 0, &err)) {
            res.status = 400;
            res.set_content("toggle failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/labor-toggle", labor_toggle_handler);
    server.Post("/labor-toggle", labor_toggle_handler);

    auto labor_mode_handler = [](const httplib::Request& req, httplib::Response& res) {
        int detail = -1;
        int mode = -1;
        if (!query_int(req, "detail", detail) || !query_int(req, "mode", mode)) {
            res.status = 400;
            res.set_content("missing detail/mode\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!labor_mode_impl(detail, mode, &err)) {
            res.status = 400;
            res.set_content("mode failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/labor-mode", labor_mode_handler);
    server.Post("/labor-mode", labor_mode_handler);

    auto labor_specialist_handler = [](const httplib::Request& req, httplib::Response& res) {
        int unit_id = -1;
        int on = 0;
        if (!query_int(req, "unit", unit_id) || !query_int(req, "on", on)) {
            res.status = 400;
            res.set_content("missing unit/on\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!labor_specialist_impl(unit_id, on != 0, &err)) {
            res.status = 400;
            res.set_content("specialist failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/labor-specialist", labor_specialist_handler);
    server.Post("/labor-specialist", labor_specialist_handler);

    auto labor_create_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.has_param("name") ? req.get_param_value("name") : "";
        int index = -1;
        std::string err;
        if (!labor_create_impl(name, &index, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"index\":" + std::to_string(index) + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/labor-create", labor_create_handler);
    server.Post("/labor-create", labor_create_handler);

    auto labor_rename_handler = [](const httplib::Request& req, httplib::Response& res) {
        int detail = -1;
        if (!query_int(req, "detail", detail) || !req.has_param("name")) {
            res.status = 400;
            res.set_content("missing detail/name\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!labor_rename_impl(detail, req.get_param_value("name"), &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/labor-rename", labor_rename_handler);
    server.Post("/labor-rename", labor_rename_handler);

    auto labor_delete_handler = [](const httplib::Request& req, httplib::Response& res) {
        int detail = -1;
        if (!query_int(req, "detail", detail)) {
            res.status = 400;
            res.set_content("missing detail\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!labor_delete_impl(detail, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/labor-delete", labor_delete_handler);
    server.Post("/labor-delete", labor_delete_handler);

    auto labor_task_handler = [](const httplib::Request& req, httplib::Response& res) {
        int detail = -1;
        int labor = -1;
        int on = 0;
        if (!query_int(req, "detail", detail) || !query_int(req, "labor", labor) ||
            !query_int(req, "on", on)) {
            res.status = 400;
            res.set_content("missing detail/labor/on\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!labor_task_toggle_impl(detail, labor, on != 0, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/labor-task-toggle", labor_task_handler);
    server.Post("/labor-task-toggle", labor_task_handler);

    server.Get("/building-info", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        BuildingPanelInfo info;
        if (!building_info_on_core_thread(id, info)) {
            res.status = 404;
            res.set_content("{\"error\":\"building not found\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(building_info_json(info) + "\n", "application/json; charset=utf-8");
    });

    auto building_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string action = req.has_param("action") ? req.get_param_value("action") : "";
        std::string err;
        if (!building_action_on_core_thread(id, action, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        attrib_record(AttribKind::Building, id, query_player(req),
                      action.empty() ? "changed" : action);
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/building-action", building_action_handler);
    server.Post("/building-action", building_action_handler);

    server.Get("/building-cage", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = building_cage_json_on_core_thread(id, &err);
        if (json.empty()) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json + "\n", "application/json; charset=utf-8");
    });

    server.Post("/building-cage-action", [](const httplib::Request& req,
                                              httplib::Response& res) {
        int id = -1;
        int target = -1;
        if (!query_int(req, "id", id) || !query_int(req, "target", target)) {
            res.status = 400;
            res.set_content("missing id/target\n", "text/plain; charset=utf-8");
            return;
        }
        const std::string kind =
            req.has_param("kind") ? req.get_param_value("kind") : "";
        const std::string action =
            req.has_param("action") ? req.get_param_value("action") : "";
        if (action != "assign" && action != "release") {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"invalid action\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        std::string err;
        if (!building_cage_action_on_core_thread(
                id, target, action == "assign", kind, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    });

    server.Get("/burial-coffin", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = burial_coffin_info_json_via_lua(id, &err);
        if (json.empty()) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    server.Post("/burial-coffin-action", [](const httplib::Request& req,
                                              httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        const std::string action =
            req.has_param("action") ? req.get_param_value("action") : "";
        std::string err;
        if (!burial_coffin_action_via_lua(id, action, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    });

    server.Post("/memorial-slab", [](const httplib::Request& req,
                                      httplib::Response& res) {
        int unit_id = -1;
        if (!query_int(req, "unit", unit_id)) {
            res.status = 400;
            res.set_content("missing unit\n", "text/plain; charset=utf-8");
            return;
        }
        std::string message;
        std::string err;
        if (!queue_memorial_slab_via_lua(unit_id, &message, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"message\":" + json_string(message) + "}\n",
                        "application/json; charset=utf-8");
    });

    auto building_rename_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        const std::string name = req.has_param("name") ? req.get_param_value("name") : "";
        std::string err;
        if (!building_rename_on_core_thread(id, name, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"name\":" + json_string(name.substr(0, 128)) + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/workshop-rename", building_rename_handler);
    server.Post("/workshop-rename", building_rename_handler);
    server.Get("/farm-plot-rename", building_rename_handler);
    server.Post("/farm-plot-rename", building_rename_handler);

    server.Get("/farm-plot", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = farm_plot_json_on_core_thread(id, &err);
        if (json.empty()) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json + "\n", "application/json; charset=utf-8");
    });

    server.Post("/farm-plot-action", [](const httplib::Request& req,
                                         httplib::Response& res) {
        int id = -1, season = -1, plant = -2;
        if (!query_int(req, "id", id) || !query_int(req, "season", season) ||
                !query_int(req, "plant", plant)) {
            res.status = 400;
            res.set_content("missing id/season/plant\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!farm_plot_set_season_crop_on_core_thread(id, season, plant, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    });
    register_chat_routes(server);

    server.Post("/farm-plot-fertilize-action", [](const httplib::Request& req,
                                                   httplib::Response& res) {
        int id = -1, seasonal = -1;
        if (!query_int(req, "id", id) || !query_int(req, "seasonal", seasonal) ||
                (seasonal != 0 && seasonal != 1)) {
            res.status = 400;
            res.set_content("missing/invalid id/seasonal\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!farm_plot_set_seasonal_fertilize_on_core_thread(
                id, seasonal != 0, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    });

    server.Get("/workshop-info", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = workshop_info_json_via_lua(id, &err);
        if (json.empty()) {
            res.status = 400;
            res.set_content("workshop info failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    auto workshop_add_job_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id) || !req.has_param("task")) {
            res.status = 400;
            res.set_content("missing id/task\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!workshop_add_job_via_lua(id, req.get_param_value("task"), &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/workshop-add-job", workshop_add_job_handler);
    server.Post("/workshop-add-job", workshop_add_job_handler);

    auto workshop_job_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int job_id = -1;
        if (!query_int(req, "id", id) || !query_int(req, "job", job_id) ||
                !req.has_param("action")) {
            res.status = 400;
            res.set_content("missing id/job/action\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!workshop_job_action_via_lua(id, job_id, req.get_param_value("action"), &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/workshop-job-action", workshop_job_action_handler);
    server.Post("/workshop-job-action", workshop_job_action_handler);

    auto workshop_worker_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int unit = -1;
        int assign = 0;
        if (!query_int(req, "id", id) || !query_int(req, "unit", unit)) {
            res.status = 400;
            res.set_content("missing id/unit\n", "text/plain; charset=utf-8");
            return;
        }
        query_int(req, "assign", assign);
        std::string err;
        if (!workshop_worker_action_via_lua(id, unit, assign != 0, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/workshop-worker-action", workshop_worker_action_handler);
    server.Post("/workshop-worker-action", workshop_worker_action_handler);

    auto workshop_workers_clear_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!workshop_workers_clear_via_lua(id, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/workshop-workers-clear", workshop_workers_clear_handler);
    server.Post("/workshop-workers-clear", workshop_workers_clear_handler);

    auto workshop_profile_handler = [](const httplib::Request& req,
                                        httplib::Response& res) {
        int id = -1, value = 0;
        if (!query_int(req, "id", id) || !req.has_param("field")) {
            res.status = 400;
            res.set_content("missing id/field\n", "text/plain; charset=utf-8");
            return;
        }
        query_int(req, "value", value);
        std::string err;
        if (!workshop_profile_set_via_lua(
                id, req.get_param_value("field"), value, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/workshop-profile", workshop_profile_handler);
    server.Post("/workshop-profile", workshop_profile_handler);

    server.Get("/zone-info", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        ZonePanelInfo info;
        if (!zone_info_on_core_thread(id, info)) {
            res.status = 404;
            res.set_content("{\"error\":\"zone not found\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(zone_info_json(info) + "\n", "application/json; charset=utf-8");
    });

    auto zone_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string action = req.has_param("action") ? req.get_param_value("action") : "";
        std::string err;
        if (!zone_action_on_core_thread(id, action, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        attrib_record(AttribKind::Zone, id, query_player(req),
                      action.empty() ? "changed" : action);
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/zone-action", zone_action_handler);
    server.Post("/zone-action", zone_action_handler);

    auto zone_rename_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id) || !req.has_param("name")) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing id/name\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        std::string err;
        if (!building_rename_on_core_thread(id, req.get_param_value("name"), &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        attrib_record(AttribKind::Zone, id, query_player(req), "renamed");
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Post("/zone-rename", zone_rename_handler);

    auto zone_repaint_handler = [](const httplib::Request& req, httplib::Response& res) {
        const std::string player = query_player(req);
        int id = -1, px = 0, py = 0, px2 = 0, py2 = 0, frame_w = 0, frame_h = 0;
        if (!query_int(req, "id", id) || !query_int(req, "px", px) ||
                !query_int(req, "py", py) || !query_int(req, "w", frame_w) ||
                !query_int(req, "h", frame_h)) {
            res.status = 400;
            res.set_content("missing id/px/py/w/h\n", "text/plain; charset=utf-8");
            return;
        }
        px2 = px;
        py2 = py;
        query_int(req, "px2", px2);
        query_int(req, "py2", py2);
        const std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "add";

        std::string err;
        if (!validate_frame_rect(px, py, px2, py2, frame_w, frame_h, err)) {
            res.status = 400;
            res.set_content("invalid repaint: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        Camera camera;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        int view_w = 0, view_h = 0;
        if (!effective_capture_viewport_dims(camera, view_w, view_h, &err) ||
                view_w <= 0 || view_h <= 0) {
            res.status = 503;
            res.set_content("viewport failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        const auto pixel_to_tile = [](int pixel, int tiles, int frame) {
            return std::max(0, std::min(tiles - 1, (pixel * tiles) / frame));
        };
        const int x1 = camera.x + pixel_to_tile(std::min(px, px2), view_w, frame_w);
        const int y1 = camera.y + pixel_to_tile(std::min(py, py2), view_h, frame_h);
        const int x2 = camera.x + pixel_to_tile(std::max(px, px2), view_w, frame_w);
        const int y2 = camera.y + pixel_to_tile(std::max(py, py2), view_h, frame_h);

        ZoneRepaintPlan plan;
        if (!plan_zone_repaint_on_core_thread(id, x1, y1, x2, y2, mode, plan, &err)) {
            res.status = 400;
            res.set_content("zone repaint failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        if (plan.removed) {
            res.status = 409;
            res.set_content("zone repaint refused: cannot erase an entire zone\n",
                            "text/plain; charset=utf-8");
            return;
        }
        if (plan.changed && !apply_zone_repaint_in_place_on_core_thread(id, plan, &err)) {
            res.status = 400;
            res.set_content("zone repaint failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        if (plan.changed)
            attrib_record(AttribKind::Zone, id, player, "repainted");
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"id\":" + std::to_string(id) +
                            (plan.changed ? "}\n" : ",\"unchanged\":true}\n"),
                        "application/json; charset=utf-8");
    };
    server.Post("/zone-repaint", zone_repaint_handler);

    server.Get("/zone-squads", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing id\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = zone_squads_json_on_core_thread(id, &err);
        if (json.empty()) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json + "\n", "application/json; charset=utf-8");
    });

    auto zone_squad_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1, squad = -1, enabled = 0;
        if (!query_int(req, "id", id) || !query_int(req, "squad", squad) ||
                !query_int(req, "enabled", enabled) || !req.has_param("mode")) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing id/squad/mode/enabled\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        std::string err;
        if (!zone_squad_action_on_core_thread(
                id, squad, req.get_param_value("mode"), enabled != 0, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Post("/zone-squad-action", zone_squad_action_handler);

    server.Get("/zone-units", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = zone_units_json_on_core_thread(id, &err);
        if (json.empty()) {
            res.status = 400;
            res.set_content("zone units failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json + "\n", "application/json; charset=utf-8");
    });

    auto zone_unit_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int unit = -1;
        int assign = 0;
        if (!query_int(req, "id", id) || !query_int(req, "unit", unit)) {
            res.status = 400;
            res.set_content("missing id/unit\n", "text/plain; charset=utf-8");
            return;
        }
        query_int(req, "assign", assign);
        std::string kind = req.has_param("kind") ? req.get_param_value("kind") : "unit";
        std::string err;
        if (!zone_unit_action_on_core_thread(id, unit, assign != 0, kind, &err)) {
            res.status = 400;
            res.set_content("zone unit action failed: " + err + "\n",
                            "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/zone-unit-action", zone_unit_action_handler);
    server.Post("/zone-unit-action", zone_unit_action_handler);

    server.Get("/zone-owners", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = zone_owners_json_on_core_thread(id, &err);
        if (json.empty()) {
            res.status = 400;
            res.set_content("zone owners failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json + "\n", "application/json; charset=utf-8");
    });

    auto zone_owner_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int unit = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        query_int(req, "unit", unit);
        std::string err;
        if (!zone_owner_action_on_core_thread(id, unit, &err)) {
            res.status = 400;
            res.set_content("zone owner action failed: " + err + "\n",
                            "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/zone-owner-action", zone_owner_action_handler);
    server.Post("/zone-owner-action", zone_owner_action_handler);

    server.Get("/zone-locations", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = zone_locations_json_via_lua(id, &err);
        if (json.empty()) {
            res.status = 400;
            res.set_content("zone locations failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json + "\n", "application/json; charset=utf-8");
    });

    auto zone_location_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int location = -1;
        if (!query_int(req, "id", id) || !req.has_param("action")) {
            res.status = 400;
            res.set_content("missing id/action\n", "text/plain; charset=utf-8");
            return;
        }
        query_int(req, "location", location);
        std::string kind = req.has_param("kind") ? req.get_param_value("kind") : "";
        std::string err;
        if (!zone_location_action_via_lua(id, req.get_param_value("action"), kind,
                                          location, &err)) {
            res.status = 400;
            res.set_content("zone location action failed: " + err + "\n",
                            "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/zone-location-action", zone_location_action_handler);
    server.Post("/zone-location-action", zone_location_action_handler);

    server.Get("/location-detail", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing id\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = location_detail_json_via_lua(id, &err);
        if (json.empty()) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json + "\n", "application/json; charset=utf-8");
    });

    auto location_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int unit = -1;
        if (!query_int(req, "id", id) || !req.has_param("action")) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing id/action\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        query_int(req, "unit", unit);
        int value = 0;
        query_int(req, "value", value);
        const std::string kind = req.has_param("kind") ? req.get_param_value("kind") : "";
        std::string err;
        if (!location_action_via_lua(
                id, req.get_param_value("action"), kind, unit, value, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Post("/location-action", location_action_handler);

    server.Get("/stockpile-info", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string json = stockpile_info_json_on_core_thread(id);
        if (json.empty()) {
            res.status = 404;
            res.set_content("not a stockpile\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    auto stockpile_rename_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id) || !req.has_param("name")) {
            res.status = 400;
            res.set_content("missing id/name\n", "text/plain; charset=utf-8");
            return;
        }
        bool ok = rename_stockpile_on_core_thread(id, req.get_param_value("name"));
        if (ok)
            attrib_record(AttribKind::Stockpile, id, query_player(req), "renamed");
        res.set_header("Cache-Control", "no-store");
        res.set_content(ok ? "{\"ok\":true}\n" : "{\"ok\":false}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/stockpile-rename", stockpile_rename_handler);
    server.Post("/stockpile-rename", stockpile_rename_handler);

    auto stockpile_remove_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        bool ok = remove_stockpile_on_core_thread(id);
        if (ok)
            attrib_record(AttribKind::Stockpile, id, query_player(req), "removed");
        res.set_header("Cache-Control", "no-store");
        res.set_content(ok ? "{\"ok\":true}\n" : "{\"ok\":false}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/stockpile-remove", stockpile_remove_handler);
    server.Post("/stockpile-remove", stockpile_remove_handler);

    auto stockpile_links_only_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int on = 0;
        if (!query_int(req, "id", id) || !query_int(req, "on", on)) {
            res.status = 400;
            res.set_content("missing id/on\n", "text/plain; charset=utf-8");
            return;
        }
        bool ok = set_stockpile_links_only_on_core_thread(id, on != 0);
        if (ok)
            attrib_record(AttribKind::Stockpile, id, query_player(req), "link policy changed");
        res.set_header("Cache-Control", "no-store");
        res.set_content(ok ? "{\"ok\":true}\n" : "{\"ok\":false}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/stockpile-links-only", stockpile_links_only_handler);
    server.Post("/stockpile-links-only", stockpile_links_only_handler);

    auto stockpile_storage_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        int barrels = -1;
        int bins = -1;
        int wheelbarrows = -1;
        query_int(req, "barrels", barrels);
        query_int(req, "bins", bins);
        query_int(req, "wheelbarrows", wheelbarrows);
        bool ok = set_stockpile_storage_on_core_thread(id, barrels, bins, wheelbarrows);
        if (ok)
            attrib_record(AttribKind::Stockpile, id, query_player(req), "storage changed");
        res.set_header("Cache-Control", "no-store");
        res.set_content(ok ? "{\"ok\":true}\n" : "{\"ok\":false}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/stockpile-storage", stockpile_storage_handler);
    server.Post("/stockpile-storage", stockpile_storage_handler);

    auto stockpile_link_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int target = -1;
        int on = 1;
        if (!query_int(req, "id", id) || !query_int(req, "target", target) ||
            !req.has_param("mode")) {
            res.status = 400;
            res.set_content("missing id/target/mode\n", "text/plain; charset=utf-8");
            return;
        }
        query_int(req, "on", on);
        std::string err;
        if (!set_stockpile_link_on_core_thread(id, target, req.get_param_value("mode"),
                                               on != 0, &err)) {
            res.status = 400;
            res.set_content("link failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        attrib_record(AttribKind::Stockpile, id, query_player(req), "links changed");
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/stockpile-link", stockpile_link_handler);
    server.Post("/stockpile-link", stockpile_link_handler);

    auto stockpile_set_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id) || !req.has_param("preset")) {
            res.status = 400;
            res.set_content("missing id/preset\n", "text/plain; charset=utf-8");
            return;
        }
        std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "set";
        std::string err;
        if (!stockpile_set_preset_via_lua(id, req.get_param_value("preset"), mode, &err)) {
            res.status = 400;
            res.set_content("set failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        attrib_record(AttribKind::Stockpile, id, query_player(req), "contents changed");
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/stockpile-set", stockpile_set_handler);
    server.Post("/stockpile-set", stockpile_set_handler);

    auto stockpile_group_from_request = [](const httplib::Request& req) {
        return req.has_param("group") ? req.get_param_value("group") : std::string();
    };

    server.Get("/stockpile-cat-groups", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("cat")) {
            res.status = 400;
            res.set_content("missing cat\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = stockpile_groups_via_lua(req.get_param_value("cat"), &err);
        if (json.empty()) {
            res.status = 500;
            res.set_content("groups failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    server.Get("/stockpile-items", [stockpile_group_from_request](const httplib::Request& req,
                                                                 httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id) || !req.has_param("cat")) {
            res.status = 400;
            res.set_content("missing id/cat\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = stockpile_items_via_lua(id, req.get_param_value("cat"),
                                                   stockpile_group_from_request(req), &err);
        if (json.empty()) {
            res.status = 500;
            res.set_content("items failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    auto stockpile_toggle_item_handler =
        [stockpile_group_from_request](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int idx = -1;
        int on = 0;
        if (!query_int(req, "id", id) || !req.has_param("cat") ||
                !query_int(req, "idx", idx)) {
            res.status = 400;
            res.set_content("missing id/cat/idx\n", "text/plain; charset=utf-8");
            return;
        }
        query_int(req, "on", on);
        std::string err;
        if (!stockpile_toggle_item_via_lua(id, req.get_param_value("cat"),
                                           stockpile_group_from_request(req),
                                           idx, on != 0, &err)) {
            res.status = 400;
            res.set_content("toggle failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/stockpile-toggle-item", stockpile_toggle_item_handler);
    server.Post("/stockpile-toggle-item", stockpile_toggle_item_handler);

    auto stockpile_toggle_all_handler =
        [stockpile_group_from_request](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int on = 0;
        if (!query_int(req, "id", id) || !req.has_param("cat")) {
            res.status = 400;
            res.set_content("missing id/cat\n", "text/plain; charset=utf-8");
            return;
        }
        query_int(req, "on", on);
        std::string err;
        if (!stockpile_toggle_all_via_lua(id, req.get_param_value("cat"),
                                          stockpile_group_from_request(req),
                                          on != 0, &err)) {
            res.status = 400;
            res.set_content("toggle-all failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/stockpile-toggle-all", stockpile_toggle_all_handler);
    server.Post("/stockpile-toggle-all", stockpile_toggle_all_handler);

    auto stockpile_repaint_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int id = -1;
        int px = 0;
        int py = 0;
        int frame_w = 0;
        int frame_h = 0;
        if (!query_int(req, "id", id) ||
                !query_int(req, "px", px) || !query_int(req, "py", py) ||
                !query_int(req, "w", frame_w) || !query_int(req, "h", frame_h)) {
            res.status = 400;
            res.set_content("missing id/px/py/w/h\n", "text/plain; charset=utf-8");
            return;
        }
        int px2 = px;
        int py2 = py;
        query_int(req, "px2", px2);
        query_int(req, "py2", py2);

        Camera camera;
        std::string err;
        if (!validate_frame_rect(px, py, px2, py2, frame_w, frame_h, err)) {
            res.status = 400;
            res.set_content("invalid repaint: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        int new_id = -1;
        if (!create_stockpile_via_lua(camera, px, py, px2, py2, frame_w, frame_h,
                                      "none", new_id, &err)) {
            res.status = 400;
            res.set_content("repaint failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        int final_id = new_id;
        if (!finish_stockpile_repaint_on_core_thread(id, new_id, final_id, &err)) {
            remove_stockpile_on_core_thread(new_id);
            res.status = 400;
            res.set_content("repaint failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"id\":" + std::to_string(final_id) + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/stockpile-repaint", stockpile_repaint_handler);
    server.Post("/stockpile-repaint", stockpile_repaint_handler);
}

} // namespace

// Wake any pending delta long-polls immediately after an input or world mutation. A monotonically
// increasing generation closes the race where input lands just before the browser opens its next
// request: the stale generation in that request still causes an immediate capture.
void notify_player_input() {
    {
        std::lock_guard<std::mutex> lock(g_frame_wake_mutex);
        g_world_frame_wake_generation = ++g_frame_wake_next_generation;
    }
    g_frame_wake_condition.notify_all();
}

std::string server_url(const std::string& bind_address, int port) {
    std::string host = bind_address == "0.0.0.0" ? "127.0.0.1" : bind_address;
    return "http://" + host + ":" + std::to_string(port) + "/view";
}

std::string server_url() {
    std::lock_guard<std::mutex> lock(g_server_mutex);
    return server_url(g_bind_address, g_port);
}

bool server_running() {
    return g_running.load();
}

bool start_server(int port, const std::string& bind_address, std::string* err) {
    std::lock_guard<std::mutex> lock(g_server_mutex);
    if (g_server) {
        if (err) *err = "server is already running";
        return false;
    }

    session_policy_start();
    auto server = std::make_unique<httplib::Server>();
    diagnostics_log("server startup: registering routes");
    register_routes(*server);
    diagnostics_log("server startup: binding " + bind_address + ":" +
                    std::to_string(port));

    if (!server->bind_to_port(bind_address.c_str(), port)) {
#ifdef _WIN32
        const int socket_error = WSAGetLastError();
#else
        const int socket_error = 0;
#endif
        session_policy_stop();
        std::string message =
            "failed to bind " + bind_address + ":" + std::to_string(port);
        if (socket_error != 0)
            message += " (Windows socket error " + std::to_string(socket_error) + ")";
        message +=
            "; the port is already in use or reserved. Close the conflicting "
            "application, or choose another port in the DFCapture launcher";
        diagnostics_log("server startup failed: " + message);
        if (err) *err = message;
        return false;
    }

    diagnostics_log("server startup: port bound; starting listener");
    g_port = port;
    g_bind_address = bind_address;
    g_running = true;
    g_server = std::move(server);
    g_server_thread = std::thread([] {
        g_server->listen_after_bind();
        g_running = false;
    });
    return true;
}

void stop_server() {
    std::unique_ptr<httplib::Server> server;
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(g_server_mutex);
        if (!g_server) {
            session_policy_stop();
            return;
        }
        g_running = false;
        notify_player_input();
        g_server->stop();
        server = std::move(g_server);
        thread = std::move(g_server_thread);
    }

    if (thread.joinable())
        thread.join();
    reset_frame_delta_states();
    session_policy_stop();
    g_running = false;
}

} // namespace dfcapture
