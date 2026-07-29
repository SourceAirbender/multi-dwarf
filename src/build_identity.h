// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <string>

namespace dfcapture {

// The plugin<->browser compatibility contract. Bump kApiSchemaVersion on any BREAKING API change
// (a route, param, or response field the shipped web assets depend on). tools/deploy.ps1 reads this
// same constant and stamps it into the deployed web/build.json, so a DLL and a web tree built from
// different schemas are detected at /version (see build_identity_json). kPluginVersion is
// informational only. THIS FILE is the single source of the schema number -- do not duplicate it.
constexpr int kApiSchemaVersion = 2;
constexpr const char* kPluginVersion = "0.9.46";

// The /version payload distinguishes API compatibility from an exact packaged build match.
// - schemaCompatible: plugin and web speak the same API schema.
// - exactBuildMatch: version + source revision match.
// Development deployments only block schema incompatibility; release deployments require both.
std::string build_identity_json();

} // namespace dfcapture
