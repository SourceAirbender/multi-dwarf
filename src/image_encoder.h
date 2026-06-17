// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, version 3 of the License.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// Runs on DFHack (Zlib); descends from DFPlex (Zlib) and webfort (ISC).
// Full license: see LICENSE. Third-party credits: see NOTICE.
//
// SPDX-License-Identifier: AGPL-3.0-only

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
