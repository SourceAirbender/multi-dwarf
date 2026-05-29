#include "http_server.h"

#include "client_state.h"
#include "diagnostics.h"
#include "hud.h"
#include "sdl_capture.h"
#include "httplib.h"
#include "image_encoder.h"
#include "json_util.h"
#include "notifications.h"
#include "placement.h"
#include "web_assets.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace dfcapture_public {
namespace {

std::mutex g_server_mutex;
std::unique_ptr<httplib::Server> g_server;
std::thread g_server_thread;
std::atomic<bool> g_running(false);
int g_port = DEFAULT_STREAM_PORT;
std::string g_bind_address = DEFAULT_BIND_ADDRESS;

std::string camera_json(const Camera& camera) {
    return "{\"x\":" + std::to_string(camera.x) +
           ",\"y\":" + std::to_string(camera.y) +
           ",\"z\":" + std::to_string(camera.z) +
           ",\"zoom\":" + std::to_string(camera.zoom_factor >= 0 ? camera.zoom_factor : 100) +
           ",\"zoomExplicit\":" + (camera.zoom_factor >= 0 ? std::string("true") : std::string("false")) +
           "}\n";
}

std::string clients_json() {
    std::ostringstream body;
    auto clients = client_camera_snapshot();
    body << "{\"count\":" << clients.size() << ",\"clients\":[";
    for (size_t i = 0; i < clients.size(); ++i) {
        if (i) body << ",";
        body << "{\"player\":" << json_string(clients[i].player)
             << ",\"camera\":{\"x\":" << clients[i].camera.x
             << ",\"y\":" << clients[i].camera.y
             << ",\"z\":" << clients[i].camera.z
             << ",\"zoom\":" << (clients[i].camera.zoom_factor >= 0 ? clients[i].camera.zoom_factor : 100)
             << ",\"zoomExplicit\":" << (clients[i].camera.zoom_factor >= 0 ? "true" : "false")
             << "}}";
    }
    body << "]}\n";
    return body.str();
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

    server.Get("/state", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(diagnostics_json(player, camera, diagnostics_snapshot()),
                        "application/json; charset=utf-8");
    });

    server.Get("/clients", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(clients_json(), "application/json; charset=utf-8");
    });

    server.Get("/host-state", [](const httplib::Request&, httplib::Response& res) {
        HostState state;
        std::string err;
        if (!host_state_on_render_thread(state, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + ",\"state\":" +
                                host_state_json(state) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(host_state_json(state), "application/json; charset=utf-8");
    });

    auto reset_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        forget_player_camera(player);
        diagnostics_reset();

        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(camera_json(camera), "application/json; charset=utf-8");
    };
    server.Get("/reset", reset_handler);
    server.Post("/reset", reset_handler);

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

    auto zoom_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        std::string direction = req.has_param("dir") ? req.get_param_value("dir") : "reset";
        Camera camera;
        std::string err;
        if (!zoom_player_camera(player, direction, camera, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(camera_json(camera), "application/json; charset=utf-8");
    };
    server.Get("/zoom", zoom_handler);
    server.Post("/zoom", zoom_handler);

    server.Get("/zoom-probe", [](const httplib::Request&, httplib::Response& res) {
        ViewportProbe probe;
        std::string err;
        if (!viewport_probe_on_render_thread(probe, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) +
                                ",\"probe\":" + viewport_probe_json(probe) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(viewport_probe_json(probe), "application/json; charset=utf-8");
    });

    auto placement_mode_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "none";
        bool active = !(mode.empty() || mode == "none" || mode == "0" || mode == "off");
        Camera camera;
        std::string err;
        if (!set_player_placement_mode(player, active, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"placementMode\":" +
                            std::string(camera.placement_mode ? "true" : "false") + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/placement-mode", placement_mode_handler);
    server.Post("/placement-mode", placement_mode_handler);

    auto placement_cursor_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int hx = -1;
        int hy = -1;
        int frame_w = 0;
        int frame_h = 0;
        int drag = 0;
        int drag_x = -1;
        int drag_y = -1;
        int build_w = 0;
        int build_h = 0;
        query_int(req, "hx", hx);
        query_int(req, "hy", hy);
        query_int(req, "w", frame_w);
        query_int(req, "h", frame_h);
        query_int(req, "drag", drag);
        query_int(req, "dx", drag_x);
        query_int(req, "dy", drag_y);
        query_int(req, "bw", build_w);
        query_int(req, "bh", build_h);

        Camera camera;
        std::string err;
        if (!set_player_placement_cursor(player, hx, hy, frame_w, frame_h, drag != 0,
                                         drag_x, drag_y, build_w, build_h, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/placement-cursor", placement_cursor_handler);
    server.Post("/placement-cursor", placement_cursor_handler);

    auto designate_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        DesignationRequest desig;
        if (!query_int(req, "px", desig.px) ||
                !query_int(req, "py", desig.py) ||
                !query_int(req, "w", desig.frame_w) ||
                !query_int(req, "h", desig.frame_h)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing px/py/w/h\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        desig.px2 = desig.px;
        desig.py2 = desig.py;
        query_int(req, "px2", desig.px2);
        query_int(req, "py2", desig.py2);
        desig.tool = req.has_param("tool") ? req.get_param_value("tool") : "dig";
        int marker = 0;
        int warm_damp = 0;
        query_int(req, "priority", desig.priority);
        query_int(req, "marker", marker);
        query_int(req, "warmdamp", warm_damp);
        query_int(req, "minemode", desig.mine_mode);
        desig.marker = marker != 0;
        desig.warm_damp = warm_damp != 0;

        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        DesignationResult result;
        if (!designate_on_render_thread(camera, desig, result, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"count\":" + std::to_string(result.count) +
                            ",\"tool\":" + json_string(result.tool) + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/designate", designate_handler);
    server.Post("/designate", designate_handler);

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
