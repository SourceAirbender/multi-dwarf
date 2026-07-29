// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
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

#include "diagnostics.h"

#include "json_util.h"
#include "render_thread_wait.h"
#include "save_barrier.h"
#include "sdl_capture.h"
#include "modules/DFSDL.h"
#include "modules/Gui.h"

#include "df/buildreq.h"
#include "df/enabler.h"
#include "df/gamest.h"
#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/main_interface.h"
#include "df/viewscreen.h"
#include "df/world.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <fstream>
#include <future>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>
#include <unordered_map>

namespace dfcapture {
namespace {

std::mutex g_diag_mutex;
CaptureDiagnostics g_diag;

struct ServerFrameMetric {
    double render_wait_ms = 0;
    double capture_ms = 0;
    double encode_ms = 0;
    double total_ms = 0;
    uint64_t bytes = 0;
    int width = 0;
    int height = 0;
    std::string transport = "jpeg";
    bool keyframe = true;
    std::string keyframe_reason;
    bool motion_compensated = false;
    int rectangles = 1;
    double changed_ratio = 1.0;
    long long at_ms = 0;
};

struct ClientFrameMetric {
    double fetch_ms = 0;
    double blob_ms = 0;
    double decode_ms = 0;
    double paint_ms = 0;
    double total_ms = 0;
    double input_visible_ms = -1;
    uint64_t bytes = 0;
    long long at_ms = 0;
};

template <typename T>
struct PlayerMetricWindow {
    std::deque<T> samples;
    long long last_ms = 0;
};

std::unordered_map<std::string, PlayerMetricWindow<ServerFrameMetric>> g_server_frames;
std::unordered_map<std::string, PlayerMetricWindow<ClientFrameMetric>> g_client_frames;
constexpr size_t kMaxFrameMetricSamples = 120;
constexpr size_t kMaxMetricPlayers = 32;

long long wall_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

template <typename Map>
void prune_metric_players(Map& values) {
    while (values.size() > kMaxMetricPlayers) {
        auto oldest = std::min_element(values.begin(), values.end(),
            [](const auto& a, const auto& b) { return a.second.last_ms < b.second.last_ms; });
        if (oldest == values.end()) break;
        values.erase(oldest);
    }
}

struct MetricSummary {
    double avg = 0;
    double p50 = 0;
    double p95 = 0;
};

MetricSummary summarize(std::vector<double> values) {
    MetricSummary out;
    if (values.empty()) return out;
    double total = 0;
    for (double value : values) total += value;
    out.avg = total / values.size();
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double p) {
        const size_t index = static_cast<size_t>(
            std::min<double>(values.size() - 1, std::ceil(p * values.size()) - 1));
        return values[index];
    };
    out.p50 = percentile(.50);
    out.p95 = percentile(.95);
    return out;
}

void append_summary(std::ostringstream& out, const char* name, const MetricSummary& value) {
    out << "\"" << name << "\":{\"avg\":" << value.avg
        << ",\"p50\":" << value.p50 << ",\"p95\":" << value.p95 << "}";
}

std::string utc_now() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm = {};
#ifdef _WIN32
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

void apply_event_time(CaptureDiagnostics& stats) {
    stats.last_event_utc = utc_now();
}

bool read_host_state(HostState& state, std::string* err) {
    state = HostState{};
    auto world = df::global::world;
    auto gps = df::global::gps;
    auto enabler = df::global::enabler;
    state.world_loaded = world != nullptr;
    state.viewscreen_ready = DFHack::Gui::getCurViewscreen(true) != nullptr;
    state.paused = df::global::pause_state && *df::global::pause_state;

    if (df::global::window_x) state.window.x = *df::global::window_x;
    if (df::global::window_y) state.window.y = *df::global::window_y;
    if (df::global::window_z) state.window.z = *df::global::window_z;

    if (world) {
        state.map_w = world->map.x_count;
        state.map_h = world->map.y_count;
        state.map_z = world->map.z_count;
        state.map_loaded = state.map_w > 0 && state.map_h > 0 && state.map_z > 0;
    }

    if (gps) {
        state.gps_w = gps->dimx;
        state.gps_h = gps->dimy;
        if (gps->main_viewport) {
            state.viewport_w = gps->main_viewport->dim_x;
            state.viewport_h = gps->main_viewport->dim_y;
        }
    }

    if (!state.world_loaded || !state.map_loaded || !gps || !enabler || !enabler->renderer) {
        if (err) *err = "DF host state is incomplete";
        return false;
    }
    return true;
}

struct HostStateRequest {
    HostState state;
    std::string err;
    std::promise<bool> done;
};

bool read_viewport_probe(ViewportProbe& probe, std::string* err) {
    probe = ViewportProbe{};
    auto gps = df::global::gps;
    auto enabler = df::global::enabler;
    probe.has_gps = gps != nullptr;
    probe.has_renderer = enabler && enabler->renderer;

    if (df::global::window_x) probe.window.x = *df::global::window_x;
    if (df::global::window_y) probe.window.y = *df::global::window_y;
    if (df::global::window_z) probe.window.z = *df::global::window_z;

    if (!gps) {
        if (err) *err = "gps unavailable";
        return false;
    }

    probe.gps_dim_x = gps->dimx;
    probe.gps_dim_y = gps->dimy;
    probe.tile_pixel_x = gps->tile_pixel_x;
    probe.tile_pixel_y = gps->tile_pixel_y;
    probe.screen_pixel_x = gps->screen_pixel_x;
    probe.screen_pixel_y = gps->screen_pixel_y;
    probe.viewport_zoom_factor = gps->viewport_zoom_factor;

    auto vp = gps->main_viewport;
    probe.has_viewport = vp != nullptr;
    if (vp) {
        probe.viewport_dim_x = vp->dim_x;
        probe.viewport_dim_y = vp->dim_y;
        probe.viewport_screen_x = vp->screen_x;
        probe.viewport_screen_y = vp->screen_y;
        probe.viewport_clip_x0 = vp->clipx[0];
        probe.viewport_clip_x1 = vp->clipx[1];
        probe.viewport_clip_y0 = vp->clipy[0];
        probe.viewport_clip_y1 = vp->clipy[1];
        probe.viewport_flag = vp->flag.whole;
    }

    if (!probe.has_viewport && err)
        *err = "main viewport unavailable";
    return probe.has_viewport;
}

struct ViewportProbeRequest {
    ViewportProbe probe;
    std::string err;
    std::promise<bool> done;
};

std::string histogram_json(int32_t* buf, int tiles) {
    if (!buf)
        return "null";

    std::map<int32_t, int> counts;
    for (int i = 0; i < tiles; ++i)
        counts[buf[i]]++;

    std::vector<std::pair<int32_t, int>> entries(counts.begin(), counts.end());
    std::sort(entries.begin(), entries.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    std::ostringstream body;
    body << "[";
    for (size_t i = 0; i < entries.size() && i < 12; ++i) {
        if (i) body << ",";
        body << "{\"texpos\":" << entries[i].first
             << ",\"count\":" << entries[i].second << "}";
    }
    body << "]";
    return body.str();
}

bool build_grid_probe_json(std::string& json, std::string* err) {
    auto gps = df::global::gps;
    auto game = df::global::game;
    if (!gps || !gps->main_viewport) {
        json = "{\"error\":\"no gps/viewport\"}";
        if (err) *err = "no gps/viewport";
        return false;
    }

    auto vp = gps->main_viewport;
    int tiles = vp->dim_x * vp->dim_y;
    if (tiles < 1 || tiles > 4000000) {
        json = "{\"error\":\"bad dims\"}";
        if (err) *err = "bad dims";
        return false;
    }

    std::ostringstream body;
    body << "{\"dim_x\":" << vp->dim_x
         << ",\"dim_y\":" << vp->dim_y
         << ",\"tiles\":" << tiles;
    if (game) {
        auto& mi = game->main_interface;
        body << ",\"main_designation_selected\":" << static_cast<int>(mi.main_designation_selected)
             << ",\"bottom_mode_selected\":" << static_cast<int>(mi.bottom_mode_selected);
    }
    body << ",\"interface_hist\":" << histogram_json(vp->screentexpos_interface, tiles)
         << ",\"designation_hist\":" << histogram_json(vp->screentexpos_designation, tiles)
         << "}";
    json = body.str();
    return true;
}

bool build_build_probe_json(std::string& json, std::string* err) {
    auto gps = df::global::gps;
    if (!gps || !gps->main_viewport) {
        json = "{\"error\":\"no gps/viewport\"}";
        if (err) *err = "no gps/viewport";
        return false;
    }

    auto vp = gps->main_viewport;
    int dimx = vp->dim_x;
    int dimy = vp->dim_y;
    int tiles = dimx * dimy;
    if (tiles < 1 || tiles > 4000000) {
        json = "{\"error\":\"bad dims\"}";
        if (err) *err = "bad dims";
        return false;
    }

    std::ostringstream body;
    auto dump_layer = [&](const char* name, int32_t* buf) {
        body << "\"" << name << "\":";
        if (!buf) {
            body << "null,";
            return;
        }
        body << "[";
        int emitted = 0;
        for (int x = 0; x < dimx && emitted < 60; ++x) {
            for (int y = 0; y < dimy && emitted < 60; ++y) {
                int32_t value = buf[x * dimy + y];
                if (value == 0)
                    continue;
                if (emitted) body << ",";
                body << "{\"x\":" << x
                     << ",\"y\":" << y
                     << ",\"t\":" << value << "}";
                ++emitted;
            }
        }
        body << "],";
    };

    body << "{\"dim_x\":" << dimx << ",\"dim_y\":" << dimy << ",";
    auto game = df::global::game;
    auto build = df::global::buildreq;
    if (game)
        body << "\"bottom_mode\":" << static_cast<int>(game->main_interface.bottom_mode_selected) << ",";
    if (build)
        body << "\"build_type\":" << static_cast<int>(build->building_type)
             << ",\"build_subtype\":" << build->building_subtype << ",";
    dump_layer("building_one", vp->screentexpos_building_one);
    dump_layer("building_two", vp->screentexpos_building_two);
    dump_layer("interface", vp->screentexpos_interface);
    dump_layer("designation", vp->screentexpos_designation);
    dump_layer("item", vp->screentexpos_item);
    dump_layer("signpost", vp->screentexpos_signpost);
    body << "\"end\":true}";
    json = body.str();
    return true;
}

struct JsonProbeRequest {
    std::string json;
    std::string err;
    std::promise<bool> done;
};

} // namespace

void diagnostics_log(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_diag_mutex);
    std::ofstream out("dfcapture.log", std::ios::app);
    if (out)
        out << utc_now() << " " << line << "\n";
}

