#pragma once

#include "frame.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dfcapture {

constexpr int DEFAULT_JPEG_QUALITY = 82;

bool write_bmp(const std::string& path, const CapturedFrame& frame, std::string* err = nullptr);
bool encode_jpeg(const CapturedFrame& frame, std::vector<uint8_t>& jpeg,
                 int quality = DEFAULT_JPEG_QUALITY, std::string* err = nullptr);
bool encode_png(const CapturedFrame& frame, std::vector<uint8_t>& png, std::string* err = nullptr);
void shutdown_image_encoder();

} // namespace dfcapture
