#pragma once

#include "camera.h"

#include <cstdint>
#include <string>

namespace dfcapture_public {

struct CaptureDiagnostics {
    uint64_t attempts = 0;
    uint64_t successes = 0;
    uint64_t failures = 0;
    uint64_t last_frame_bytes = 0;
    int last_width = 0;
    int last_height = 0;
    int last_duration_ms = 0;
    Camera last_camera;
    std::string last_error;
    std::string last_event_utc;
};

struct HostState {
    bool world_loaded = false;
    bool map_loaded = false;
    bool viewscreen_ready = false;
    bool paused = false;
    Camera window;
    int map_w = 0;
    int map_h = 0;
    int map_z = 0;
    int gps_w = 0;
    int gps_h = 0;
    int viewport_w = 0;
    int viewport_h = 0;
};

void diagnostics_log(const std::string& line);
void diagnostics_capture_attempt(const Camera& camera);
void diagnostics_capture_success(const Camera& camera, int width, int height,
                                 uint64_t bytes, int duration_ms);
void diagnostics_capture_failure(const Camera& camera, const std::string& err,
                                 int duration_ms);
void diagnostics_reset();

CaptureDiagnostics diagnostics_snapshot();
std::string diagnostics_json(const std::string& player, const Camera& camera,
                             const CaptureDiagnostics& stats);

bool host_state_on_render_thread(HostState& state, std::string* err = nullptr);
std::string host_state_json(const HostState& state);

} // namespace dfcapture_public
