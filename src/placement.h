#pragma once

#include "camera.h"

#include <string>

namespace dfcapture_public {

struct DesignationRequest {
    int px = 0;
    int py = 0;
    int px2 = 0;
    int py2 = 0;
    int frame_w = 0;
    int frame_h = 0;
    std::string tool = "dig";
    int priority = 4;
    bool marker = false;
    bool warm_damp = false;
    int mine_mode = 0;
};

struct DesignationResult {
    int count = 0;
    std::string tool;
};

bool designate_on_render_thread(const Camera& camera, const DesignationRequest& request,
                                DesignationResult& result, std::string* err = nullptr);

} // namespace dfcapture_public