void diagnostics_capture_attempt(const Camera& camera) {
    std::lock_guard<std::mutex> lock(g_diag_mutex);
    ++g_diag.attempts;
    g_diag.last_camera = camera;
    apply_event_time(g_diag);
}

void diagnostics_capture_success(const Camera& camera, int width, int height,
                                 uint64_t bytes, int duration_ms) {
    std::lock_guard<std::mutex> lock(g_diag_mutex);
    ++g_diag.successes;
    g_diag.last_camera = camera;
    g_diag.last_width = width;
    g_diag.last_height = height;
    g_diag.last_frame_bytes = bytes;
    g_diag.last_duration_ms = duration_ms;
    g_diag.last_error.clear();
    apply_event_time(g_diag);
}

void diagnostics_capture_failure(const Camera& camera, const std::string& err,
                                 int duration_ms) {
    {
        std::lock_guard<std::mutex> lock(g_diag_mutex);
        ++g_diag.failures;
        g_diag.last_camera = camera;
        g_diag.last_duration_ms = duration_ms;
        g_diag.last_error = err;
        apply_event_time(g_diag);
    }
    diagnostics_log("capture failed camera=" + std::to_string(camera.x) + "," +
                    std::to_string(camera.y) + "," + std::to_string(camera.z) +
                    " err=" + err);
}

