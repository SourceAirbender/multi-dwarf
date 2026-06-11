#pragma once

#include <string>

namespace dfcapture {

const char* web_root();
bool web_assets_ok(std::string* missing = nullptr);
std::string index_html();

} // namespace dfcapture
