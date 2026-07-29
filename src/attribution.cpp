// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#include "attribution.h"

#include "json_util.h"
#include "session_policy.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace dfcapture {
namespace {

std::mutex g_mutex;
std::string g_world;
std::unordered_map<int32_t, std::string> g_buildings;
std::unordered_map<int32_t, std::string> g_orders;
std::unordered_map<int32_t, std::string> g_stockpiles;
std::unordered_map<int32_t, std::string> g_zones;
uint64_t g_next_event_id = 1;

struct ActivityEvent {
    uint64_t id = 0;
    std::string save_dir;
    std::string actor;
    std::string actor_name;
    std::string action;
    AttribKind kind = AttribKind::Building;
    int32_t object_id = -1;
    long long timestamp_ms = 0;
};

std::deque<ActivityEvent> g_events;
constexpr size_t kMaxActivityEvents = 256;

const char* kind_name(AttribKind kind) {
    switch (kind) {
    case AttribKind::Order: return "order";
    case AttribKind::Stockpile: return "stockpile";
    case AttribKind::Zone: return "zone";
    default: return "building";
    }
}

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void append_event_locked(AttribKind kind, int32_t id, const std::string& player,
                         const std::string& action) {
    if (id < 0 || player.empty() || action.empty())
        return;
    ActivityEvent event;
    event.id = g_next_event_id++;
    event.save_dir = g_world;
    event.actor = player.substr(0, 64);
    event.actor_name = session_display_name(player).substr(0, 32);
    event.action = action.substr(0, 64);
    event.kind = kind;
    event.object_id = id;
    event.timestamp_ms = now_ms();
    g_events.push_back(std::move(event));
    while (g_events.size() > kMaxActivityEvents)
        g_events.pop_front();
}

std::unordered_map<int32_t, std::string>& map_for(AttribKind kind) {
    switch (kind) {
    case AttribKind::Order: return g_orders;
    case AttribKind::Stockpile: return g_stockpiles;
    case AttribKind::Zone: return g_zones;
    default: return g_buildings;
    }
}

void append_map(std::ostringstream& out, const char* name,
                const std::unordered_map<int32_t, std::string>& values) {
    out << "\"" << name << "\":{";
    bool first = true;
    for (const auto& entry : values) {
        if (!first) out << ",";
        first = false;
        out << "\"" << entry.first << "\":" << json_string(entry.second);
    }
    out << "}";
}

} // namespace

void attrib_note_world(const std::string& save_dir) {
    if (save_dir.empty()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_world.empty() && g_world != save_dir) {
        g_buildings.clear();
        g_orders.clear();
        g_stockpiles.clear();
        g_zones.clear();
        g_events.clear();
        g_next_event_id = 1;
    }
    g_world = save_dir;
}

void attrib_stamp(AttribKind kind, int32_t id, const std::string& player) {
    if (id < 0 || player.empty()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    map_for(kind)[id] = player.substr(0, 64);
    append_event_locked(kind, id, player, "created");
}

void attrib_record(AttribKind kind, int32_t id, const std::string& player,
                   const std::string& action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    append_event_locked(kind, id, player, action);
}

bool attrib_lookup(AttribKind kind, int32_t id, std::string& player) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto& values = map_for(kind);
    auto it = values.find(id);
    if (it == values.end()) return false;
    player = it->second;
    return true;
}

std::string attrib_json() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::ostringstream out;
    out << "{\"world\":" << json_string(g_world) << ",";
    append_map(out, "buildings", g_buildings);
    out << ",";
    append_map(out, "orders", g_orders);
    out << ",";
    append_map(out, "stockpiles", g_stockpiles);
    out << ",";
    append_map(out, "zones", g_zones);
    out << ",\"players\":{";
    bool first_player = true;
    for (const auto& player : session_players_snapshot()) {
        if (!first_player) out << ",";
        first_player = false;
        out << json_string(player.player_id) << ":"
            << json_string(player.name.empty() ? player.player_id : player.name);
    }
    out << "}";
    out << ",\"scope\":\"session\",\"events\":[";
    for (size_t i = 0; i < g_events.size(); ++i) {
        if (i) out << ",";
        const auto& event = g_events[i];
        out << "{\"eventId\":" << event.id
            << ",\"saveDir\":" << json_string(event.save_dir)
            << ",\"actorPlayerId\":" << json_string(event.actor)
            << ",\"actorDisplayName\":" << json_string(event.actor_name)
            << ",\"action\":" << json_string(event.action)
            << ",\"objectKind\":" << json_string(kind_name(event.kind))
            << ",\"objectId\":" << event.object_id
            << ",\"timestamp\":" << event.timestamp_ms << "}";
    }
    out << "]";
    out << "}\n";
    return out.str();
}

} // namespace dfcapture
