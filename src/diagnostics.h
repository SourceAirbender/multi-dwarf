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

#pragma once

#include "camera.h"

#include <cstdint>
#include <string>

namespace dfcapture {

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

struct ViewportProbe {
    bool has_gps = false;
    bool has_viewport = false;
    bool has_renderer = false;
    Camera window;
    int gps_dim_x = 0;
    int gps_dim_y = 0;
    int tile_pixel_x = 0;
    int tile_pixel_y = 0;
    int screen_pixel_x = 0;
    int screen_pixel_y = 0;
    int viewport_zoom_factor = 0;
    int viewport_dim_x = 0;
    int viewport_dim_y = 0;
    int viewport_screen_x = 0;
    int viewport_screen_y = 0;
    int viewport_clip_x0 = 0;
    int viewport_clip_x1 = 0;
    int viewport_clip_y0 = 0;
    int viewport_clip_y1 = 0;
    uint32_t viewport_flag = 0;
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

bool viewport_probe_on_render_thread(ViewportProbe& probe, std::string* err = nullptr);
std::string viewport_probe_json(const ViewportProbe& probe);
bool grid_probe_on_render_thread(std::string& json, std::string* err = nullptr);
bool build_probe_on_render_thread(std::string& json, std::string* err = nullptr);

} // namespace dfcapture
