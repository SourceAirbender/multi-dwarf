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

#pragma once

#include "camera.h"
#include "frame.h"

#include <mutex>
#include <string>
#include <vector>

namespace dfcapture {

struct FramePipelineTiming {
    double capture_queue_ms = 0.0;
    double render_wait_ms = 0.0;
    double capture_ms = 0.0;
    double target_setup_ms = 0.0;
    double viewport_draw_ms = 0.0;
    double readback_ms = 0.0;
    double host_restore_ms = 0.0;
    double encode_ms = 0.0;
    double total_ms = 0.0;
    int width = 0;
    int height = 0;
    int lower_viewports = 0;
    int auxiliary_renders = 0;
    bool host_paused = false;
    bool reused = false;
};

bool read_host_camera(Camera& camera, std::string* err = nullptr);
bool clamp_camera(Camera& camera, std::string* err = nullptr);
bool effective_capture_viewport_dims(const Camera& camera, int& width_tiles,
                                     int& height_tiles, std::string* err = nullptr);
bool capture_camera_frame(const Camera& camera, CapturedFrame& frame, std::string* err = nullptr);
bool capture_camera_frame_timed(const Camera& camera, CapturedFrame& frame,
                                std::string* err = nullptr,
                                FramePipelineTiming* timing = nullptr);
bool capture_camera_jpeg(const Camera& camera, std::vector<uint8_t>& jpeg,
                         CaptureGeometry* geometry = nullptr, std::string* err = nullptr,
                         FramePipelineTiming* timing = nullptr);
std::recursive_mutex& capture_state_mutex();

} // namespace dfcapture