void diagnostics_reset() {
    std::lock_guard<std::mutex> lock(g_diag_mutex);
    g_diag = CaptureDiagnostics{};
    g_server_frames.clear();
    g_client_frames.clear();
    apply_event_time(g_diag);
}

CaptureDiagnostics diagnostics_snapshot() {
    std::lock_guard<std::mutex> lock(g_diag_mutex);
    return g_diag;
}

std::string diagnostics_json(const std::string& player, const Camera& camera,
                             const CaptureDiagnostics& stats) {
    std::ostringstream body;
    body << "{\"ok\":true"
         << ",\"player\":" << json_string(player)
         << ",\"camera\":{\"x\":" << camera.x
         << ",\"y\":" << camera.y
         << ",\"z\":" << camera.z
         << ",\"zoom\":" << (camera.zoom_factor >= 0 ? camera.zoom_factor : 100)
         << ",\"zoomExplicit\":" << (camera.zoom_factor >= 0 ? "true" : "false") << "}"
         << ",\"capture\":{\"attempts\":" << stats.attempts
         << ",\"successes\":" << stats.successes
         << ",\"failures\":" << stats.failures
         << ",\"lastWidth\":" << stats.last_width
         << ",\"lastHeight\":" << stats.last_height
         << ",\"lastFrameBytes\":" << stats.last_frame_bytes
         << ",\"lastDurationMs\":" << stats.last_duration_ms
         << ",\"lastCamera\":{\"x\":" << stats.last_camera.x
         << ",\"y\":" << stats.last_camera.y
         << ",\"z\":" << stats.last_camera.z
         << ",\"zoom\":" << (stats.last_camera.zoom_factor >= 0 ? stats.last_camera.zoom_factor : 100)
         << ",\"zoomExplicit\":" << (stats.last_camera.zoom_factor >= 0 ? "true" : "false") << "}"
         << ",\"lastError\":" << json_string(stats.last_error)
         << ",\"lastEventUtc\":" << json_string(stats.last_event_utc)
         << "}}\n";
    return body.str();
}

