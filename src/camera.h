#pragma once

namespace dfcapture_public {

struct Camera {
    int x = 0;
    int y = 0;
    int z = 0;
    int zoom_factor = -1; // -1 inherits the fixed reference zoom; otherwise percent scale.

    int placement_mode = 0;
    int hover_px = -1;
    int hover_py = -1;
    int ui_frame_w = 0;
    int ui_frame_h = 0;
    int drag_active = 0;
    int drag_px = -1;
    int drag_py = -1;
    int build_w = 0;
    int build_h = 0;
};

} // namespace dfcapture_public
