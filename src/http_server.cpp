#include "http_server.h"

#include "httplib.h"
#include "web_assets.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace dfcapture_public {
namespace {

std::mutex g_server_mutex;
std::unique_ptr<httplib::Server> g_server;
std::thread g_server_thread;
std::atomic<bool> g_running(false);
int g_port = DEFAULT_STREAM_PORT;
std::string g_bind_address = DEFAULT_BIND_ADDRESS;

void register_routes(httplib::Server& server) {
    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/view");
    });

    server.Get("/view", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(index_html(), "text/html; charset=utf-8");
    });

    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"ok\":true,\"service\":\"dfcapture_public\"}\n",
                        "application/json; charset=utf-8");
    });
}

} // namespace

std::string server_url(const std::string& bind_address, int port) {
    std::string host = bind_address == "0.0.0.0" ? "127.0.0.1" : bind_address;
    return "http://" + host + ":" + std::to_string(port) + "/view";
}

std::string server_url() {
    std::lock_guard<std::mutex> lock(g_server_mutex);
    return server_url(g_bind_address, g_port);
}

bool server_running() {
    return g_running.load();
}

bool start_server(int port, const std::string& bind_address, std::string* err) {
    std::lock_guard<std::mutex> lock(g_server_mutex);
    if (g_server) {
        if (err) *err = "server is already running";
        return false;
    }

    auto server = std::make_unique<httplib::Server>();
    register_routes(*server);

    if (!server->bind_to_port(bind_address.c_str(), port)) {
        if (err) *err = "failed to bind " + bind_address + ":" + std::to_string(port);
        return false;
    }

    g_port = port;
    g_bind_address = bind_address;
    g_running = true;
    g_server = std::move(server);
    g_server_thread = std::thread([] {
        g_server->listen_after_bind();
        g_running = false;
    });
    return true;
}

void stop_server() {
    std::unique_ptr<httplib::Server> server;
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(g_server_mutex);
        if (!g_server)
            return;
        g_server->stop();
        server = std::move(g_server);
        thread = std::move(g_server_thread);
    }

    if (thread.joinable())
        thread.join();
    g_running = false;
}

} // namespace dfcapture_public
