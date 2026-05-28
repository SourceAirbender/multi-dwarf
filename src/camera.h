#pragma once

namespace dfcapture_public {

struct Camera {
    int x = 0;
    int y = 0;
    int z = 0;
    int zoom_factor = -1; // -1 inherits the fixed reference zoom; otherwise percent scale.
};

} // namespace dfcapture_public
