#include "web_assets.h"

#include <fstream>
#include <sstream>
#include <string>

namespace dfcapture_public {

namespace {
constexpr const char* kWebRoot = "hack/dfcapture-public-web";
}

const char* web_root() {
    return kWebRoot;
}

bool web_assets_ok(std::string* missing) {
    std::string path = std::string(kWebRoot) + "/index.html";
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (missing) *missing = path;
        return false;
    }
    return true;
}

std::string index_html() {
    std::string path = std::string(kWebRoot) + "/index.html";
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return std::string(
            "<!doctype html><html><head><meta charset=\"utf-8\"><title>dfcapture public</title></head>"
            "<body style=\"font:14px/1.5 ui-monospace,Consolas,monospace;background:#161413;color:#f2e6cf;padding:28px\">"
            "<h1 style=\"color:#ffb74d\">dfcapture public: web UI not found</h1>"
            "<p>Could not open <code>") + path + "</code> "
            "(relative to the Dwarf Fortress folder).</p>"
            "<p>Copy this repository's <code>web/</code> directory to "
            "<code>&lt;Dwarf&nbsp;Fortress&gt;/" + std::string(kWebRoot) + "/</code>, then reload.</p>"
            "</body></html>";
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace dfcapture_public
