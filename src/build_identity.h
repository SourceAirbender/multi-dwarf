// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <string>

namespace dfcapture {

// The plugin<->browser compatibility contract. Bump kApiSchemaVersion on any breaking API change
// (a route, parameter, or response field required by the shipped web assets). Release packaging
// stamps the same value into web/build.json so /version can detect mismatched plugin and web files.
// kPluginVersion is informational only. This file is the single source of the schema number.
constexpr int kApiSchemaVersion = 2;
constexpr const char* kPluginVersion = "0.9.46";

// The /version payload distinguishes API compatibility from an exact packaged build match.
// - schemaCompatible: plugin and web speak the same API schema.
// - exactBuildMatch: version + source revision match.
// Development builds block schema incompatibility; release packages require both.
std::string build_identity_json();

} // namespace dfcapture
