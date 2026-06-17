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

#include <string>
#include <vector>

namespace dfcapture {

struct ClientCamera {
    std::string player;
    Camera camera;
};

bool camera_for_player(const std::string& player, Camera& camera, std::string* err = nullptr);
void set_player_camera(const std::string& player, const Camera& camera);
void forget_player_camera(const std::string& player);
bool zoom_player_camera(const std::string& player, const std::string& direction,
                        Camera& camera, std::string* err = nullptr);
bool set_player_placement_mode(const std::string& player, bool active,
                               Camera& camera, std::string* err = nullptr);
bool set_player_placement_cursor(const std::string& player, int hx, int hy,
                                 int frame_w, int frame_h, bool dragging,
                                 int drag_x, int drag_y, int build_w, int build_h,
                                 Camera& camera, std::string* err = nullptr);
std::vector<ClientCamera> client_camera_snapshot();

} // namespace dfcapture
