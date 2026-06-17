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
#include "frame.h"

#include <mutex>
#include <string>
#include <vector>

namespace dfcapture {

bool read_host_camera(Camera& camera, std::string* err = nullptr);
bool clamp_camera(Camera& camera, std::string* err = nullptr);
bool effective_capture_viewport_dims(const Camera& camera, int& width_tiles,
                                     int& height_tiles, std::string* err = nullptr);
bool capture_camera_frame(const Camera& camera, CapturedFrame& frame, std::string* err = nullptr);
bool capture_camera_jpeg(const Camera& camera, std::vector<uint8_t>& jpeg, std::string* err = nullptr);
std::recursive_mutex& capture_state_mutex();

} // namespace dfcapture
