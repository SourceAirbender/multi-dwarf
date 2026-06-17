# dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
# Copyright (C) 2026 Gabriel Rios
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, version 3 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# Runs on DFHack (Zlib); descends from DFPlex (Zlib) and webfort (ISC).
# Full license: see LICENSE. Third-party credits: see NOTICE.
#
# SPDX-License-Identifier: AGPL-3.0-only

project(dfcapture)

dfhack_plugin(dfcapture_public
    src/building_zone.cpp
    src/client_state.cpp
    src/dfcapture.cpp
    src/diagnostics.cpp
    src/hud.cpp
    src/http_server.cpp
    src/info_panel.cpp
    src/interaction.cpp
    src/image_encoder.cpp
    src/json_util.cpp
    src/labor.cpp
    src/lua_bridge.cpp
    src/notifications.cpp
    src/overlay_control.cpp
    src/placement.cpp
    src/sdl_capture.cpp
    src/stockpile_panel.cpp
    src/unit_portrait.cpp
    src/unit_sheet.cpp
    src/web_assets.cpp
    src/work_orders.cpp
    LINK_LIBRARIES gdiplus ole32 ws2_32 lua)

target_include_directories(dfcapture_public PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../dfplex-src/server/cpp-httplib")

set_target_properties(dfcapture_public PROPERTIES OUTPUT_NAME dfcapture)

if(INSTALL_DATA_FILES)
    install(FILES dfcapture.lua
        DESTINATION ${DFHACK_LUA_DESTINATION}/plugins)
endif()
