// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "camera.h"
#include "frame.h"
#include "sdl_capture.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dfcapture {

constexpr int kFrameDeltaProtocol = 2;

struct FrameDeltaResult {
    std::vector<uint8_t> packet;
    CaptureGeometry geometry;
    FramePipelineTiming timing;
    uint64_t sequence = 0;
    uint64_t base_sequence = 0;
    bool keyframe = false;
    std::string keyframe_reason;
    int rectangle_count = 0;
    double changed_ratio = 0.0;
    bool has_scroll = false;
    int scroll_x = 0;
    int scroll_y = 0;
    int scroll_width = 0;
    int scroll_height = 0;
    int scroll_dx = 0;
    int scroll_dy = 0;
};

// Captures one authoritative native-rendered frame and packages either a JPEG keyframe or
// lossless PNG dirty rectangles. client_base is the last sequence the browser has applied.
bool capture_camera_delta(const std::string& player, const Camera& camera,
                          uint64_t client_base, bool force_keyframe,
                          FrameDeltaResult& result, std::string* err = nullptr);

// Releases all per-player raw-frame baselines. Called on server shutdown.
void reset_frame_delta_states();

} // namespace dfcapture
