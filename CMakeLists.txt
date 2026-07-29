# dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
# Copyright (C) 2025 - 2026 Gabriel Rios <grios019@gmail.com>
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

# Reconfigure when the checked-out commit changes so the DLL cannot retain the previous commit id
# after a normal `git commit` followed by `cmake --build`.
# DFHack commonly loads external plugins through a directory junction. Git resolves repository
# ancestry from the junction path (and finds DFHack's repository) unless we canonicalize back to
# this plugin checkout first.
file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}" DFCAPTURE_GIT_WORKING_DIR)
# CMake's REAL_PATH does not dereference Windows directory junctions. The standard DFHack external
# layout puts the real checkout beside dfhack-src, so recognize that layout without hard-coding the
# checkout name or workspace.
get_filename_component(DFCAPTURE_SOURCE_NAME "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
get_filename_component(DFCAPTURE_WORKSPACE_CANDIDATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(DFCAPTURE_SIBLING_CANDIDATE
    "${DFCAPTURE_WORKSPACE_CANDIDATE}/${DFCAPTURE_SOURCE_NAME}")
if(EXISTS "${DFCAPTURE_SIBLING_CANDIDATE}/.git")
    set(DFCAPTURE_GIT_WORKING_DIR "${DFCAPTURE_SIBLING_CANDIDATE}")
endif()
execute_process(
    COMMAND git rev-parse --absolute-git-dir
    WORKING_DIRECTORY "${DFCAPTURE_GIT_WORKING_DIR}"
    OUTPUT_VARIABLE DFCAPTURE_GIT_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
execute_process(
    COMMAND git symbolic-ref -q HEAD
    WORKING_DIRECTORY "${DFCAPTURE_GIT_WORKING_DIR}"
    OUTPUT_VARIABLE DFCAPTURE_GIT_HEAD_REF
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(DFCAPTURE_GIT_DIR)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${DFCAPTURE_GIT_DIR}/HEAD")
    if(DFCAPTURE_GIT_HEAD_REF)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${DFCAPTURE_GIT_DIR}/${DFCAPTURE_GIT_HEAD_REF}")
    endif()
endif()

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${DFCAPTURE_GIT_WORKING_DIR}"
    OUTPUT_VARIABLE DFCAPTURE_GIT_REV
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT DFCAPTURE_GIT_REV)
    set(DFCAPTURE_GIT_REV "unknown")
else()
    execute_process(
        COMMAND git status --porcelain
        WORKING_DIRECTORY "${DFCAPTURE_GIT_WORKING_DIR}"
        OUTPUT_VARIABLE DFCAPTURE_GIT_DIRTY
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(DFCAPTURE_GIT_DIRTY)
        string(APPEND DFCAPTURE_GIT_REV "-dirty")
    endif()
endif()

dfhack_plugin(dfcapture_public
    src/announcements.cpp
    src/attribution.cpp
    src/art_desc.cpp
    src/audio_stream.cpp
    src/build_identity.cpp
    src/building_zone.cpp
    src/burrows_panel.cpp
    src/chat.cpp
    src/curses_palette.cpp
    src/client_state.cpp
    src/console_routes.cpp
    src/dfcapture.cpp
    src/diagnostics.cpp
    src/diplo.cpp
    src/fort_admin.cpp
    src/fortress_utilities.cpp
    src/frame_delta.cpp
    src/hauling.cpp
    src/hud.cpp
    src/http_server.cpp
    src/info_panel.cpp
    src/interaction.cpp
    src/image_encoder.cpp
    src/json_util.cpp
    src/labor.cpp
    src/lever_link.cpp
    src/lua_bridge.cpp
    src/missions.cpp
    src/notifications.cpp
    src/native_popup.cpp
    src/native_trade_5315.cpp
    src/overlay_control.cpp
    src/placement.cpp
    src/player_ownership.cpp
    src/save_barrier.cpp
    src/session_policy.cpp
    src/sdl_capture.cpp
    src/squads.cpp
    src/standing_orders.cpp
    src/stockpile_panel.cpp
    src/stone_use.cpp
    src/unit_portrait.cpp
    src/ui_cache_purge.cpp
    src/vote.cpp
    src/unit_sheet.cpp
    src/web_assets.cpp
    src/work_orders.cpp
    src/worldmap_panel.cpp
    src/write_guards.cpp
    src/hospital.cpp
    src/kitchen_panel.cpp
    src/trade_depot.cpp
    src/unit_activity.cpp
    LINK_LIBRARIES bcrypt gdiplus ole32 ws2_32 lua jsoncpp_static)

target_include_directories(dfcapture_public PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib")

target_compile_definitions(dfcapture_public PRIVATE
    DFCAPTURE_GIT_REV="${DFCAPTURE_GIT_REV}")

set_target_properties(dfcapture_public PROPERTIES OUTPUT_NAME dfcapture)

if(INSTALL_DATA_FILES)
    install(FILES dfcapture.lua
        DESTINATION ${DFHACK_LUA_DESTINATION}/plugins)
    install(FILES scripts/gui/dfcapture.lua
        DESTINATION ${DFHACK_DATA_DESTINATION}/scripts/gui)
endif()
