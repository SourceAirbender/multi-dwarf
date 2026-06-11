#pragma once

#include <string>

#include "ColorText.h"

namespace dfcapture {

constexpr int DEFAULT_STREAM_PORT = 8765;
constexpr int DEFAULT_STREAM_FPS = 8;
constexpr const char* DEFAULT_BIND_ADDRESS = "127.0.0.1";

bool start_server(int port, const std::string& bind_address, std::string* err = nullptr);
void stop_server();
bool server_running();
std::string server_url();
std::string server_url(const std::string& bind_address, int port);

} // namespace dfcapture
