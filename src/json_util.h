#pragma once

#include "httplib.h"

#include <sstream>
#include <string>
#include <vector>

namespace dfcapture {

bool query_int(const httplib::Request& req, const char* name, int& value);
bool is_safe_player_id(const std::string& player);
std::string query_player(const httplib::Request& req);

std::string json_escape(const std::string& raw);
std::string json_string(const std::string& raw);
void append_json_string_array(std::ostringstream& body, const std::vector<std::string>& values);

} // namespace dfcapture
