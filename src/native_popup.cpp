// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#include "native_popup.h"

#include "Core.h"
#include "json_util.h"
#include "sdl_capture.h"

#include "modules/Gui.h"

#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/popup_message.h"
#include "df/world.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace dfcapture {
namespace {

struct PopupSnapshot {
    int id = -1;
    const df::popup_message* pointer = nullptr;
    std::string raw;
    std::vector<std::string> lines;
};

std::mutex g_mutex;
std::unordered_map<const void*, int> g_ids;
int g_next_id = 1;
std::atomic<bool> g_blocked(false);

std::string plain_markup(const std::string& raw) {
    std::string out;
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] != '[') {
            if (raw[i] != '\r') out.push_back(raw[i]);
            ++i;
            continue;
        }
        if (i + 1 < raw.size() && raw[i + 1] == '[') {
            out.push_back('[');
            i += 2;
            continue;
        }
        size_t end = raw.find(']', i + 1);
        if (end == std::string::npos) break;
        std::string token = raw.substr(i + 1, end - i - 1);
        if (token == "R" || token == "P") out.push_back('\n');
        else if (token == "B") out += "\n\n";
        else if (token.rfind("CHAR:", 0) == 0) {
            std::string value = token.substr(5);
            if (value.size() == 2 && value[0] == '~') out.push_back(value[1]);
            else {
                try {
                    int code = std::stoi(value);
                    if (code >= 32 && code < 127) out.push_back(static_cast<char>(code));
                } catch (...) {}
            }
        }
        i = end + 1;
    }
    return out;
}

std::vector<std::string> split_lines(const std::string& raw) {
    std::vector<std::string> lines;
    std::istringstream in(plain_markup(raw));
    std::string line;
    while (std::getline(in, line) && lines.size() < 60) {
        if (line.size() > 400) line.resize(400);
        lines.push_back(line);
    }
    if (lines.empty()) lines.push_back("Announcement");
    return lines;
}

std::vector<PopupSnapshot> sample_popups() {
    std::vector<PopupSnapshot> result;
    DFHack::ConditionalCoreSuspender suspend;
    if (!suspend || !df::global::world) return result;

    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto popup : df::global::world->status.popups) {
        if (!popup) continue;
        PopupSnapshot row;
        row.pointer = popup;
        row.raw = popup->text;
        row.lines = split_lines(row.raw);
        auto found = g_ids.find(popup);
        if (found == g_ids.end())
            found = g_ids.emplace(popup, g_next_id++).first;
        row.id = found->second;
        result.push_back(std::move(row));
        if (result.size() >= 8) break;
    }
    g_blocked.store(!result.empty());
    return result;
}

std::string popup_json(const std::vector<PopupSnapshot>& popups) {
    std::ostringstream out;
    out << "{\"ok\":true,\"blocked\":" << (popups.empty() ? "false" : "true")
        << ",\"popups\":[";
    for (size_t i = 0; i < popups.size(); ++i) {
        if (i) out << ",";
        out << "{\"id\":" << popups[i].id
            << ",\"kind\":\"mega\",\"pauses\":true,\"text\":";
        append_json_string_array(out, popups[i].lines);
        out << "}";
    }
    out << "]}\n";
    return out.str();
}

bool dismiss_popup(int id, bool& already, std::string& err) {
    already = false;
    std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
    DFHack::CoreSuspender suspend;
    auto world = df::global::world;
    if (!world) {
        err = "world unavailable";
        return false;
    }
    if (world->status.popups.empty()) {
        already = true;
        g_blocked.store(false);
        return true;
    }

    auto front = world->status.popups.front();
    int front_id = -1;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto found = g_ids.find(front);
        if (found != g_ids.end()) front_id = found->second;
    }
    if (front_id != id) {
        err = "only the current announcement can be dismissed";
        return false;
    }

    world->status.popups.erase(world->status.popups.begin());
    DFHack::Gui::MTB_clean(&world->status.mega_text);
    if (!world->status.popups.empty() && world->status.popups.front()) {
        auto next = world->status.popups.front();
        DFHack::Gui::MTB_parse(&world->status.mega_text, next->text);
        DFHack::Gui::MTB_set_width(&world->status.mega_text);
        world->status.mega_portrait_hfid = next->portrait_hfid;
    } else {
        world->status.mega_portrait_hfid = -1;
    }
    if (df::global::gps && df::global::gps->force_full_display_count < 2)
        df::global::gps->force_full_display_count = 2;
    g_blocked.store(!world->status.popups.empty());
    return true;
}

} // namespace

bool native_popup_blocked() {
    // Do not rely on a browser having polled /popup before an unpause request arrives.
    // Sampling here keeps the global pause gate authoritative for host and remote clients.
    sample_popups();
    return g_blocked.load();
}

void register_native_popup_routes(httplib::Server& server) {
    server.Get("/popup", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(popup_json(sample_popups()), "application/json; charset=utf-8");
    });

    auto dismiss = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id) || id <= 0) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"invalid popup id\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        bool already = false;
        std::string err;
        if (!dismiss_popup(id, already, err)) {
            res.status = 409;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_content(std::string("{\"ok\":true,\"already\":") +
                            (already ? "true" : "false") + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/popup/dismiss", dismiss);
    server.Post("/popup/dismiss", dismiss);
}

} // namespace dfcapture
