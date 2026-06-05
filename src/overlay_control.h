#pragma once

#include "ColorText.h"

#include <string>

namespace dfcapture_public {

bool disable_overlay_for_stream(DFHack::color_ostream& out, std::string* note = nullptr);
void restore_overlay_after_stream(DFHack::color_ostream* out = nullptr);

} // namespace dfcapture_public
