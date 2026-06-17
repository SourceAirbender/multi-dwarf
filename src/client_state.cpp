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

#include "client_state.h"

#include "sdl_capture.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace dfcapture {
namespace {

std::mutex g_client_mutex;
std::unordered_map<std::string, Camera> g_player_cameras;

} // namespace

bool camera_for_player(const std::string& player, Camera& camera, std::string* err) {
    {
        std::lock_guard<std::mutex> lock(g_client_mutex);
        auto it = g_player_cameras.find(player);
        if (it != g_player_cameras.end()) {
            camera = it->second;
            return true;
        }
    }

    if (!read_host_camera(camera, err))
        return false;
    clamp_camera(camera, nullptr);

    {
        std::lock_guard<std::mutex> lock(g_client_mutex);
        g_player_cameras[player] = camera;
    }
    return true;
}

void set_player_camera(const std::string& player, const Camera& camera) {
    std::lock_guard<std::mutex> lock(g_client_mutex);
    g_player_cameras[player] = camera;
}

void forget_player_camera(const std::string& player) {
    std::lock_guard<std::mutex> lock(g_client_mutex);
    g_player_cameras.erase(player);
}

bool zoom_player_camera(const std::string& player, const std::string& direction,
                        Camera& camera, std::string* err) {
    Camera current;
    if (!camera_for_player(player, current, err))
        return false;

    std::lock_guard<std::mutex> lock(g_client_mutex);
    Camera& stored = g_player_cameras[player];
    if (direction == "reset") {
        stored.zoom_factor = -1;
    } else {
        int zoom = stored.zoom_factor >= 0 ? stored.zoom_factor : 100;
        if (direction == "in") {
            zoom -= 20;
        } else if (direction == "out") {
            zoom += 20;
        } else {
            if (err) *err = "bad zoom direction";
            return false;
        }
        stored.zoom_factor = std::max(40, std::min(300, zoom));
    }
    camera = stored;
    return true;
}

bool set_player_placement_mode(const std::string& player, bool active,
                               Camera& camera, std::string* err) {
    Camera current;
    if (!camera_for_player(player, current, err))
        return false;

    std::lock_guard<std::mutex> lock(g_client_mutex);
    Camera& stored = g_player_cameras[player];
    stored.placement_mode = active ? 1 : 0;
    if (!active) {
        stored.hover_px = -1;
        stored.hover_py = -1;
        stored.drag_active = 0;
        stored.drag_px = -1;
        stored.drag_py = -1;
    }
    camera = stored;
    return true;
}

bool set_player_placement_cursor(const std::string& player, int hx, int hy,
                                 int frame_w, int frame_h, bool dragging,
                                 int drag_x, int drag_y, int build_w, int build_h,
                                 Camera& camera, std::string* err) {
    Camera current;
    if (!camera_for_player(player, current, err))
        return false;

    std::lock_guard<std::mutex> lock(g_client_mutex);
    Camera& stored = g_player_cameras[player];
    stored.hover_px = hx;
    stored.hover_py = hy;
    stored.ui_frame_w = std::max(0, frame_w);
    stored.ui_frame_h = std::max(0, frame_h);
    stored.drag_active = dragging ? 1 : 0;
    stored.drag_px = drag_x;
    stored.drag_py = drag_y;
    stored.build_w = std::max(0, build_w);
    stored.build_h = std::max(0, build_h);
    camera = stored;
    return true;
}

std::vector<ClientCamera> client_camera_snapshot() {
    std::vector<ClientCamera> out;
    std::lock_guard<std::mutex> lock(g_client_mutex);
    out.reserve(g_player_cameras.size());
    for (const auto& entry : g_player_cameras)
        out.push_back(ClientCamera{entry.first, entry.second});
    return out;
}

} // namespace dfcapture
