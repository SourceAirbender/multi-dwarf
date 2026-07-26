// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#include "audio_stream.h"

#include "diagnostics.h"
#include "httplib.h"
#include "json_util.h"
#include "session_policy.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace dfcapture {
namespace {

namespace fs = std::filesystem;

constexpr std::uintmax_t kMaximumSoundBytes = 64ull * 1024ull * 1024ull;

struct Track {
    const char* key;
    const char* label;
    const char* path;
};

constexpr Track kTracks[] = {
    {"hill_dwarf", "Hill Dwarf", "tracks/hill_dwarf/HD_Full.ogg"},
    {"strange_moods", "Strange Moods", "tracks/strange_moods/SM_Full.ogg"},
    {"mountainhome", "Mountainhome", "tracks/mountainhome/MH_Full.ogg"},
    {"craftsdwarfship", "Craftsdwarfship", "tracks/craftsdwarfship/CS_Full.ogg"},
    {"nabidas", "Nabidas", "tracks/nabidas/Nabidas.ogg"},
    {"first_year", "First Year", "tracks/first_year/FY_Full.ogg"},
    {"another_year", "Another Year", "tracks/another_year/AY_Full.ogg"},
    {"winter_entombs_you", "Winter Entombs You",
     "tracks/winter_entombs_you/WEY_Full.ogg"},
    {"expansive_cavern", "Expansive Cavern",
     "tracks/expansive_cavern/EC_Full.ogg"},
    {"forgotten_beast", "Forgotten Beast",
     "tracks/forgotten_beast/FB_Full.ogg"},
    {"vile_force_of_darkness", "Vile Force of Darkness",
     "tracks/vile_force_of_darkness/VFOD_Full.ogg"},
    {"koganusan", "Koganusan", "tracks/koganusan/KG_Full.ogg"},
    {"death_spiral", "Death Spiral", "tracks/death_spiral/DS_Full.ogg"},
    {"strike_the_earth", "Strike the Earth!",
     "tracks/strike_the_earth!/STE_Full.ogg"},
    {"drink_and_industry", "Drink & Industry",
     "tracks/drink_&_industry/DI_Full.ogg"},
    {"dwarf_fortress", "Dwarf Fortress (Theme)",
     "tracks/dwarf_fortress/Dwarf_Fortress.ogg"},
    {"song_game", "In-Game (Default)", "song_game.ogg"},
};

struct MusicState {
    std::string track = "hill_dwarf";
    long long start_ms = 0;
    bool manual = false;
};

std::mutex g_music_mutex;
MusicState g_music;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

const Track* find_track(const std::string& key) {
    for (const auto& track : kTracks)
        if (key == track.key) return &track;
    return nullptr;
}

bool has_ogg_suffix(const std::string& path) {
    if (path.size() < 4) return false;
    std::string suffix = path.substr(path.size() - 4);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return suffix == ".ogg";
}

bool safe_relative_sound_path(const std::string& path) {
    if (path.empty() || path.size() > 512 || path.front() == '/') return false;
    if (!has_ogg_suffix(path) || path.find("..") != std::string::npos) return false;
    for (unsigned char c : path)
        if (c < 0x20 || c == '\\' || c == ':') return false;
    return true;
}

bool path_begins_with(const fs::path& path, const fs::path& root) {
    auto path_it = path.begin();
    auto root_it = root.begin();
    for (; root_it != root.end(); ++root_it, ++path_it)
        if (path_it == path.end() || *path_it != *root_it) return false;
    return true;
}

fs::path locate_sound_root() {
    static const fs::path root = [] {
        const fs::path candidates[] = {
            fs::path("data") / "sound",
            fs::path("..") / "Dwarf Fortress" / "data" / "sound",
        };
        std::error_code ec;
        for (const auto& candidate : candidates) {
            fs::path found = fs::weakly_canonical(candidate, ec);
            if (ec) {
                ec.clear();
                continue;
            }
            if (fs::is_regular_file(found / "song_game.ogg", ec) && !ec)
                return found;
            ec.clear();
        }
        return fs::path();
    }();
    return root;
}

bool resolve_sound_file(const std::string& relative, fs::path& file) {
    if (!safe_relative_sound_path(relative)) return false;
    const fs::path root = locate_sound_root();
    if (root.empty()) return false;

    std::error_code ec;
    const fs::path candidate = fs::weakly_canonical(root / fs::path(relative), ec);
    if (ec || !path_begins_with(candidate, root) ||
        !fs::is_regular_file(candidate, ec) || ec)
        return false;
    const auto size = fs::file_size(candidate, ec);
    if (ec || size > kMaximumSoundBytes) return false;
    file = candidate;
    return true;
}

bool read_sound_file(const fs::path& path, std::string& body) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const std::streamoff length = input.tellg();
    if (length < 0 || static_cast<std::uintmax_t>(length) > kMaximumSoundBytes)
        return false;
    body.resize(static_cast<size_t>(length));
    input.seekg(0, std::ios::beg);
    if (length > 0) input.read(body.data(), length);
    return input.good() || input.eof();
}

