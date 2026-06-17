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

namespace dfcapture {

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

} // namespace dfcapture
