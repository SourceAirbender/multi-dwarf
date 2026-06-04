#pragma once

#include "frame.h"

#include <cstdint>
#include <string>

namespace dfcapture_public {

bool unit_portrait_on_render_thread(int32_t unit_id,
                                    bool allow_icon_fallbacks,
                                    bool allow_view_sheet_generation,
                                    CapturedFrame& frame,
                                    int32_t& texpos,
                                    std::string& source,
                                    std::string* err = nullptr);

} // namespace dfcapture_public