void diagnostics_frame_pipeline(const std::string& player,
                                double render_wait_ms, double capture_ms,
                                double encode_ms, double total_ms,
                                uint64_t payload_bytes, int width, int height,
                                const std::string& transport, bool keyframe,
                                int rectangles, double changed_ratio,
                                const std::string& keyframe_reason,
                                bool motion_compensated) {
    if (player.empty()) return;
    ServerFrameMetric sample;
    sample.render_wait_ms = std::max(0.0, render_wait_ms);
    sample.capture_ms = std::max(0.0, capture_ms);
    sample.encode_ms = std::max(0.0, encode_ms);
    sample.total_ms = std::max(0.0, total_ms);
    sample.bytes = payload_bytes;
    sample.width = width;
    sample.height = height;
    sample.transport = transport == "delta" ? "delta" : "jpeg";
    sample.keyframe = keyframe;
    sample.keyframe_reason = keyframe_reason.substr(0, 24);
    sample.motion_compensated = motion_compensated;
    sample.rectangles = std::max(0, rectangles);
    sample.changed_ratio = std::max(0.0, std::min(1.0, changed_ratio));
    sample.at_ms = wall_now_ms();
    std::lock_guard<std::mutex> lock(g_diag_mutex);
    auto& window = g_server_frames[player.substr(0, 64)];
    window.samples.push_back(sample);
    window.last_ms = sample.at_ms;
    while (window.samples.size() > kMaxFrameMetricSamples) window.samples.pop_front();
    prune_metric_players(g_server_frames);
}

void diagnostics_frame_client(const std::string& player,
                              double fetch_ms, double blob_ms, double decode_ms,
                              double paint_ms, double total_ms, double input_visible_ms,
                              uint64_t payload_bytes) {
    if (player.empty()) return;
    ClientFrameMetric sample;
    sample.fetch_ms = std::max(0.0, fetch_ms);
    sample.blob_ms = std::max(0.0, blob_ms);
    sample.decode_ms = std::max(0.0, decode_ms);
    sample.paint_ms = std::max(0.0, paint_ms);
    sample.total_ms = std::max(0.0, total_ms);
    sample.input_visible_ms = input_visible_ms >= 0 ? input_visible_ms : -1;
    sample.bytes = payload_bytes;
    sample.at_ms = wall_now_ms();
    std::lock_guard<std::mutex> lock(g_diag_mutex);
    auto& window = g_client_frames[player.substr(0, 64)];
    window.samples.push_back(sample);
    window.last_ms = sample.at_ms;
    while (window.samples.size() > kMaxFrameMetricSamples) window.samples.pop_front();
    prune_metric_players(g_client_frames);
}

