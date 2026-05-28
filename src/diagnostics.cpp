#include "diagnostics.h"

#include "json_util.h"
#include "modules/DFSDL.h"
#include "modules/Gui.h"

#include "df/enabler.h"
#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/viewscreen.h"
#include "df/world.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <future>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>

namespace dfcapture_public {
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

} // namespace

void diagnostics_log(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_diag_mutex);
    std::ofstream out("dfcapture_public.log", std::ios::app);
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

} // namespace dfcapture_public
