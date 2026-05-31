#pragma once

#include "camera.h"
#include "frame.h"

#include <mutex>
#include <string>
#include <vector>

namespace dfcapture_public {

bool read_host_camera(Camera& camera, std::string* err = nullptr);
bool clamp_camera(Camera& camera, std::string* err = nullptr);
bool effective_capture_viewport_dims(const Camera& camera, int& width_tiles,
                                     int& height_tiles, std::string* err = nullptr);
bool capture_camera_frame(const Camera& camera, CapturedFrame& frame, std::string* err = nullptr);
bool capture_camera_jpeg(const Camera& camera, std::vector<uint8_t>& jpeg, std::string* err = nullptr);
std::recursive_mutex& capture_state_mutex();

} // namespace dfcapture_public
