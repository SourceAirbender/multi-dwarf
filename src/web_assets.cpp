// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, version 3 of the License.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// Runs on DFHack (Zlib); descends from DFPlex (Zlib) and webfort (ISC).
// Full license: see LICENSE. Third-party credits: see NOTICE.
//
// SPDX-License-Identifier: AGPL-3.0-only

#include "web_assets.h"

#include <fstream>
#include <sstream>
#include <string>

namespace dfcapture {

namespace {
constexpr const char* kWebRoot = "hack/dfcapture-web";
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
            "<!doctype html><html><head><meta charset=\"utf-8\"><title>dfcapture</title></head>"
            "<body style=\"font:14px/1.5 ui-monospace,Consolas,monospace;background:#161413;color:#f2e6cf;padding:28px\">"
            "<h1 style=\"color:#ffb74d\">dfcapture: web UI not found</h1>"
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

} // namespace dfcapture
