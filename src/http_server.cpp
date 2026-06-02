#include "http_server.h"

#include "building_zone.h"
#include "client_state.h"
#include "diagnostics.h"
#include "hud.h"
#include "sdl_capture.h"
#include "httplib.h"
#include "image_encoder.h"
#include "info_panel.h"
#include "interaction.h"
#include "json_util.h"
#include "labor.h"
#include "lua_bridge.h"
#include "notifications.h"
#include "placement.h"
#include "unit_sheet.h"
#include "stockpile_panel.h"
#include "web_assets.h"

#include <atomic>
#include <chrono>
#include <cctype>
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

std::string build_options_from_request(const httplib::Request& req) {
    static const char* option_names[] = {
        "hollow", "weapon_count",
        "plate_units", "plate_water", "plate_magma", "plate_track", "plate_citizens",
        "plate_resets", "unit_min", "unit_max", "water_min", "water_max", "magma_min",
        "magma_max", "track_min", "track_max", "track_dump", "dump_x", "dump_y",
        "friction", "speed",
    };
    std::ostringstream out;
    for (auto name : option_names) {
        int value = 0;
        if (query_int(req, name, value))
            out << name << "=" << value << ";";
    }
    for (int i = 0; i < 4; ++i) {
        std::string key = "mat" + std::to_string(i);
        if (!req.has_param(key.c_str()))
            continue;
        std::string value = req.get_param_value(key.c_str());
        bool clean = value == "closest";
        if (!clean) {
            clean = !value.empty() && value.size() < 32;
            for (char c : value) {
                if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == ':')) {
                    clean = false;
                    break;
                }
            }
        }
        if (clean)
            out << key << "=" << value << ";";
    }
    return out.str();
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

    server.Get("/lua-ping", [](const httplib::Request& req, httplib::Response& res) {
        int value = 41;
        query_int(req, "n", value);
        int out = 0;
        std::string err;
        if (!lua_ping(value, out, &err)) {
            res.status = 500;
            res.set_content("lua ping failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"value\":" + std::to_string(out) + "}\n",
                        "application/json; charset=utf-8");
    });

    server.Get("/build-catalog", [](const httplib::Request&, httplib::Response& res) {
        std::string err;
        std::string json = building_catalog_json_via_lua(&err);
        if (json.empty()) {
            res.status = 500;
            res.set_content("catalog failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    server.Get("/build-materials", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("token")) {
            res.status = 400;
            res.set_content("missing token\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = build_materials_json_via_lua(req.get_param_value("token"), &err);
        if (json.empty()) {
            res.status = 500;
            res.set_content("materials failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    auto build_place_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int px = 0, py = 0, frame_w = 0, frame_h = 0;
        if (!query_int(req, "px", px) || !query_int(req, "py", py) ||
                !query_int(req, "w", frame_w) || !query_int(req, "h", frame_h) ||
                !req.has_param("token")) {
            res.status = 400;
            res.set_content("missing px/py/w/h/token\n", "text/plain; charset=utf-8");
            return;
        }
        int px2 = px, py2 = py, direction = -1;
        query_int(req, "px2", px2);
        query_int(req, "py2", py2);
        query_int(req, "direction", direction);

        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        int count = 0;
        int id = -1;
        std::string options = build_options_from_request(req);
        if (!place_building_via_lua(camera, px, py, px2, py2, frame_w, frame_h,
                                    req.get_param_value("token"), direction, options,
                                    count, id, &err)) {
            res.status = 400;
            res.set_content("building failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"count\":" + std::to_string(count) +
                        ",\"id\":" + std::to_string(id) + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/build-place", build_place_handler);
    server.Post("/build-place", build_place_handler);

    auto stockpile_create_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int px = 0, py = 0, frame_w = 0, frame_h = 0;
        if (!query_int(req, "px", px) || !query_int(req, "py", py) ||
                !query_int(req, "w", frame_w) || !query_int(req, "h", frame_h)) {
            res.status = 400;
            res.set_content("missing px/py/w/h\n", "text/plain; charset=utf-8");
            return;
        }
        int px2 = px, py2 = py;
        query_int(req, "px2", px2);
        query_int(req, "py2", py2);
        std::string preset = req.has_param("preset") ? req.get_param_value("preset") : "all";

        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        int id = -1;
        if (!create_stockpile_via_lua(camera, px, py, px2, py2, frame_w, frame_h,
                                      preset, id, &err)) {
            res.status = 400;
            res.set_content("stockpile failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"id\":" + std::to_string(id) + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/stockpile", stockpile_create_handler);
    server.Post("/stockpile", stockpile_create_handler);

    auto zone_create_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int px = 0, py = 0, frame_w = 0, frame_h = 0;
        if (!query_int(req, "px", px) || !query_int(req, "py", py) ||
                !query_int(req, "w", frame_w) || !query_int(req, "h", frame_h)) {
            res.status = 400;
            res.set_content("missing px/py/w/h\n", "text/plain; charset=utf-8");
            return;
        }
        int px2 = px, py2 = py;
        query_int(req, "px2", px2);
        query_int(req, "py2", py2);
        std::string type = req.has_param("type") ? req.get_param_value("type") : "MeetingHall";

        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        int id = -1;
        if (!create_zone_via_lua(camera, px, py, px2, py2, frame_w, frame_h, type, id, &err)) {
            res.status = 400;
            res.set_content("zone failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"id\":" + std::to_string(id) + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/zone", zone_create_handler);
    server.Post("/zone", zone_create_handler);

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

    server.Get("/zones", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        std::string json = zones_json_on_core_thread(player, camera, &err);
        if (json.empty()) {
            res.status = 500;
            res.set_content("zones failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    server.Get("/panel", [](const httplib::Request& req, httplib::Response& res) {
        std::string panel_name = req.has_param("panel") ? req.get_param_value("panel") : "citizens";
        std::string section = req.has_param("section") ? req.get_param_value("section") : "";
        std::string detail = req.has_param("detail") ? req.get_param_value("detail") : "";

        InfoPanel panel;
        std::string err;
        if (!info_panel_on_render_thread(panel_name, section, detail, panel, &err)) {
            res.status = 503;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(info_panel_json(panel), "application/json; charset=utf-8");
    });

    server.Get("/unit", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int unit_id = -1;
        if (!query_int(req, "id", unit_id)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing id\"}\n",
                            "application/json; charset=utf-8");
            return;
        }

        UnitSheet unit;
        Camera tile;
        std::string err;
        if (!unit_sheet_on_render_thread(unit_id, unit, tile, &err)) {
            res.status = 404;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(unit_sheet_json(player, unit, tile), "application/json; charset=utf-8");
    });

    auto action_handler = [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("action")) {
            res.status = 400;
            res.set_content("missing action\n", "text/plain; charset=utf-8");
            return;
        }

        std::string err;
        if (!action_on_core_thread(req.get_param_value("action"), &err)) {
            res.status = 400;
            res.set_content("action failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/action", action_handler);
    server.Post("/action", action_handler);

    auto stock_item_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int item_id = -1;
        if (!query_int(req, "id", item_id) || !req.has_param("action")) {
            res.status = 400;
            res.set_content("missing id/action\n", "text/plain; charset=utf-8");
            return;
        }

        StockItemActionResult result;
        if (!stock_item_action_on_core_thread(item_id, req.get_param_value("action"), result)) {
            res.status = 400;
            res.set_content("item action failed: " + result.err + "\n", "text/plain; charset=utf-8");
            return;
        }

        if (result.has_camera) {
            Camera camera = result.camera;
            std::string err;
            if (clamp_camera(camera, &err)) {
                result.camera = camera;
                set_player_camera(player, camera);
            }
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(stock_item_action_json(item_id, result), "application/json; charset=utf-8");
    };
    server.Get("/stock-item-action", stock_item_action_handler);
    server.Post("/stock-item-action", stock_item_action_handler);

    server.Get("/inspect", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int px = 0;
        int py = 0;
        int frame_w = 0;
        int frame_h = 0;
        if (!query_int(req, "px", px) || !query_int(req, "py", py) ||
            !query_int(req, "w", frame_w) || !query_int(req, "h", frame_h)) {
            res.status = 400;
            res.set_content("missing px/py/w/h\n", "text/plain; charset=utf-8");
            return;
        }

        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        InspectResult result;
        if (!inspect_on_core_thread(camera, px, py, frame_w, frame_h, result, &err)) {
            res.status = 503;
            res.set_content("inspect failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(inspect_json(player, result), "application/json; charset=utf-8");
    });

    server.Get("/hover", [](const httplib::Request& req, httplib::Response& res) {
        std::string player = query_player(req);
        int px = 0;
        int py = 0;
        int frame_w = 0;
        int frame_h = 0;
        if (!query_int(req, "px", px) || !query_int(req, "py", py) ||
            !query_int(req, "w", frame_w) || !query_int(req, "h", frame_h)) {
            res.status = 400;
            res.set_content("missing px/py/w/h\n", "text/plain; charset=utf-8");
            return;
        }

        Camera camera;
        std::string err;
        if (!camera_for_player(player, camera, &err)) {
            res.status = 503;
            res.set_content("camera failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        HoverResult result;
        if (!hover_on_core_thread(camera, px, py, frame_w, frame_h, result, &err)) {
            res.status = 503;
            res.set_content("hover failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(hover_json(player, result), "application/json; charset=utf-8");
    });

    server.Get("/labor", [](const httplib::Request& req, httplib::Response& res) {
        int detail = -1;
        query_int(req, "detail", detail);
        LaborState state;
        std::string err;
        if (!build_labor_state(detail, state, &err)) {
            res.status = 503;
            res.set_content("labor failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(labor_json(state), "application/json; charset=utf-8");
    });

    auto labor_toggle_handler = [](const httplib::Request& req, httplib::Response& res) {
        int detail = -1;
        int unit_id = -1;
        int on = 0;
        if (!query_int(req, "detail", detail) || !query_int(req, "unit", unit_id) ||
            !query_int(req, "on", on)) {
            res.status = 400;
            res.set_content("missing detail/unit/on\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!labor_toggle_impl(detail, unit_id, on != 0, &err)) {
            res.status = 400;
            res.set_content("toggle failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/labor-toggle", labor_toggle_handler);
    server.Post("/labor-toggle", labor_toggle_handler);

    auto labor_mode_handler = [](const httplib::Request& req, httplib::Response& res) {
        int detail = -1;
        int mode = -1;
        if (!query_int(req, "detail", detail) || !query_int(req, "mode", mode)) {
            res.status = 400;
            res.set_content("missing detail/mode\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!labor_mode_impl(detail, mode, &err)) {
            res.status = 400;
            res.set_content("mode failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/labor-mode", labor_mode_handler);
    server.Post("/labor-mode", labor_mode_handler);

    auto labor_specialist_handler = [](const httplib::Request& req, httplib::Response& res) {
        int unit_id = -1;
        int on = 0;
        if (!query_int(req, "unit", unit_id) || !query_int(req, "on", on)) {
            res.status = 400;
            res.set_content("missing unit/on\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!labor_specialist_impl(unit_id, on != 0, &err)) {
            res.status = 400;
            res.set_content("specialist failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/labor-specialist", labor_specialist_handler);
    server.Post("/labor-specialist", labor_specialist_handler);

    auto labor_create_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.has_param("name") ? req.get_param_value("name") : "";
        int index = -1;
        std::string err;
        if (!labor_create_impl(name, &index, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"index\":" + std::to_string(index) + "}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/labor-create", labor_create_handler);
    server.Post("/labor-create", labor_create_handler);

    auto labor_rename_handler = [](const httplib::Request& req, httplib::Response& res) {
        int detail = -1;
        if (!query_int(req, "detail", detail) || !req.has_param("name")) {
            res.status = 400;
            res.set_content("missing detail/name\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!labor_rename_impl(detail, req.get_param_value("name"), &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/labor-rename", labor_rename_handler);
    server.Post("/labor-rename", labor_rename_handler);

    auto labor_delete_handler = [](const httplib::Request& req, httplib::Response& res) {
        int detail = -1;
        if (!query_int(req, "detail", detail)) {
            res.status = 400;
            res.set_content("missing detail\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!labor_delete_impl(detail, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/labor-delete", labor_delete_handler);
    server.Post("/labor-delete", labor_delete_handler);

    auto labor_task_handler = [](const httplib::Request& req, httplib::Response& res) {
        int detail = -1;
        int labor = -1;
        int on = 0;
        if (!query_int(req, "detail", detail) || !query_int(req, "labor", labor) ||
            !query_int(req, "on", on)) {
            res.status = 400;
            res.set_content("missing detail/labor/on\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!labor_task_toggle_impl(detail, labor, on != 0, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/labor-task-toggle", labor_task_handler);
    server.Post("/labor-task-toggle", labor_task_handler);

    server.Get("/building-info", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        BuildingPanelInfo info;
        if (!building_info_on_core_thread(id, info)) {
            res.status = 404;
            res.set_content("{\"error\":\"building not found\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(building_info_json(info) + "\n", "application/json; charset=utf-8");
    });

    auto building_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string action = req.has_param("action") ? req.get_param_value("action") : "";
        std::string err;
        if (!building_action_on_core_thread(id, action, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/building-action", building_action_handler);
    server.Post("/building-action", building_action_handler);

    server.Get("/workshop-info", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = workshop_info_json_via_lua(id, &err);
        if (json.empty()) {
            res.status = 400;
            res.set_content("workshop info failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    auto workshop_add_job_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id) || !req.has_param("task")) {
            res.status = 400;
            res.set_content("missing id/task\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!workshop_add_job_via_lua(id, req.get_param_value("task"), &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/workshop-add-job", workshop_add_job_handler);
    server.Post("/workshop-add-job", workshop_add_job_handler);

    auto workshop_job_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int job_id = -1;
        if (!query_int(req, "id", id) || !query_int(req, "job", job_id) ||
                !req.has_param("action")) {
            res.status = 400;
            res.set_content("missing id/job/action\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!workshop_job_action_via_lua(id, job_id, req.get_param_value("action"), &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/workshop-job-action", workshop_job_action_handler);
    server.Post("/workshop-job-action", workshop_job_action_handler);

    auto workshop_worker_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int unit = -1;
        int assign = 0;
        if (!query_int(req, "id", id) || !query_int(req, "unit", unit)) {
            res.status = 400;
            res.set_content("missing id/unit\n", "text/plain; charset=utf-8");
            return;
        }
        query_int(req, "assign", assign);
        std::string err;
        if (!workshop_worker_action_via_lua(id, unit, assign != 0, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/workshop-worker-action", workshop_worker_action_handler);
    server.Post("/workshop-worker-action", workshop_worker_action_handler);

    auto workshop_workers_clear_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        if (!workshop_workers_clear_via_lua(id, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/workshop-workers-clear", workshop_workers_clear_handler);
    server.Post("/workshop-workers-clear", workshop_workers_clear_handler);

    server.Get("/zone-info", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        ZonePanelInfo info;
        if (!zone_info_on_core_thread(id, info)) {
            res.status = 404;
            res.set_content("{\"error\":\"zone not found\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(zone_info_json(info) + "\n", "application/json; charset=utf-8");
    });

    auto zone_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string action = req.has_param("action") ? req.get_param_value("action") : "";
        std::string err;
        if (!zone_action_on_core_thread(id, action, &err)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":" + json_string(err) + "}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/zone-action", zone_action_handler);
    server.Post("/zone-action", zone_action_handler);

    server.Get("/zone-units", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = zone_units_json_on_core_thread(id, &err);
        if (json.empty()) {
            res.status = 400;
            res.set_content("zone units failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json + "\n", "application/json; charset=utf-8");
    });

    auto zone_unit_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int unit = -1;
        int assign = 0;
        if (!query_int(req, "id", id) || !query_int(req, "unit", unit)) {
            res.status = 400;
            res.set_content("missing id/unit\n", "text/plain; charset=utf-8");
            return;
        }
        query_int(req, "assign", assign);
        std::string kind = req.has_param("kind") ? req.get_param_value("kind") : "unit";
        std::string err;
        if (!zone_unit_action_on_core_thread(id, unit, assign != 0, kind, &err)) {
            res.status = 400;
            res.set_content("zone unit action failed: " + err + "\n",
                            "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/zone-unit-action", zone_unit_action_handler);
    server.Post("/zone-unit-action", zone_unit_action_handler);

    server.Get("/zone-owners", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = zone_owners_json_on_core_thread(id, &err);
        if (json.empty()) {
            res.status = 400;
            res.set_content("zone owners failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json + "\n", "application/json; charset=utf-8");
    });

    auto zone_owner_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int unit = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        query_int(req, "unit", unit);
        std::string err;
        if (!zone_owner_action_on_core_thread(id, unit, &err)) {
            res.status = 400;
            res.set_content("zone owner action failed: " + err + "\n",
                            "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/zone-owner-action", zone_owner_action_handler);
    server.Post("/zone-owner-action", zone_owner_action_handler);

    server.Get("/zone-locations", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = zone_locations_json_via_lua(id, &err);
        if (json.empty()) {
            res.status = 400;
            res.set_content("zone locations failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json + "\n", "application/json; charset=utf-8");
    });

    auto zone_location_action_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int location = -1;
        if (!query_int(req, "id", id) || !req.has_param("action")) {
            res.status = 400;
            res.set_content("missing id/action\n", "text/plain; charset=utf-8");
            return;
        }
        query_int(req, "location", location);
        std::string kind = req.has_param("kind") ? req.get_param_value("kind") : "";
        std::string err;
        if (!zone_location_action_via_lua(id, req.get_param_value("action"), kind,
                                          location, &err)) {
            res.status = 400;
            res.set_content("zone location action failed: " + err + "\n",
                            "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/zone-location-action", zone_location_action_handler);
    server.Post("/zone-location-action", zone_location_action_handler);

    server.Get("/stockpile-info", [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        std::string json = stockpile_info_json_on_core_thread(id);
        if (json.empty()) {
            res.status = 404;
            res.set_content("not a stockpile\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    auto stockpile_rename_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id) || !req.has_param("name")) {
            res.status = 400;
            res.set_content("missing id/name\n", "text/plain; charset=utf-8");
            return;
        }
        bool ok = rename_stockpile_on_core_thread(id, req.get_param_value("name"));
        res.set_header("Cache-Control", "no-store");
        res.set_content(ok ? "{\"ok\":true}\n" : "{\"ok\":false}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/stockpile-rename", stockpile_rename_handler);
    server.Post("/stockpile-rename", stockpile_rename_handler);

    auto stockpile_remove_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        bool ok = remove_stockpile_on_core_thread(id);
        res.set_header("Cache-Control", "no-store");
        res.set_content(ok ? "{\"ok\":true}\n" : "{\"ok\":false}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/stockpile-remove", stockpile_remove_handler);
    server.Post("/stockpile-remove", stockpile_remove_handler);

    auto stockpile_links_only_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int on = 0;
        if (!query_int(req, "id", id) || !query_int(req, "on", on)) {
            res.status = 400;
            res.set_content("missing id/on\n", "text/plain; charset=utf-8");
            return;
        }
        bool ok = set_stockpile_links_only_on_core_thread(id, on != 0);
        res.set_header("Cache-Control", "no-store");
        res.set_content(ok ? "{\"ok\":true}\n" : "{\"ok\":false}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/stockpile-links-only", stockpile_links_only_handler);
    server.Post("/stockpile-links-only", stockpile_links_only_handler);

    auto stockpile_storage_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            res.status = 400;
            res.set_content("missing id\n", "text/plain; charset=utf-8");
            return;
        }
        int barrels = -1;
        int bins = -1;
        int wheelbarrows = -1;
        query_int(req, "barrels", barrels);
        query_int(req, "bins", bins);
        query_int(req, "wheelbarrows", wheelbarrows);
        bool ok = set_stockpile_storage_on_core_thread(id, barrels, bins, wheelbarrows);
        res.set_header("Cache-Control", "no-store");
        res.set_content(ok ? "{\"ok\":true}\n" : "{\"ok\":false}\n",
                        "application/json; charset=utf-8");
    };
    server.Get("/stockpile-storage", stockpile_storage_handler);
    server.Post("/stockpile-storage", stockpile_storage_handler);

    auto stockpile_link_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int target = -1;
        int on = 1;
        if (!query_int(req, "id", id) || !query_int(req, "target", target) ||
            !req.has_param("mode")) {
            res.status = 400;
            res.set_content("missing id/target/mode\n", "text/plain; charset=utf-8");
            return;
        }
        query_int(req, "on", on);
        std::string err;
        if (!set_stockpile_link_on_core_thread(id, target, req.get_param_value("mode"),
                                               on != 0, &err)) {
            res.status = 400;
            res.set_content("link failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/stockpile-link", stockpile_link_handler);
    server.Post("/stockpile-link", stockpile_link_handler);

    auto stockpile_set_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id) || !req.has_param("preset")) {
            res.status = 400;
            res.set_content("missing id/preset\n", "text/plain; charset=utf-8");
            return;
        }
        std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "set";
        std::string err;
        if (!set_stockpile_category_on_core_thread(id, req.get_param_value("preset"), mode, &err)) {
            res.status = 400;
            res.set_content("set failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/stockpile-set", stockpile_set_handler);
    server.Post("/stockpile-set", stockpile_set_handler);

    auto stockpile_group_from_request = [](const httplib::Request& req) {
        return req.has_param("group") ? req.get_param_value("group") : std::string();
    };

    server.Get("/stockpile-cat-groups", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("cat")) {
            res.status = 400;
            res.set_content("missing cat\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = stockpile_groups_via_lua(req.get_param_value("cat"), &err);
        if (json.empty()) {
            res.status = 500;
            res.set_content("groups failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    server.Get("/stockpile-items", [stockpile_group_from_request](const httplib::Request& req,
                                                                 httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id) || !req.has_param("cat")) {
            res.status = 400;
            res.set_content("missing id/cat\n", "text/plain; charset=utf-8");
            return;
        }
        std::string err;
        std::string json = stockpile_items_via_lua(id, req.get_param_value("cat"),
                                                   stockpile_group_from_request(req), &err);
        if (json.empty()) {
            res.status = 500;
            res.set_content("items failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json; charset=utf-8");
    });

    auto stockpile_toggle_item_handler =
        [stockpile_group_from_request](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int idx = -1;
        int on = 0;
        if (!query_int(req, "id", id) || !req.has_param("cat") ||
                !query_int(req, "idx", idx)) {
            res.status = 400;
            res.set_content("missing id/cat/idx\n", "text/plain; charset=utf-8");
            return;
        }
        query_int(req, "on", on);
        std::string err;
        if (!stockpile_toggle_item_via_lua(id, req.get_param_value("cat"),
                                           stockpile_group_from_request(req),
                                           idx, on != 0, &err)) {
            res.status = 400;
            res.set_content("toggle failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/stockpile-toggle-item", stockpile_toggle_item_handler);
    server.Post("/stockpile-toggle-item", stockpile_toggle_item_handler);

    auto stockpile_toggle_all_handler =
        [stockpile_group_from_request](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int on = 0;
        if (!query_int(req, "id", id) || !req.has_param("cat")) {
            res.status = 400;
            res.set_content("missing id/cat\n", "text/plain; charset=utf-8");
            return;
        }
        query_int(req, "on", on);
        std::string err;
        if (!stockpile_toggle_all_via_lua(id, req.get_param_value("cat"),
                                          stockpile_group_from_request(req),
                                          on != 0, &err)) {
            res.status = 400;
            res.set_content("toggle-all failed: " + err + "\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true}\n", "application/json; charset=utf-8");
    };
    server.Get("/stockpile-toggle-all", stockpile_toggle_all_handler);
    server.Post("/stockpile-toggle-all", stockpile_toggle_all_handler);
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
