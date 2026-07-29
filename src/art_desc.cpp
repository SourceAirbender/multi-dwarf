// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#include "art_desc.h"

#include "modules/Translation.h"

#include "df/art_image.h"
#include "df/art_image_chunk.h"
#include "df/art_image_element.h"
#include "df/art_image_property.h"
#include "df/global_objects.h"
#include "df/world.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace dfcapture {
namespace {

df::art_image* loaded_art_image(int32_t art_id, int16_t art_sub_id) {
    auto world = df::global::world;
    if (!world || art_id < 0 || art_sub_id < 0 || art_sub_id >= 500)
        return nullptr;
    for (auto chunk : world->art_image_chunks.all) {
        if (chunk && chunk->id == art_id)
            return chunk->images[art_sub_id].art_image;
    }
    return nullptr;
}

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
        [](unsigned char ch) { return std::isspace(ch); });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
        [](unsigned char ch) { return std::isspace(ch); }).base();
    if (first >= last)
        return {};
    value = std::string(first, last);
    while (!value.empty() && (value.back() == '.' || value.back() == ','))
        value.pop_back();
    return value;
}

std::string lower_first(std::string value) {
    if (!value.empty())
        value[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(value[0])));
    return value;
}

std::string join_natural(const std::vector<std::string>& values) {
    if (values.empty()) return {};
    if (values.size() == 1) return values.front();
    if (values.size() == 2) return values[0] + " and " + values[1];
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << (i + 1 == values.size() ? ", and " : ", ");
        out << values[i];
    }
    return out.str();
}

} // namespace

ArtworkModel capture_artwork_model(int32_t art_id, int16_t art_sub_id) {
    ArtworkModel model;
    model.art_id = art_id;
    model.art_sub_id = art_sub_id;
    auto image = loaded_art_image(art_id, art_sub_id);
    if (!image)
        return model;

    model.name = trim(DFHack::Translation::translateName(&image->name, true));
    for (auto element : image->elements) {
        if (!element) continue;
        std::string text;
        element->getName(&text, true, true, true);
        text = trim(text);
        if (!text.empty())
            model.subjects.push_back(lower_first(std::move(text)));
    }
    for (auto property : image->properties) {
        if (!property) continue;
        std::string text;
        property->getName(&text, image, true, true);
        text = trim(text);
        if (!text.empty())
            model.properties.push_back(std::move(text));
    }
    return model;
}

std::string compose_artwork_prose(const ArtworkModel& art,
                                  const std::string& medium,
                                  const std::string& quality,
                                  const std::string& surface,
                                  const std::string& artist) {
    std::ostringstream out;
    out << "This is ";
    if (!quality.empty()) out << lower_first(quality) << " ";
    out << (medium.empty() ? "artwork" : medium);
    if (!surface.empty()) out << " on the " << surface;
    if (!artist.empty()) out << " by " << artist;
    out << ".";
    if (!art.name.empty())
        out << " It is entitled \"" << art.name << "\".";
    if (!art.subjects.empty())
        out << " It depicts " << join_natural(art.subjects) << ".";
    if (!art.properties.empty())
        out << " " << join_natural(art.properties) << ".";
    return out.str();
}

} // namespace dfcapture
