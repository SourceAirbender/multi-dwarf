// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>
#include <string>

namespace dfcapture {

enum class AttribKind { Building, Order, Stockpile, Zone };

void attrib_note_world(const std::string& save_dir);
void attrib_stamp(AttribKind kind, int32_t id, const std::string& player);
void attrib_record(AttribKind kind, int32_t id, const std::string& player,
                   const std::string& action);
bool attrib_lookup(AttribKind kind, int32_t id, std::string& player);
std::string attrib_json();

} // namespace dfcapture
