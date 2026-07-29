// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dfcapture {

// Pointer-free artwork data captured while DF is suspended. Prose composition consumes only this
// neutral model, so no lazy art-image pointer escapes the core-thread read section.
struct ArtworkModel {
    int32_t art_id = -1;
    int16_t art_sub_id = -1;
    std::string name;
    std::vector<std::string> subjects;
    std::vector<std::string> properties;
};

// Caller must hold CoreSuspender. Missing/unloaded image chunks yield a valid partial model.
ArtworkModel capture_artwork_model(int32_t art_id, int16_t art_sub_id);

// Pure formatter. It intentionally provides useful partial prose, not byte-identical native prose.
std::string compose_artwork_prose(const ArtworkModel& art,
                                  const std::string& medium,
                                  const std::string& quality,
                                  const std::string& surface,
                                  const std::string& artist);

} // namespace dfcapture
