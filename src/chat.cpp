// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, version 3 of the License.
//
// SPDX-License-Identifier: AGPL-3.0-only

#include "chat.h"

#include "httplib.h"
#include "json_util.h"
#include "session_policy.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace dfcapture {
namespace {

constexpr size_t CHAT_HISTORY_LIMIT = 100;
constexpr size_t CHAT_TEXT_LIMIT = 500;
constexpr size_t CHAT_NAME_LIMIT = 32;
constexpr int64_t CHAT_RATE_LIMIT_MS = 550;

struct ChatLine {
    int64_t seq = 0;
    int64_t ts = 0;
    std::string player;
    std::string from;
    std::string text;
};

std::mutex g_chat_mutex;
std::deque<ChatLine> g_chat_lines;
std::unordered_map<std::string, int64_t> g_chat_last_post;
int64_t g_chat_next_seq = 1;

int64_t chat_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool is_utf8_continuation(unsigned char ch) {
    return (ch & 0xc0) == 0x80;
}

void clamp_utf8(std::string& value, size_t limit) {
    if (value.size() <= limit)
        return;
    size_t cut = limit;
    // value[cut] is the first excluded byte. If it is a continuation byte, `cut` split a
    // multi-byte code point, so back up to that code point's leading byte.
    while (cut > 0 && cut < value.size() &&
           is_utf8_continuation(static_cast<unsigned char>(value[cut])))
        --cut;
    value.resize(cut);
}

std::string sanitize_text(const std::string& raw, size_t limit) {
    std::string clean;
    clean.reserve(std::min(raw.size(), limit));
    bool pending_space = false;
    for (unsigned char ch : raw) {
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            pending_space = !clean.empty();
            continue;
        }
        if (ch < 0x20 || ch == 0x7f)
            continue;
        if (pending_space && ch != ' ') {
            clean.push_back(' ');
            pending_space = false;
        }
        clean.push_back(static_cast<char>(ch));
        // Keep enough look-ahead to determine whether the byte boundary split UTF-8, but never
        // let an oversized request make this temporary string grow without bound.
        if (clean.size() > limit + 4)
            break;
    }
    clamp_utf8(clean, limit);
    while (!clean.empty() && clean.front() == ' ')
        clean.erase(clean.begin());
    while (!clean.empty() && clean.back() == ' ')
        clean.pop_back();
    return clean;
}

// json_util converts DF's legacy encoding to UTF-8. Chat text already arrives as UTF-8 from the
// browser, so escape it directly or non-ASCII player names/messages are double-transcoded.
std::string chat_json_string(const std::string& raw) {
    std::ostringstream out;
    out << '"';
    static const char hex[] = "0123456789abcdef";
    for (unsigned char ch : raw) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                out << "\\u00" << hex[(ch >> 4) & 0xf] << hex[ch & 0xf];
            } else {
                out << static_cast<char>(ch);
            }
        }
    }
    out << '"';
    return out.str();
}

std::string line_json(const ChatLine& line) {
    std::ostringstream out;
    out << "{\"seq\":" << line.seq
        << ",\"player\":" << chat_json_string(line.player)
        << ",\"from\":" << chat_json_string(line.from)
        << ",\"text\":" << chat_json_string(line.text)
        << ",\"ts\":" << line.ts << "}";
    return out.str();
}

int64_t query_sequence(const httplib::Request& req) {
    if (!req.has_param("since"))
        return 0;
    const std::string raw = req.get_param_value("since");
    char* end = nullptr;
    const long long value = std::strtoll(raw.c_str(), &end, 10);
    return end && *end == '\0' && value > 0 ? value : 0;
}

std::string history_json(int64_t since) {
    std::lock_guard<std::mutex> lock(g_chat_mutex);
    std::ostringstream out;
    out << "{\"ok\":true,\"latest\":" << (g_chat_next_seq - 1) << ",\"lines\":[";
    bool first = true;
    for (const ChatLine& line : g_chat_lines) {
        if (line.seq <= since)
            continue;
        if (!first)
            out << ",";
        first = false;
        out << line_json(line);
    }
    out << "]}";
    return out.str();
}

} // namespace

void register_chat_routes(httplib::Server& server) {
    server.Get("/chat", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(history_json(query_sequence(req)) + "\n",
                        "application/json; charset=utf-8");
    });

    server.Post("/chat", [](const httplib::Request& req, httplib::Response& res) {
        const std::string player = query_player(req);
        const std::string from =
            sanitize_text(session_display_name(player), CHAT_NAME_LIMIT);
        const std::string text = sanitize_text(
            req.has_param("text") ? req.get_param_value("text") : std::string(),
            CHAT_TEXT_LIMIT);
        if (text.empty()) {
            res.status = 400;
            res.set_header("Cache-Control", "no-store");
            res.set_content("{\"ok\":false,\"error\":\"empty message\"}\n",
                            "application/json; charset=utf-8");
            return;
        }

        ChatLine line;
        const int64_t now = chat_now_ms();
        {
            std::lock_guard<std::mutex> lock(g_chat_mutex);
            if (g_chat_last_post.size() > 256) {
                for (auto it = g_chat_last_post.begin(); it != g_chat_last_post.end();) {
                    if (now - it->second > 60000)
                        it = g_chat_last_post.erase(it);
                    else
                        ++it;
                }
                while (g_chat_last_post.size() > 384)
                    g_chat_last_post.erase(g_chat_last_post.begin());
            }
            const auto last = g_chat_last_post.find(player);
            if (last != g_chat_last_post.end() &&
                now - last->second < CHAT_RATE_LIMIT_MS) {
                const int64_t retry = CHAT_RATE_LIMIT_MS - (now - last->second);
                res.status = 429;
                res.set_header("Cache-Control", "no-store");
                res.set_header("Retry-After", "1");
                res.set_content("{\"ok\":false,\"error\":\"rate limited\",\"retryMs\":" +
                                    std::to_string(retry) + "}\n",
                                "application/json; charset=utf-8");
                return;
            }
            g_chat_last_post[player] = now;
            line.seq = g_chat_next_seq++;
            line.ts = now;
            line.player = player;
            line.from = from.empty() ? player : from;
            line.text = text;
            g_chat_lines.push_back(line);
            while (g_chat_lines.size() > CHAT_HISTORY_LIMIT)
                g_chat_lines.pop_front();
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"ok\":true,\"line\":" + line_json(line) + "}\n",
                        "application/json; charset=utf-8");
    });
}

} // namespace dfcapture