std::string frame_pipeline_diagnostics_json(const std::string& player) {
    std::deque<ServerFrameMetric> server;
    std::deque<ClientFrameMetric> client;
    {
        std::lock_guard<std::mutex> lock(g_diag_mutex);
        if (auto it = g_server_frames.find(player); it != g_server_frames.end())
            server = it->second.samples;
        if (auto it = g_client_frames.find(player); it != g_client_frames.end())
            client = it->second.samples;
    }
    const auto server_values = [&](auto member) {
        std::vector<double> values;
        values.reserve(server.size());
        for (const auto& sample : server) values.push_back(sample.*member);
        return summarize(std::move(values));
    };
    const auto client_values = [&](auto member, bool skip_negative = false) {
        std::vector<double> values;
        values.reserve(client.size());
        for (const auto& sample : client) {
            const double value = sample.*member;
            if (!skip_negative || value >= 0) values.push_back(value);
        }
        return summarize(std::move(values));
    };
    uint64_t server_bytes = 0;
    uint64_t client_bytes = 0;
    uint64_t jpeg_bytes = 0;
    uint64_t delta_bytes = 0;
    size_t jpeg_samples = 0;
    size_t delta_samples = 0;
    size_t keyframes = 0;
    size_t empty_deltas = 0;
    size_t motion_frames = 0;
    size_t forced_keyframes = 0;
    size_t periodic_keyframes = 0;
    size_t resync_keyframes = 0;
    size_t change_fallback_keyframes = 0;
    for (const auto& sample : server) server_bytes += sample.bytes;
    for (const auto& sample : server) {
        if (sample.transport == "delta") {
            ++delta_samples;
            delta_bytes += sample.bytes;
            if (!sample.keyframe && sample.rectangles == 0) ++empty_deltas;
        } else {
            ++jpeg_samples;
            jpeg_bytes += sample.bytes;
        }
        if (sample.keyframe) ++keyframes;
        if (sample.motion_compensated) ++motion_frames;
        if (sample.keyframe_reason == "forced") ++forced_keyframes;
        if (sample.keyframe_reason == "periodic") ++periodic_keyframes;
        if (sample.keyframe_reason == "resync") ++resync_keyframes;
        if (sample.keyframe_reason == "change-fallback") ++change_fallback_keyframes;
    }
    for (const auto& sample : client) client_bytes += sample.bytes;
    const auto elapsed_seconds = [](const auto& samples) {
        if (samples.size() < 2 || samples.back().at_ms <= samples.front().at_ms)
            return 0.0;
        return static_cast<double>(samples.back().at_ms - samples.front().at_ms) / 1000.0;
    };
    const double server_seconds = elapsed_seconds(server);
    const double client_seconds = elapsed_seconds(client);
    const double server_fps =
        server_seconds > 0 ? static_cast<double>(server.size() - 1) / server_seconds : 0.0;
    const double client_fps =
        client_seconds > 0 ? static_cast<double>(client.size() - 1) / client_seconds : 0.0;
    const auto average_bytes = [](uint64_t bytes, size_t samples) {
        return samples ? static_cast<double>(bytes) / samples : 0.0;
    };

    std::ostringstream out;
    out << "{\"ok\":true,\"player\":" << json_string(player)
        << ",\"windowLimit\":" << kMaxFrameMetricSamples
        << ",\"server\":{\"samples\":" << server.size()
        << ",\"payloadBytes\":" << server_bytes << ",";
    append_summary(out, "renderWaitMs", server_values(&ServerFrameMetric::render_wait_ms));
    out << ",";
    append_summary(out, "captureMs", server_values(&ServerFrameMetric::capture_ms));
    out << ",";
    append_summary(out, "encodeMs", server_values(&ServerFrameMetric::encode_ms));
    out << ",";
    append_summary(out, "totalMs", server_values(&ServerFrameMetric::total_ms));
    out << ",";
    append_summary(out, "rectangles", server_values(&ServerFrameMetric::rectangles));
    out << ",";
    append_summary(out, "changedRatio", server_values(&ServerFrameMetric::changed_ratio));
    out << ",\"cadence\":{\"observedFps\":" << server_fps
        << ",\"bytesPerSecond\":"
        << (server_seconds > 0 ? server_bytes / server_seconds : 0.0)
        << ",\"averageBytes\":" << average_bytes(server_bytes, server.size()) << "}";
    out << ",\"transport\":{\"jpegSamples\":" << jpeg_samples
        << ",\"jpegBytes\":" << jpeg_bytes
        << ",\"deltaSamples\":" << delta_samples
        << ",\"deltaBytes\":" << delta_bytes
        << ",\"keyframes\":" << keyframes
        << ",\"keyframeRate\":"
        << (server.empty() ? 0.0 : static_cast<double>(keyframes) / server.size())
        << ",\"emptyDeltas\":" << empty_deltas
        << ",\"motionFrames\":" << motion_frames
        << ",\"keyframeReasons\":{\"forced\":" << forced_keyframes
        << ",\"periodic\":" << periodic_keyframes
        << ",\"resync\":" << resync_keyframes
        << ",\"changeFallback\":" << change_fallback_keyframes << "}}";
    if (!server.empty()) {
        out << ",\"last\":{\"width\":" << server.back().width
            << ",\"height\":" << server.back().height
            << ",\"bytes\":" << server.back().bytes
            << ",\"transport\":" << json_string(server.back().transport)
            << ",\"keyframe\":" << (server.back().keyframe ? "true" : "false")
            << ",\"keyframeReason\":" << json_string(server.back().keyframe_reason)
            << ",\"motionCompensated\":"
            << (server.back().motion_compensated ? "true" : "false")
            << ",\"rectangles\":" << server.back().rectangles
            << ",\"changedRatio\":" << server.back().changed_ratio
            << ",\"at\":" << server.back().at_ms << "}";
    } else {
        out << ",\"last\":null";
    }
    out << "},\"client\":{\"samples\":" << client.size()
        << ",\"payloadBytes\":" << client_bytes << ",";
    append_summary(out, "fetchMs", client_values(&ClientFrameMetric::fetch_ms));
    out << ",";
    append_summary(out, "blobMs", client_values(&ClientFrameMetric::blob_ms));
    out << ",";
    append_summary(out, "decodeMs", client_values(&ClientFrameMetric::decode_ms));
    out << ",";
    append_summary(out, "paintMs", client_values(&ClientFrameMetric::paint_ms));
    out << ",";
    append_summary(out, "totalMs", client_values(&ClientFrameMetric::total_ms));
    out << ",";
    append_summary(out, "inputVisibleMs",
                   client_values(&ClientFrameMetric::input_visible_ms, true));
    out << ",\"cadence\":{\"observedFps\":" << client_fps
        << ",\"bytesPerSecond\":"
        << (client_seconds > 0 ? client_bytes / client_seconds : 0.0)
        << ",\"averageBytes\":" << average_bytes(client_bytes, client.size()) << "}";
    out << "}}\n";
    return out.str();
}

