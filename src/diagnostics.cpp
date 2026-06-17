// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios
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
#include <ctime>
#include <fstream>
#include <future>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

namespace dfcapture {
namespace {

std::mutex g_diag_mutex;
CaptureDiagnostics g_diag;

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

bool host_state_on_render_thread(HostState& state, std::string* err) {
    auto request = std::make_shared<HostStateRequest>();
    auto future = request->done.get_future();
    DFHack::runOnRenderThread([request]() {
        request->done.set_value(read_host_state(request->state, &request->err));
    });

    bool ok = future.get();
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
        request->done.set_value(read_viewport_probe(request->probe, &request->err));
    });

    bool ok = future.get();
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
        request->done.set_value(build_grid_probe_json(request->json, &request->err));
    });

    bool ok = future.get();
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
        request->done.set_value(build_build_probe_json(request->json, &request->err));
    });

    bool ok = future.get();
    json = request->json;
    diagnostics_log("BUILD-PROBE " + json);
    if (!ok && err)
        *err = request->err;
    return ok;
}

} // namespace dfcapture
