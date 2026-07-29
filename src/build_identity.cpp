// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios
// SPDX-License-Identifier: AGPL-3.0-only

#include "build_identity.h"

#include "frame_delta.h"
#include "json_util.h"
#include "web_assets.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace dfcapture {
namespace {

#ifndef DFCAPTURE_GIT_REV
#define DFCAPTURE_GIT_REV "unknown"
#endif

// Stamped at each compile, so the DLL always carries the moment it was built.
constexpr const char* kBuildTime = __DATE__ " " __TIME__;
constexpr const char* kSourceCommit = DFCAPTURE_GIT_REV;

// Minimal field scans over the package-generated build.json. The file is small and has a fixed
// shape, so a targeted scan is sufficient.
struct WebBuild {
    int schema = -1;
    std::string version;
    std::string build_id;
    std::string built_at;
    std::string source_commit;
    std::string mode;
};

bool read_web_build(WebBuild& build) {
    const char* root = web_root();
    std::string path = root ? root : "";
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
        path += "/";
    path += "build.json";
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;   // Unpackaged source tree.
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    auto pos = text.find("\"schema\"");
    if (pos != std::string::npos) {
        pos = text.find(':', pos);
        if (pos != std::string::npos)
            build.schema = std::atoi(text.c_str() + pos + 1);
    }
    auto scan_str = [&](const char* key, std::string& out) {
        auto p = text.find(key);
        if (p == std::string::npos) return;
        p = text.find(':', p);
        if (p == std::string::npos) return;
        p = text.find('"', p);
        if (p == std::string::npos) return;
        auto e = text.find('"', p + 1);
        if (e == std::string::npos) return;
        out = text.substr(p + 1, e - p - 1);
    };
    scan_str("\"version\"", build.version);
    scan_str("\"buildId\"", build.build_id);
    scan_str("\"builtAt\"", build.built_at);
    scan_str("\"sourceCommit\"", build.source_commit);
    scan_str("\"mode\"", build.mode);
    return true;
}

} // namespace

std::string build_identity_json() {
    WebBuild web;
    const bool web_present = read_web_build(web);

    // A missing build.json is permitted for source builds, but package identity is not enforced.
    const bool schema_compatible = !web_present || web.schema == kApiSchemaVersion;
    const bool version_matches = web_present && web.version == kPluginVersion;
    const bool commit_comparable = web_present && !web.source_commit.empty() &&
                                   std::string(kSourceCommit) != "unknown";
    const bool commit_matches = !commit_comparable || web.source_commit == kSourceCommit;
    const bool exact_build_match = web_present && version_matches && commit_matches;
    const bool release_mode = web_present && web.mode == "release";
    const bool compatible = schema_compatible && (!release_mode || exact_build_match);
    std::string reason;
    std::string warning;
    if (!schema_compatible)
        reason = "the plugin (schema " + std::to_string(kApiSchemaVersion) +
                 ") and the web assets (schema " + std::to_string(web.schema) +
                 ") use incompatible APIs; redeploy both, then restart Dwarf Fortress";
    else if (release_mode && !exact_build_match)
        reason = "the release plugin and web assets are not from the same build; redeploy the "
                 "complete package, then restart Dwarf Fortress";
    else if (!web_present)
        warning = "web/build.json missing (source/dev tree); compatibility is not enforced";
    else if (!exact_build_match)
        warning = "development plugin/web revisions differ but their API schemas are compatible";

    std::ostringstream out;
    out << "{\"ok\":true,\"service\":\"dfcapture\""
        << ",\"version\":" << json_string(kPluginVersion)
        << ",\"schema\":" << kApiSchemaVersion
        << ",\"buildTime\":" << json_string(kBuildTime)
        << ",\"sourceCommit\":" << json_string(kSourceCommit)
        << ",\"web\":";
    if (web_present)
        out << "{\"schema\":" << web.schema
            << ",\"version\":" << json_string(web.version)
            << ",\"buildId\":" << json_string(web.build_id)
            << ",\"builtAt\":" << json_string(web.built_at)
            << ",\"sourceCommit\":" << json_string(web.source_commit)
            << ",\"mode\":" << json_string(web.mode) << "}";
    else
        out << "null";
    out << ",\"schemaCompatible\":" << (schema_compatible ? "true" : "false")
        << ",\"exactBuildMatch\":" << (exact_build_match ? "true" : "false")
        << ",\"capabilities\":{\"frameDelta\":{\"protocol\":" << kFrameDeltaProtocol << ","
           "\"keyframes\":\"jpeg\",\"patches\":\"png\"},"
           "\"frameLongPoll\":{\"protocol\":1,\"maxHoldMs\":250}}"
        << ",\"compatible\":" << (compatible ? "true" : "false")
        << ",\"reason\":" << json_string(reason)
        << ",\"warning\":" << json_string(warning) << "}\n";
    return out.str();
}

} // namespace dfcapture
