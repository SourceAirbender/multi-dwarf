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
std::vector<ClientCamera> client_camera_snapshot();

} // namespace dfcapture_public