std::string music_json(const httplib::Request& req) {
    MusicState state;
    {
        std::lock_guard<std::mutex> lock(g_music_mutex);
        if (!g_music.start_ms) g_music.start_ms = now_ms();
        state = g_music;
    }
    const long long elapsed = std::max<long long>(0, now_ms() - state.start_ms);
    std::ostringstream out;
    out << "{\"ok\":true,\"host\":"
        << (session_request_is_host(req) ? "true" : "false")
        << ",\"track\":" << json_string(state.track)
        << ",\"elapsedMs\":" << elapsed
        << ",\"manual\":" << (state.manual ? "true" : "false")
        << ",\"tracks\":[";
    bool first = true;
    for (const auto& track : kTracks) {
        fs::path file;
        if (!resolve_sound_file(track.path, file)) continue;
        if (!first) out << ",";
        first = false;
        out << "{\"key\":" << json_string(track.key)
            << ",\"label\":" << json_string(track.label)
            << ",\"path\":" << json_string(track.path) << "}";
    }
    out << "]}\n";
    return out.str();
}

std::string body_string(const httplib::Request& req, const std::string& key) {
    if (req.has_param(key.c_str())) return req.get_param_value(key.c_str());
    const std::string needle = "\"" + key + "\"";
    size_t pos = req.body.find(needle);
    if (pos == std::string::npos) return {};
    pos = req.body.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = req.body.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    const size_t end = req.body.find('"', pos + 1);
    return end == std::string::npos ? std::string() :
                                     req.body.substr(pos + 1, end - pos - 1);
}

bool body_auto(const httplib::Request& req) {
    if (req.has_param("auto")) {
        const std::string value = req.get_param_value("auto");
        return value == "1" || value == "true" || value == "on";
    }
    const size_t key = req.body.find("\"auto\"");
    if (key == std::string::npos) return false;
    const size_t colon = req.body.find(':', key + 6);
    if (colon == std::string::npos) return false;
    const size_t value = req.body.find_first_not_of(" \t\r\n", colon + 1);
    return value != std::string::npos && req.body.compare(value, 4, "true") == 0;
}

int requested_season(const httplib::Request& req) {
    if (!req.has_param("season")) return -1;
    try {
        const int season = std::stoi(req.get_param_value("season"));
        return season >= 0 && season <= 3 ? season : -1;
    } catch (...) {
        return -1;
    }
}

std::string automatic_track(int season) {
    return season == 3 ? "winter_entombs_you" : "hill_dwarf";
}

void apply_auto_context(const httplib::Request& req, bool force) {
    const int season = requested_season(req);
    const std::string selected = automatic_track(season);
    std::lock_guard<std::mutex> lock(g_music_mutex);
    if (!force && g_music.manual) return;
    if (!g_music.start_ms || g_music.track != selected || g_music.manual) {
        g_music = MusicState{selected, now_ms(), false};
    }
}

void json_error(httplib::Response& res, int status, const std::string& error) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content("{\"ok\":false,\"error\":" + json_string(error) + "}\n",
                    "application/json; charset=utf-8");
}

} // namespace

void register_audio_stream_routes(httplib::Server& server) {
    server.Get("/sound-info", [](const httplib::Request& req, httplib::Response& res) {
        const bool available = !locate_sound_root().empty();
        const bool remote = session_remote_audio_enabled();
        const bool host = session_request_is_host(req);
        res.set_header("Cache-Control", "no-store");
        res.set_content(std::string("{\"audio\":") + (available ? "true" : "false") +
                            ",\"allowed\":" + ((host || remote) ? "true" : "false") +
                            ",\"remote\":" + (remote ? "true" : "false") +
                            ",\"host\":" + (host ? "true" : "false") + "}\n",
                        "application/json; charset=utf-8");
    });

    server.Get(R"(/sound/(.+))", [](const httplib::Request& req,
                                    httplib::Response& res) {
        if (!session_request_is_host(req) && !session_remote_audio_enabled()) {
            json_error(res, 403, "remote audio disabled by host");
            return;
        }
        const std::string relative =
            req.matches.size() > 1 ? req.matches[1].str() : std::string();
        fs::path file;
        std::string body;
        if (!resolve_sound_file(relative, file) || !read_sound_file(file, body)) {
            res.status = 404;
            res.set_header("Cache-Control", "no-store");
            res.set_content("not found\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "max-age=31536000, immutable");
        res.set_content(std::move(body), "audio/ogg");
    });

    server.Get("/music", [](const httplib::Request& req, httplib::Response& res) {
        if (session_request_is_host(req)) apply_auto_context(req, false);
        res.set_header("Cache-Control", "no-store");
        res.set_content(music_json(req), "application/json; charset=utf-8");
    });

    server.Post("/music", [](const httplib::Request& req, httplib::Response& res) {
        if (!session_request_is_host(req)) {
            json_error(res, 403, "host only");
            return;
        }
        if (body_auto(req)) {
            apply_auto_context(req, true);
        } else {
            const std::string key = body_string(req, "track");
            if (!find_track(key)) {
                json_error(res, 400, "unknown track");
                return;
            }
            std::lock_guard<std::mutex> lock(g_music_mutex);
            g_music = MusicState{key, now_ms(), true};
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(music_json(req), "application/json; charset=utf-8");
    });

    diagnostics_log("audio-stream: registered authenticated Ogg and synchronized music routes");
}

} // namespace dfcapture
