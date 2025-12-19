#include "Core.h"
#include "Export.h"
#include "PluginManager.h"

#include "http_server.h"

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

    std::string err;
    if (!dfcapture_public::start_server(port, bind_address, &err)) {
        out.printerr("dfcapture-public-start: %s\n", err.c_str());
        return CR_FAILURE;
    }

    print_line(out, "dfcapture public server: " +
                    dfcapture_public::server_url(bind_address, port) + "\n");
    return CR_OK;
#else
    out.printerr("dfcapture public server is currently Windows-only.\n");
    return CR_FAILURE;
#endif
}

command_result cmd_stop(color_ostream& out, std::vector<std::string>&) {
#ifdef _WIN32
    dfcapture_public::stop_server();
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
    dfcapture_public::stop_server();
#endif
    return CR_OK;
}
