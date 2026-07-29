// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

#include "frame_delta.h"

#include "image_encoder.h"
#include "json_util.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace dfcapture {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kBlockSize = 32;
constexpr int kMaxRectangles = 48;
constexpr double kKeyframeChangedRatio = 0.42;
constexpr auto kKeyframeInterval = std::chrono::seconds(5);
constexpr auto kStateExpiry = std::chrono::seconds(60);
constexpr size_t kMaxPlayerStates = 32;
constexpr size_t kMaxPacketBytes = 64 * 1024 * 1024;

struct DirtyRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::vector<uint8_t> bytes;
};

struct PlayerDeltaState {
    std::mutex mutex;
    CapturedFrame previous;
    Camera previous_camera;
    bool has_previous_camera = false;
    uint64_t sequence = 0;
    Clock::time_point last_keyframe{};
    Clock::time_point last_access = Clock::now();
};

std::mutex g_states_mutex;
std::unordered_map<std::string, std::shared_ptr<PlayerDeltaState>> g_states;

std::shared_ptr<PlayerDeltaState> state_for_player(const std::string& raw_player) {
    const std::string player = raw_player.substr(0, 64);
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(g_states_mutex);
    for (auto it = g_states.begin(); it != g_states.end();) {
        if (now - it->second->last_access > kStateExpiry)
            it = g_states.erase(it);
        else
            ++it;
    }
    if (g_states.size() >= kMaxPlayerStates && !g_states.count(player)) {
        auto oldest = std::min_element(g_states.begin(), g_states.end(),
            [](const auto& a, const auto& b) {
                return a.second->last_access < b.second->last_access;
            });
        if (oldest != g_states.end()) g_states.erase(oldest);
    }
    auto& state = g_states[player];
    if (!state) state = std::make_shared<PlayerDeltaState>();
    state->last_access = now;
    return state;
}

bool block_changed(const CapturedFrame& a, const CapturedFrame& b,
                   int x, int y, int w, int h) {
    const size_t row_bytes = static_cast<size_t>(w) * 4;
    for (int row = 0; row < h; ++row) {
        const size_t offset =
            (static_cast<size_t>(y + row) * a.width + x) * 4;
        if (std::memcmp(a.bgra.data() + offset, b.bgra.data() + offset, row_bytes) != 0)
            return true;
    }
    return false;
}

std::vector<DirtyRect> dirty_rectangles(const CapturedFrame& previous,
                                        const CapturedFrame& current,
                                        uint64_t& changed_area) {
    std::vector<DirtyRect> rectangles;
    const int blocks_x = (current.width + kBlockSize - 1) / kBlockSize;
    const int blocks_y = (current.height + kBlockSize - 1) / kBlockSize;
    for (int by = 0; by < blocks_y; ++by) {
        int bx = 0;
        while (bx < blocks_x) {
            const int x = bx * kBlockSize;
            const int y = by * kBlockSize;
            const int w = std::min(kBlockSize, current.width - x);
            const int h = std::min(kBlockSize, current.height - y);
            if (!block_changed(previous, current, x, y, w, h)) {
                ++bx;
                continue;
            }
            const int run_start = bx;
            ++bx;
            while (bx < blocks_x) {
                const int next_x = bx * kBlockSize;
                const int next_w = std::min(kBlockSize, current.width - next_x);
                if (!block_changed(previous, current, next_x, y, next_w, h)) break;
                ++bx;
            }
            DirtyRect rect;
            rect.x = run_start * kBlockSize;
            rect.y = y;
            rect.w = std::min(current.width, bx * kBlockSize) - rect.x;
            rect.h = h;

            // Merge vertically with an identical horizontal run from the preceding block row.
            if (!rectangles.empty()) {
                auto& last = rectangles.back();
                if (last.x == rect.x && last.w == rect.w &&
                        last.y + last.h == rect.y && last.bytes.empty()) {
                    last.h += rect.h;
                    changed_area += static_cast<uint64_t>(rect.w) * rect.h;
                    continue;
                }
            }
            changed_area += static_cast<uint64_t>(rect.w) * rect.h;
            rectangles.push_back(std::move(rect));
        }
    }
    return rectangles;
}

CapturedFrame extract_rect(const CapturedFrame& source, const DirtyRect& rect) {
    CapturedFrame patch;
    patch.width = rect.w;
    patch.height = rect.h;
    patch.bgra.resize(static_cast<size_t>(rect.w) * rect.h * 4);
    const size_t row_bytes = static_cast<size_t>(rect.w) * 4;
    for (int row = 0; row < rect.h; ++row) {
        const size_t source_offset =
            (static_cast<size_t>(rect.y + row) * source.width + rect.x) * 4;
        const size_t target_offset = static_cast<size_t>(row) * row_bytes;
        std::memcpy(patch.bgra.data() + target_offset,
                    source.bgra.data() + source_offset, row_bytes);
    }
    return patch;
}

