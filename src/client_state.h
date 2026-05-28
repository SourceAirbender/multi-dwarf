#pragma once

#include "camera.h"

#include <string>
#include <vector>

namespace dfcapture_public {

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

} // namespace dfcapture_public
