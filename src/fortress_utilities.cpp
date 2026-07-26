// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#include "fortress_utilities.h"

#include "Core.h"
#include "json_util.h"
#include "save_barrier.h"
#include "sdl_capture.h"

#include "df/global_objects.h"
#include "df/hotkey_type.h"
#include "df/plotinfost.h"
#include "df/ui_hotkey.h"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <string>

namespace dfcapture {
namespace {

constexpr int kHotkeyEmpty = -30000;
std::recursive_mutex g_utility_mutex;

template <typename Fn>
bool run_locked(Fn&& fn) {
    std::lock_guard<std::recursive_mutex> utility_lock(g_utility_mutex);
    std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
    DFHack::CoreSuspender suspend;
    return !save_barrier_active() && fn();
}

void json_error(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content("{\"ok\":false,\"error\":" + json_string(message) + "}\n",
                    "application/json; charset=utf-8");
}

std::string hotkeys_json() {
    std::ostringstream out;
    bool ok = run_locked([&]() {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) return false;
        out << "{\"ok\":true,\"hotkeys\":[";
        for (int i = 0; i < 16; ++i) {
            auto& hotkey = plotinfo->main.hotkeys[i];
            bool set = hotkey.cmd == df::hotkey_type::Zoom && hotkey.x >= 0;
            if (i) out << ",";
            out << "{\"slot\":" << i
                << ",\"name\":" << json_string(hotkey.name)
                << ",\"set\":" << (set ? "true" : "false")
                << ",\"x\":" << hotkey.x << ",\"y\":" << hotkey.y
                << ",\"z\":" << hotkey.z << "}";
        }
        out << "]}\n";
        return true;
    });
    return ok ? out.str() : std::string();
}

bool hotkey_action(int slot, const std::string& action, bool has_xyz,
                   int x, int y, int z, const std::string& name, std::string& err) {
    if (slot < 0 || slot >= 16) {
        err = "slot out of range";
        return false;
    }
    return run_locked([&]() {
        auto plotinfo = df::global::plotinfo;
        if (!plotinfo) {
            err = "plotinfo unavailable";
            return false;
        }
        auto& hotkey = plotinfo->main.hotkeys[slot];
        if (action == "set") {
            if (!has_xyz) {
                err = "set requires x/y/z";
                return false;
            }
            hotkey.cmd = df::hotkey_type::Zoom;
            hotkey.x = x;
            hotkey.y = y;
            hotkey.z = z;
            if (!name.empty()) hotkey.name = name.substr(0, 128);
            else if (hotkey.name.empty()) hotkey.name = "Location " + std::to_string(slot + 1);
            return true;
        }
        if (action == "clear") {
            hotkey.cmd = df::hotkey_type::None;
            hotkey.name.clear();
            hotkey.x = hotkey.y = hotkey.z = kHotkeyEmpty;
            return true;
        }
        if (action == "rename") {
            hotkey.name = name.substr(0, 128);
            return true;
        }
        err = "unknown hotkey action";
        return false;
    });
}

std::string traffic_costs_json(int written) {
    auto plotinfo = df::global::plotinfo;
    if (!plotinfo) return {};
    std::ostringstream out;
    out << "{\"ok\":true,\"written\":" << written
        << ",\"costs\":{\"high\":" << plotinfo->main.traffic_cost_high
        << ",\"normal\":" << plotinfo->main.traffic_cost_normal
        << ",\"low\":" << plotinfo->main.traffic_cost_low
        << ",\"restricted\":" << plotinfo->main.traffic_cost_restricted
        << "}}\n";
    return out.str();
}

} // namespace

void register_fortress_utility_routes(httplib::Server& server) {
    server.Get("/hotkeys", [](const httplib::Request&, httplib::Response& res) {
        std::string json = hotkeys_json();
        if (json.empty()) {
            json_error(res, 503, "hotkeys unavailable");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    auto hotkey_handler = [](const httplib::Request& req, httplib::Response& res) {
        int slot = -1;
        if (!query_int(req, "slot", slot)) {
            json_error(res, 400, "missing slot");
            return;
        }
        const std::string action =
            req.has_param("action") ? req.get_param_value("action") : std::string();
        int x = 0, y = 0, z = 0;
        bool has_xyz = query_int(req, "x", x) && query_int(req, "y", y) &&
                       query_int(req, "z", z);
        const std::string name =
            req.has_param("name") ? req.get_param_value("name") : std::string();
        std::string err;
        if (!hotkey_action(slot, action, has_xyz, x, y, z, name, err)) {
            json_error(res, 400, err.empty() ? "hotkey action failed" : err);
            return;
        }
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/hotkey-action", hotkey_handler);
    server.Post("/hotkey-action", hotkey_handler);

    auto traffic_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string json;
        bool ok = run_locked([&]() {
            auto plotinfo = df::global::plotinfo;
            if (!plotinfo) return false;
            int written = 0;
            auto apply = [&](const char* param, int32_t& field) {
                int value = 0;
                if (!query_int(req, param, value)) return;
                field = std::max(1, std::min(10000, value));
                ++written;
            };
            apply("high", plotinfo->main.traffic_cost_high);
            apply("normal", plotinfo->main.traffic_cost_normal);
            apply("low", plotinfo->main.traffic_cost_low);
            apply("restricted", plotinfo->main.traffic_cost_restricted);
            json = traffic_costs_json(written);
            return true;
        });
        if (!ok || json.empty()) {
            json_error(res, 503, "traffic costs unavailable");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    };
    server.Get("/traffic-costs", traffic_handler);
    server.Post("/traffic-costs", traffic_handler);
}

} // namespace dfcapture