bool host_state_on_render_thread(HostState& state, std::string* err) {
    auto request = std::make_shared<HostStateRequest>();
    auto future = request->done.get_future();
    DFHack::runOnRenderThread([request]() {
        if (save_barrier_active()) {
            request->err = "Dwarf Fortress is saving or unloading";
            request->done.set_value(false);
            return;
        }
        request->done.set_value(read_host_state(request->state, &request->err));
    });

    bool ok = false;
    if (!render_future_get(future, ok)) {
        if (err) *err = "host-state render-thread request timed out or was abandoned";
        return false;
    }
    state = request->state;
    if (!ok && err)
        *err = request->err;
    return ok;
}

std::string host_state_json(const HostState& state) {
    std::ostringstream body;
    body << "{\"ok\":true"
         << ",\"worldLoaded\":" << (state.world_loaded ? "true" : "false")
         << ",\"mapLoaded\":" << (state.map_loaded ? "true" : "false")
         << ",\"viewscreenReady\":" << (state.viewscreen_ready ? "true" : "false")
         << ",\"paused\":" << (state.paused ? "true" : "false")
         << ",\"window\":{\"x\":" << state.window.x
         << ",\"y\":" << state.window.y
         << ",\"z\":" << state.window.z << "}"
         << ",\"map\":{\"w\":" << state.map_w
         << ",\"h\":" << state.map_h
         << ",\"z\":" << state.map_z << "}"
         << ",\"gps\":{\"w\":" << state.gps_w
         << ",\"h\":" << state.gps_h << "}"
         << ",\"viewport\":{\"w\":" << state.viewport_w
         << ",\"h\":" << state.viewport_h << "}}\n";
    return body.str();
}

