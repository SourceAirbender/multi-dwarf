#include "http_server.h"

#include "hud.h"
#include "sdl_capture.h"
#include "httplib.h"
#include "image_encoder.h"
#include "json_util.h"
#include "notifications.h"
#include "web_assets.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dfcapture_public {
namespace {

std::mutex g_server_mutex;
std::unique_ptr<httplib::Server> g_server;
std::thread g_server_thread;
std::atomic<bool> g_running(false);
int g_port = DEFAULT_STREAM_PORT;
std::string g_bind_address = DEFAULT_BIND_ADDRESS;
std::mutex g_camera_mutex;
std::unordered_map<std::string, Camera> g_player_cameras;

std::string camera_json(const Camera& camera) {
    return "{\"x\":" + std::to_string(camera.x) +
           ",\"y\":" + std::to_string(camera.y) +
           ",\"z\":" + std::to_string(camera.z) + "}\n";
}

bool camera_for_player(const std::string& player, Camera& camera, std::string* err) {
    {
        std::lock_guard<std::mutex> lock(g_camera_mutex);
        auto it = g_player_cameras.find(player);
        if (it != g_player_cameras.end()) {
            camera = it->second;
            return true;
        }
    }

    if (!read_host_camera(camera, err))
        return false;
    clamp_camera(camera, nullptr);

    {
        std::lock_guard<std::mutex> lock(g_camera_mutex);
        g_player_cameras[player] = camera;
    }
    return true;
}

void set_player_camera(const std::string& player, const Camera& camera) {
    std::lock_guard<std::mutex> lock(g_camera_mutex);
    g_player_cameras[player] = camera;
}

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

    server.Get("/camera", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":\"" + err + "\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_content(camera_json(camera), "application/json; charset=utf-8");
    });

    server.Post("/camera/move", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":\"" + err + "\"}\n",
                            "application/json; charset=utf-8");
            return;
        }

        int dx = 0;
        int dy = 0;
        int dz = 0;
        query_int(req, "dx", dx);
        query_int(req, "dy", dy);
        query_int(req, "dz", dz);
        camera.x += dx;
        camera.y += dy;
        camera.z += dz;
        if (!clamp_camera(camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":\"" + err + "\"}\n",
                            "application/json; charset=utf-8");
            return;
        }

        set_player_camera(player, camera);
        res.set_content(camera_json(camera), "application/json; charset=utf-8");
    });

    server.Post("/camera/set", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        query_int(req, "x", camera.x);
        query_int(req, "y", camera.y);
        query_int(req, "z", camera.z);
        if (!clamp_camera(camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        set_player_camera(player, camera);
        res.set_content(camera_json(camera), "application/json; charset=utf-8");
    });

    server.Post("/camera/home", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        Camera camera;
        std::string err;
        if (!read_host_camera(camera, &err) || !clamp_camera(camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        set_player_camera(player, camera);
        res.set_content(camera_json(camera), "application/json; charset=utf-8");
    });

    server.Get("/frame.jpg", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera unavailable: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        std::vector<uint8_t> jpeg;
        if (!capture_camera_jpeg(camera, jpeg, &err)) {
            res.status = 503;
            res.set_content("capture failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(reinterpret_cast<const char*>(jpeg.data()), jpeg.size(), "image/jpeg");
    });

    auto stream_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        auto last_frame = std::make_shared<std::chrono::steady_clock::time_point>(
            std::chrono::steady_clock::now() - std::chrono::milliseconds(1000));
        auto interval = std::chrono::milliseconds(1000 / DEFAULT_STREAM_FPS);

        res.set_header("Cache-Control", "no-store");
        res.set_header("Connection", "close");
        res.set_header("Content-Type", "multipart/x-mixed-replace; boundary=dfcapture");
        res.set_chunked_content_provider(
            [player, last_frame, interval](size_t, httplib::DataSink& sink) mutable {
                if (!g_running.load() || !sink.is_writable()) {
                    sink.done();
                    return;
                }

                auto now = std::chrono::steady_clock::now();
                auto elapsed = now - *last_frame;
                if (elapsed < interval)
                    std::this_thread::sleep_for(interval - elapsed);

                Camera camera;
                std::string err;
                if (!camera_for_player(player, camera, &err)) {
                    sink.done();
                    return;
                }

                std::vector<uint8_t> jpeg;
                if (!capture_camera_jpeg(camera, jpeg, &err)) {
                    sink.done();
                    return;
                }

                std::ostringstream header;
                header << "--dfcapture\r\n"
                       << "Content-Type: image/jpeg\r\n"
                       << "Content-Length: " << jpeg.size() << "\r\n"
                       << "X-DFCapture-Camera: " << camera.x << "," << camera.y << "," << camera.z
                       << "\r\n\r\n";
                std::string h = header.str();
                sink.write(h.data(), h.size());
                sink.write(reinterpret_cast<const char*>(jpeg.data()), jpeg.size());
                sink.write("\r\n", 2);
                *last_frame = std::chrono::steady_clock::now();
            });
    };

    server.Get("/stream", stream_handler);
    server.Get("/stream.mjpg", stream_handler);

    server.Get("/hud", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        HudState hud;
        if (!hud_on_render_thread(camera, hud, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(hud_json(player, hud), "application/json; charset=utf-8");
    });

    server.Get("/notifications", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        NotificationState state;
        std::string err;
        if (!notifications_on_render_thread(state, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(notifications_json(player, state), "application/json; charset=utf-8");
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
