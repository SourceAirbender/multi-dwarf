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

#include "camera.h"

#include <string>

namespace dfcapture {

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

} // namespace dfcapture