bool viewport_probe_on_render_thread(ViewportProbe& probe, std::string* err) {
    auto request = std::make_shared<ViewportProbeRequest>();
    auto future = request->done.get_future();
    DFHack::runOnRenderThread([request]() {
        if (save_barrier_active()) {
            request->err = "Dwarf Fortress is saving or unloading";
            request->done.set_value(false);
            return;
        }
        request->done.set_value(read_viewport_probe(request->probe, &request->err));
    });

    bool ok = false;
    if (!render_future_get(future, ok)) {
        if (err) *err = "viewport-probe render-thread request timed out or was abandoned";
        return false;
    }
    probe = request->probe;
    if (!ok && err)
        *err = request->err;
    return ok;
}

std::string viewport_probe_json(const ViewportProbe& probe) {
    std::ostringstream body;
    body << "{\"ok\":true"
         << ",\"hasGps\":" << (probe.has_gps ? "true" : "false")
         << ",\"hasViewport\":" << (probe.has_viewport ? "true" : "false")
         << ",\"hasRenderer\":" << (probe.has_renderer ? "true" : "false")
         << ",\"window\":{\"x\":" << probe.window.x
         << ",\"y\":" << probe.window.y
         << ",\"z\":" << probe.window.z << "}"
         << ",\"gps\":{\"dimX\":" << probe.gps_dim_x
         << ",\"dimY\":" << probe.gps_dim_y
         << ",\"tilePixelX\":" << probe.tile_pixel_x
         << ",\"tilePixelY\":" << probe.tile_pixel_y
         << ",\"screenPixelX\":" << probe.screen_pixel_x
         << ",\"screenPixelY\":" << probe.screen_pixel_y
         << ",\"viewportZoomFactor\":" << probe.viewport_zoom_factor << "}"
         << ",\"viewport\":{\"dimX\":" << probe.viewport_dim_x
         << ",\"dimY\":" << probe.viewport_dim_y
         << ",\"screenX\":" << probe.viewport_screen_x
         << ",\"screenY\":" << probe.viewport_screen_y
         << ",\"clipX0\":" << probe.viewport_clip_x0
         << ",\"clipX1\":" << probe.viewport_clip_x1
         << ",\"clipY0\":" << probe.viewport_clip_y0
         << ",\"clipY1\":" << probe.viewport_clip_y1
         << ",\"flag\":" << probe.viewport_flag << "}}\n";
    return body.str();
}

bool grid_probe_on_render_thread(std::string& json, std::string* err) {
    std::lock_guard<std::recursive_mutex> lock(capture_state_mutex());
    auto request = std::make_shared<JsonProbeRequest>();
    auto future = request->done.get_future();
    DFHack::runOnRenderThread([request]() {
        if (save_barrier_active()) {
            request->err = "Dwarf Fortress is saving or unloading";
            request->done.set_value(false);
            return;
        }
        request->done.set_value(build_grid_probe_json(request->json, &request->err));
    });

    bool ok = false;
    if (!render_future_get(future, ok)) {
        if (err) *err = "grid-probe render-thread request timed out or was abandoned";
        return false;
    }
    json = request->json;
    diagnostics_log("GRID-PROBE " + json);
    if (!ok && err)
        *err = request->err;
    return ok;
}

bool build_probe_on_render_thread(std::string& json, std::string* err) {
    std::lock_guard<std::recursive_mutex> lock(capture_state_mutex());
    auto request = std::make_shared<JsonProbeRequest>();
    auto future = request->done.get_future();
    DFHack::runOnRenderThread([request]() {
        if (save_barrier_active()) {
            request->err = "Dwarf Fortress is saving or unloading";
            request->done.set_value(false);
            return;
        }
        request->done.set_value(build_build_probe_json(request->json, &request->err));
    });

    bool ok = false;
    if (!render_future_get(future, ok)) {
        if (err) *err = "build-probe render-thread request timed out or was abandoned";
        return false;
    }
    json = request->json;
    diagnostics_log("BUILD-PROBE " + json);
    if (!ok && err)
        *err = request->err;
    return ok;
}

} // namespace dfcapture