bool prepare_scroll_baseline(const CapturedFrame& previous,
                             const CapturedFrame& current,
                             const Camera& previous_camera,
                             const Camera& camera,
                             CapturedFrame& aligned,
                             FrameDeltaResult& result) {
    const auto& a = previous.geometry;
    const auto& b = current.geometry;
    if (!a.valid || !b.valid || previous_camera.z != camera.z ||
            previous_camera.zoom_factor != camera.zoom_factor ||
            a.origin_x != b.origin_x || a.origin_y != b.origin_y ||
            a.zoom_factor != b.zoom_factor ||
            a.viewport_width != b.viewport_width ||
            a.viewport_height != b.viewport_height ||
            b.zoom_factor <= 0 || b.zoom_factor % 4 != 0)
        return false;

    const int tile_pixels = b.zoom_factor / 4;
    const int dx = -(camera.x - previous_camera.x) * tile_pixels;
    const int dy = -(camera.y - previous_camera.y) * tile_pixels;
    if (dx == 0 && dy == 0)
        return false;

    const int x0 = std::clamp(b.origin_x, 0, current.width);
    const int y0 = std::clamp(b.origin_y, 0, current.height);
    const int x1 = std::clamp(
        b.origin_x + b.viewport_width * tile_pixels, 0, current.width);
    const int y1 = std::clamp(
        b.origin_y + b.viewport_height * tile_pixels, 0, current.height);
    const int width = x1 - x0;
    const int height = y1 - y0;
    if (width <= 0 || height <= 0 ||
            std::abs(dx) >= width || std::abs(dy) >= height)
        return false;

    aligned = previous;
    const size_t clear_bytes = static_cast<size_t>(width) * 4;
    for (int y = y0; y < y1; ++y) {
        const size_t offset =
            (static_cast<size_t>(y) * current.width + x0) * 4;
        std::memset(aligned.bgra.data() + offset, 0, clear_bytes);
    }

    // destination = source + (dx, dy). Copy only the overlap; the zeroed exposed strips are
    // guaranteed to differ and become ordinary PNG patches.
    const int destination_x0 = std::max(x0, x0 + dx);
    const int destination_x1 = std::min(x1, x1 + dx);
    const int destination_y0 = std::max(y0, y0 + dy);
    const int destination_y1 = std::min(y1, y1 + dy);
    const size_t copy_bytes =
        static_cast<size_t>(destination_x1 - destination_x0) * 4;
    for (int y = destination_y0; y < destination_y1; ++y) {
        const int source_y = y - dy;
        const int source_x = destination_x0 - dx;
        const size_t source_offset =
            (static_cast<size_t>(source_y) * previous.width + source_x) * 4;
        const size_t destination_offset =
            (static_cast<size_t>(y) * aligned.width + destination_x0) * 4;
        std::memcpy(aligned.bgra.data() + destination_offset,
                    previous.bgra.data() + source_offset, copy_bytes);
    }

    result.has_scroll = true;
    result.scroll_x = x0;
    result.scroll_y = y0;
    result.scroll_width = width;
    result.scroll_height = height;
    result.scroll_dx = dx;
    result.scroll_dy = dy;
    return true;
}

void append_u32_le(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

void assemble_packet(FrameDeltaResult& result, int width, int height,
                     const std::vector<DirtyRect>& rectangles,
                     const char* encoding) {
    size_t payload_size = 0;
    std::ostringstream header;
    header << "{\"protocol\":" << kFrameDeltaProtocol
           << ",\"sequence\":" << result.sequence
           << ",\"baseSequence\":" << result.base_sequence
           << ",\"keyframe\":" << (result.keyframe ? "true" : "false")
           << ",\"width\":" << width << ",\"height\":" << height
           << ",\"changedRatio\":" << result.changed_ratio
           << ",\"scroll\":";
    if (result.has_scroll) {
        header << "{\"x\":" << result.scroll_x
               << ",\"y\":" << result.scroll_y
               << ",\"width\":" << result.scroll_width
               << ",\"height\":" << result.scroll_height
               << ",\"dx\":" << result.scroll_dx
               << ",\"dy\":" << result.scroll_dy << "}";
    } else {
        header << "null";
    }
    header
           << ",\"rectangles\":[";
    for (size_t i = 0; i < rectangles.size(); ++i) {
        const auto& rect = rectangles[i];
        if (i) header << ",";
        header << "{\"x\":" << rect.x << ",\"y\":" << rect.y
               << ",\"width\":" << rect.w << ",\"height\":" << rect.h
               << ",\"encoding\":" << json_string(encoding)
               << ",\"offset\":" << payload_size
               << ",\"length\":" << rect.bytes.size() << "}";
        payload_size += rect.bytes.size();
    }
    header << "],\"payloadLength\":" << payload_size << "}";
    const std::string header_text = header.str();

    result.packet.clear();
    result.packet.reserve(8 + header_text.size() + payload_size);
    result.packet.insert(result.packet.end(), {'D', 'F', 'D', '1'});
    append_u32_le(result.packet, static_cast<uint32_t>(header_text.size()));
    result.packet.insert(result.packet.end(), header_text.begin(), header_text.end());
    for (const auto& rect : rectangles)
        result.packet.insert(result.packet.end(), rect.bytes.begin(), rect.bytes.end());
}

} // namespace

