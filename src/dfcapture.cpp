// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2025 - 2026 Gabriel Rios
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

#include "Core.h"
#include "Export.h"
#include "PluginManager.h"
#include "modules/DFSDL.h"

#include "diagnostics.h"
#include "http_server.h"
#include "image_encoder.h"
#include "overlay_control.h"
#include "sdl_capture.h"
#include "web_assets.h"

#include "df/global_objects.h"

#include <cstdlib>
#include <string>
#include <vector>

using namespace DFHack;

DFHACK_PLUGIN("dfcapture");

namespace {

bool parse_port(const std::string& text, int& port) {
    char* end = nullptr;
    long value = std::strtol(text.c_str(), &end, 10);
    if (!end || *end != '\0' || value < 1 || value > 65535)
        return false;
    port = static_cast<int>(value);
    return true;
}

void print_line(color_ostream& out, const std::string& text) {
    out.print("%s", text.c_str());
}

void write_debug_capture(dfcapture::Camera camera, const char* path) {
    dfcapture::CapturedFrame frame;
    std::string err;
    if (!dfcapture::capture_camera_frame(camera, frame, &err)) {
        dfcapture::diagnostics_log(std::string("debug capture failed: ") + err);
        return;
    }
    if (!dfcapture::write_bmp(path, frame, &err))
        dfcapture::diagnostics_log(std::string("debug BMP write failed: ") + err);
}

void write_current_debug_capture() {
    if (!df::global::window_x || !df::global::window_y || !df::global::window_z) {
        dfcapture::diagnostics_log("debug capture failed: DF window coordinates are unavailable");
        return;
    }
    dfcapture::Camera camera;
    camera.x = *df::global::window_x;
    camera.y = *df::global::window_y;
    camera.z = *df::global::window_z;
    write_debug_capture(camera, "dfcapture_test.bmp");
}

command_result cmd_capture(color_ostream& out, std::vector<std::string>&) {
    dfcapture::diagnostics_log("--- capture requested ---");
    DFHack::runOnRenderThread([]() { write_current_debug_capture(); });
    out.print("dfcapture: queued; check dfcapture_test.bmp / dfcapture.log in the DF folder.\n");
    return CR_OK;
}

command_result cmd_capture_at(color_ostream& out, std::vector<std::string>& args) {
    if (args.size() < 3) {
        out.printerr("usage: capture-at <x> <y> <z>\n");
        return CR_WRONG_USAGE;
    }
    dfcapture::Camera camera;
    camera.x = std::atoi(args[0].c_str());
    camera.y = std::atoi(args[1].c_str());
    camera.z = std::atoi(args[2].c_str());
    dfcapture::diagnostics_log("--- capture-at requested ---");
    DFHack::runOnRenderThread([camera]() { write_debug_capture(camera, "dfcapture_at.bmp"); });
    out.print("dfcapture: queued capture-at; check dfcapture_at.bmp / dfcapture.log.\n");
    return CR_OK;
}

command_result cmd_start(color_ostream& out, std::vector<std::string>& args) {
#ifdef _WIN32
    int port = dfcapture::DEFAULT_STREAM_PORT;
    std::string bind_address = dfcapture::DEFAULT_BIND_ADDRESS;

    if (!args.empty() && !parse_port(args[0], port)) {
        out.printerr("capture-stream-start: invalid port: %s\n", args[0].c_str());
        return CR_FAILURE;
    }
    if (args.size() >= 2)
        bind_address = args[1];

    std::string missing;
    if (!dfcapture::web_assets_ok(&missing)) {
        out.printerr("dfcapture: web UI not found: %s\n", missing.c_str());
        out.printerr("deploy the plugin's web/ folder to <Dwarf Fortress>/%s/ and retry.\n",
                     dfcapture::web_root());
        dfcapture::diagnostics_log("web assets missing: " + missing);
        return CR_FAILURE;
    }

    if (dfcapture::server_running()) {
        out.printerr("dfcapture: stream server is already running\n");
        return CR_FAILURE;
    }

    std::string overlay_note;
    if (!dfcapture::disable_overlay_for_stream(out, &overlay_note)) {
        out.printerr("dfcapture: cannot stream -- %s\n", overlay_note.c_str());
        dfcapture::diagnostics_log("stream start failed: overlay could not be disabled: " +
                                          overlay_note);
        return CR_FAILURE;
    }

    std::string err;
    if (!dfcapture::start_server(port, bind_address, &err)) {
        dfcapture::restore_overlay_after_stream(&out);
        out.printerr("dfcapture: %s\n", err.c_str());
        return CR_FAILURE;
    }

    dfcapture::diagnostics_log("server started " +
                                      dfcapture::server_url(bind_address, port));
    print_line(out, "dfcapture: stream server at " +
                    dfcapture::server_url(bind_address, port) + "\n");
    if (!overlay_note.empty())
        print_line(out, "dfcapture: " + overlay_note + "\n");
    return CR_OK;
#else
    out.printerr("dfcapture streaming is currently Windows-only.\n");
    return CR_FAILURE;
#endif
}

command_result cmd_stop(color_ostream& out, std::vector<std::string>&) {
#ifdef _WIN32
    dfcapture::stop_server();
    dfcapture::restore_overlay_after_stream(&out);
    dfcapture::diagnostics_log("server stopped");
    out.print("dfcapture: stream server stopped.\n");
    return CR_OK;
#else
    out.printerr("dfcapture streaming is currently Windows-only.\n");
    return CR_FAILURE;
#endif
}

command_result cmd_status(color_ostream& out, std::vector<std::string>&) {
#ifdef _WIN32
    if (dfcapture::server_running())
        print_line(out, "dfcapture: stream server running at " +
                        dfcapture::server_url() + "\n");
    else
        out.print("dfcapture: stream server stopped.\n");
    return CR_OK;
#else
    out.printerr("dfcapture streaming is currently Windows-only.\n");
    return CR_FAILURE;
#endif
}

} // namespace

DFhackCExport command_result plugin_init(color_ostream& out, std::vector<PluginCommand>& commands) {
    commands.push_back(PluginCommand(
        "capture",
        "Path-2 test: render the current view offscreen and save dfcapture_test.bmp",
        cmd_capture));
    commands.push_back(PluginCommand(
        "capture-at",
        "Path-2 test: render an arbitrary camera <x> <y> <z> offscreen -> dfcapture_at.bmp",
        cmd_capture_at));
    commands.push_back(PluginCommand(
        "capture-stream-start",
        "Start the premium MJPEG stream server; usage: capture-stream-start [port] [bind-address]",
        cmd_start));
    commands.push_back(PluginCommand(
        "capture-stream-stop",
        "Stop the premium MJPEG stream server",
        cmd_stop));
    commands.push_back(PluginCommand(
        "capture-stream-status",
        "Show the premium stream server status",
        cmd_status));

    out.print("dfcapture: loaded. Start browser streaming after a fort is loaded with: capture-stream-start\n");
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream&) {
#ifdef _WIN32
    dfcapture::diagnostics_log("plugin shutdown");
    dfcapture::stop_server();
    dfcapture::restore_overlay_after_stream();
    dfcapture::shutdown_image_encoder();
#endif
    return CR_OK;
}
