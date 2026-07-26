// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#include "attribution.h"

#include "json_util.h"

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
    }
    g_world = save_dir;
}

void attrib_stamp(AttribKind kind, int32_t id, const std::string& player) {
    if (id < 0 || player.empty()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    map_for(kind)[id] = player.substr(0, 64);
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
    out << "}\n";
    return out.str();
}

} // namespace dfcapture
