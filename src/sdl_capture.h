#pragma once

#include "camera.h"
#include "frame.h"

#include <string>
#include <vector>

namespace dfcapture_public {

bool read_host_camera(Camera& camera, std::string* err = nullptr);
bool clamp_camera(Camera& camera, std::string* err = nullptr);
bool capture_camera_frame(const Camera& camera, CapturedFrame& frame, std::string* err = nullptr);
bool capture_camera_jpeg(const Camera& camera, std::vector<uint8_t>& jpeg, std::string* err = nullptr);

} // namespace dfcapture_public