bool capture_camera_delta(const std::string& player, const Camera& camera,
                          uint64_t client_base, bool force_keyframe,
                          FrameDeltaResult& result, std::string* err) {
    const auto total_started = Clock::now();
    auto state = state_for_player(player);
    // Serialize a player's capture through baseline replacement. This prevents two overlapping
    // polls from committing captured frames in reverse order after the global render lock releases.
    std::lock_guard<std::mutex> lock(state->mutex);
    CapturedFrame current;
    if (!capture_camera_frame_timed(camera, current, err, &result.timing))
        return false;
    result.geometry = current.geometry;
    result.timing.width = current.width;
    result.timing.height = current.height;

    result.sequence = state->sequence + 1;
    result.base_sequence = state->sequence;

    const bool dimensions_changed =
        state->previous.width != current.width || state->previous.height != current.height;
    const bool baseline_invalid =
        state->previous.bgra.size() != current.bgra.size() || state->sequence == 0;
    const bool base_mismatch = client_base != state->sequence;
    const bool keyframe_due =
        state->last_keyframe.time_since_epoch().count() == 0 ||
        Clock::now() - state->last_keyframe >= kKeyframeInterval;
    result.keyframe = force_keyframe || dimensions_changed || baseline_invalid ||
                      base_mismatch || keyframe_due;
    if (force_keyframe)
        result.keyframe_reason = "forced";
    else if (dimensions_changed)
        result.keyframe_reason = "dimensions";
    else if (baseline_invalid)
        result.keyframe_reason = "baseline";
    else if (base_mismatch)
        result.keyframe_reason = "resync";
    else if (keyframe_due)
        result.keyframe_reason = "periodic";

    const auto encode_started = Clock::now();
    std::vector<DirtyRect> rectangles;
    if (!result.keyframe) {
        const CapturedFrame* baseline = &state->previous;
        CapturedFrame aligned;
        if (state->has_previous_camera &&
                prepare_scroll_baseline(state->previous, current,
                                        state->previous_camera, camera,
                                        aligned, result))
            baseline = &aligned;
        uint64_t changed_area = 0;
        rectangles = dirty_rectangles(*baseline, current, changed_area);
        const uint64_t total_area =
            static_cast<uint64_t>(current.width) * current.height;
        result.changed_ratio =
            total_area ? static_cast<double>(changed_area) / total_area : 1.0;
        if (rectangles.size() > kMaxRectangles ||
                result.changed_ratio >= kKeyframeChangedRatio) {
            result.keyframe = true;
            result.keyframe_reason = "change-fallback";
            result.has_scroll = false;
            rectangles.clear();
        }
    }

    if (result.keyframe) {
        result.has_scroll = false;
        DirtyRect full;
        full.w = current.width;
        full.h = current.height;
        if (!encode_jpeg(current, full.bytes, DEFAULT_JPEG_QUALITY, err))
            return false;
        rectangles.push_back(std::move(full));
        result.changed_ratio = 1.0;
        result.base_sequence = 0;
        state->last_keyframe = Clock::now();
        assemble_packet(result, current.width, current.height, rectangles, "jpeg");
    } else {
        for (auto& rect : rectangles) {
            CapturedFrame patch = extract_rect(current, rect);
            if (!encode_png(patch, rect.bytes, err))
                return false;
        }
        assemble_packet(result, current.width, current.height, rectangles, "png");
    }

    result.rectangle_count = static_cast<int>(rectangles.size());
    if (result.packet.size() > kMaxPacketBytes) {
        if (err) *err = "delta packet exceeded the 64 MiB safety limit";
        return false;
    }
    result.timing.encode_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - encode_started).count();
    result.timing.total_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - total_started).count();

    state->previous = std::move(current);
    state->previous_camera = camera;
    state->has_previous_camera = true;
    state->sequence = result.sequence;
    return true;
}

void reset_frame_delta_states() {
    std::lock_guard<std::mutex> lock(g_states_mutex);
    g_states.clear();
}

} // namespace dfcapture
