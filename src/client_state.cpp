#include "client_state.h"

#include "sdl_capture.h"

#include <mutex>
#include <unordered_map>

namespace dfcapture_public {
namespace {

std::mutex g_client_mutex;
std::unordered_map<std::string, Camera> g_player_cameras;

} // namespace

bool camera_for_player(const std::string& player, Camera& camera, std::string* err) {
    {
        std::lock_guard<std::mutex> lock(g_client_mutex);
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
        std::lock_guard<std::mutex> lock(g_client_mutex);
        g_player_cameras[player] = camera;
    }
    return true;
}

void set_player_camera(const std::string& player, const Camera& camera) {
    std::lock_guard<std::mutex> lock(g_client_mutex);
    g_player_cameras[player] = camera;
}

void forget_player_camera(const std::string& player) {
    std::lock_guard<std::mutex> lock(g_client_mutex);
    g_player_cameras.erase(player);
}

std::vector<ClientCamera> client_camera_snapshot() {
    std::vector<ClientCamera> out;
    std::lock_guard<std::mutex> lock(g_client_mutex);
    out.reserve(g_player_cameras.size());
    for (const auto& entry : g_player_cameras)
        out.push_back(ClientCamera{entry.first, entry.second});
    return out;
}

} // namespace dfcapture_public
