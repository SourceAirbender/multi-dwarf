#include "Core.h"
#include "Export.h"
#include "PluginManager.h"

#include "diagnostics.h"
#include "http_server.h"
#include "image_encoder.h"
#include "overlay_control.h"
#include "web_assets.h"

#include <cstdlib>
#include <string>
#include <vector>

using namespace DFHack;

DFHACK_PLUGIN("dfcapture_public");

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

command_result cmd_start(color_ostream& out, std::vector<std::string>& args) {
#ifdef _WIN32
    int port = dfcapture_public::DEFAULT_STREAM_PORT;
    std::string bind_address = dfcapture_public::DEFAULT_BIND_ADDRESS;

    if (!args.empty() && !parse_port(args[0], port)) {
        out.printerr("dfcapture-public-start: invalid port: %s\n", args[0].c_str());
        return CR_FAILURE;
    }
    if (args.size() >= 2)
        bind_address = args[1];

    std::string missing;
    if (!dfcapture_public::web_assets_ok(&missing)) {
        out.printerr("dfcapture-public-start: web UI not found: %s\n", missing.c_str());
        out.printerr("copy this repo's web/ folder to <Dwarf Fortress>/hack/dfcapture-public-web/\n");
        dfcapture_public::diagnostics_log("web assets missing: " + missing);
        return CR_FAILURE;
    }

    if (dfcapture_public::server_running()) {
        out.printerr("dfcapture-public-start: server is already running\n");
        return CR_FAILURE;
    }

    std::string overlay_note;
    if (!dfcapture_public::disable_overlay_for_stream(out, &overlay_note)) {
        out.printerr("dfcapture-public-start: cannot stream -- %s\n", overlay_note.c_str());
        dfcapture_public::diagnostics_log("stream start failed: overlay could not be disabled: " +
                                          overlay_note);
        return CR_FAILURE;
    }

    std::string err;
    if (!dfcapture_public::start_server(port, bind_address, &err)) {
        dfcapture_public::restore_overlay_after_stream(&out);
        out.printerr("dfcapture-public-start: %s\n", err.c_str());
        return CR_FAILURE;
    }

    dfcapture_public::diagnostics_log("server started " +
                                      dfcapture_public::server_url(bind_address, port));
    print_line(out, "dfcapture public server: " +
                    dfcapture_public::server_url(bind_address, port) + "\n");
    if (!overlay_note.empty())
        print_line(out, "dfcapture public: " + overlay_note + "\n");
    return CR_OK;
#else
    out.printerr("dfcapture public server is currently Windows-only.\n");
    return CR_FAILURE;
#endif
}

command_result cmd_stop(color_ostream& out, std::vector<std::string>&) {
#ifdef _WIN32
    dfcapture_public::stop_server();
    dfcapture_public::restore_overlay_after_stream(&out);
    dfcapture_public::diagnostics_log("server stopped");
    out.print("dfcapture public server stopped.\n");
    return CR_OK;
#else
    out.printerr("dfcapture public server is currently Windows-only.\n");
    return CR_FAILURE;
#endif
}

command_result cmd_status(color_ostream& out, std::vector<std::string>&) {
#ifdef _WIN32
    if (dfcapture_public::server_running())
        print_line(out, "dfcapture public server running at " +
                        dfcapture_public::server_url() + "\n");
    else
        out.print("dfcapture public server stopped.\n");
    return CR_OK;
#else
    out.printerr("dfcapture public server is currently Windows-only.\n");
    return CR_FAILURE;
#endif
}

} // namespace

DFhackCExport command_result plugin_init(color_ostream& out, std::vector<PluginCommand>& commands) {
    commands.push_back(PluginCommand(
        "dfcapture-public-start",
        "Start the dfcapture public reconstruction web server; usage: dfcapture-public-start [port] [bind-address]",
        cmd_start));
    commands.push_back(PluginCommand(
        "dfcapture-public-stop",
        "Stop the dfcapture public reconstruction web server",
        cmd_stop));
    commands.push_back(PluginCommand(
        "dfcapture-public-status",
        "Show the dfcapture public reconstruction web server status",
        cmd_status));

    out.print("dfcapture public reconstruction loaded. Start with: dfcapture-public-start\n");
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream&) {
#ifdef _WIN32
    dfcapture_public::diagnostics_log("plugin shutdown");
    dfcapture_public::stop_server();
    dfcapture_public::restore_overlay_after_stream();
    dfcapture_public::shutdown_image_encoder();
#endif
    return CR_OK;
}
