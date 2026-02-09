#pragma once

#include <cstdint>
#include <vector>

namespace dfcapture_public {

struct CapturedFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;
};

} // namespace dfcapture_public
