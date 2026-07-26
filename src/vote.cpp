// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// Copyright (C) 2026 Jake Taplin
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

#include "vote.h"

#include "json_util.h"
#include "save_barrier.h"
#include "ws_compat.h"

#include "Core.h"

#include "df/diplomacy_interfacest.h"
#include "df/entity_position.h"
#include "df/gamest.h"
#include "df/global_objects.h"
#include "df/historical_entity.h"
#include "df/main_interface.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dfcapture {
namespace {

long long wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
long long steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- state (all guarded by g_vote_mutex; attribution.cpp posture: plugin memory, ephemeral) ----
std::mutex g_vote_mutex;

struct VoteRecord {
    int id = 0;
    std::string topic;
    std::string kind;      // "elevation" (auto-detected native offer) | "custom" (manual)
    std::string opened_by; // player name, or "server" for the auto-open
    long long opened_ms = 0;
    bool auto_opened = false;
    // Per player NAME, one entry each, changeable while open (find-and-replace on recast).
    // Insertion order preserved so the who-voted list reads in cast order.
    std::vector<std::pair<std::string, bool>> votes; // name -> yes?
};
bool g_active = false;
VoteRecord g_vote;
int g_next_id = 1;

struct ClosedRecord {
    bool valid = false;
    int id = 0;
    std::string topic, kind, opened_by, closed_by;
    std::string result; // "yes" | "no" | "tie"
    int yes = 0, no = 0;
    long long opened_ms = 0, closed_ms = 0;
    std::vector<std::pair<std::string, bool>> votes;
};
ClosedRecord g_last;

// Native land-holder offer detection (sampled by vote_push_tick under ConditionalCoreSuspender;
// GET /vote only ever reads this CACHE -- request handlers never take CoreSuspender here).
struct Detection {
    bool pending = false;
    std::string topic;                 // "Become a Barony?" etc.
    std::vector<std::string> titles;   // offered position names ("baron", ...)
    long long sampled_ms = 0;          // steady clock; 0 = never sampled
};
Detection g_detect;

// Late-join sync bookkeeping: names already pushed the current state (pruned to the live set).
std::set<std::string> g_synced;

// ---- helpers -----------------------------------------------------------------------------------

bool roster_has(const std::string& player) {
    // This polled build has no authenticated roster. The HTTP surface uses the open-LAN trust
    // model, so the roster check passes open and is_safe_player_id (already applied by the
    // caller) remains the input boundary. Restore a membership test if authenticated sessions
    // are added.
    (void)player;
    return true;
}

// Raw ?player= with EXPLICIT validation -- unlike query_player() there is no "default" fallback:
// vote actions must be attributable to a real, known player name or be rejected.
bool vote_query_player(const httplib::Request& req, std::string& out, std::string* err) {
    if (!req.has_param("player")) { if (err) *err = "missing player"; return false; }
    std::string player = req.get_param_value("player");
    if (!is_safe_player_id(player)) { if (err) *err = "invalid player"; return false; }
    if (!roster_has(player)) { if (err) *err = "unknown player"; return false; }
    out = player;
    return true;
}

std::string sanitize_topic(std::string topic) {
    if (topic.size() > 120) topic.resize(120);
    for (char& ch : topic)
        if (static_cast<unsigned char>(ch) < 0x20) ch = ' ';
    return topic;
}

void tally_locked(const std::vector<std::pair<std::string, bool>>& votes, int& yes, int& no) {
    yes = no = 0;
    for (const auto& v : votes) (v.second ? yes : no)++;
}

void append_votes_json(std::ostringstream& body, const std::vector<std::pair<std::string, bool>>& votes) {
    body << "[";
    bool first = true;
    for (const auto& v : votes) {
        if (!first) body << ",";
        first = false;
        body << "{\"player\":" << json_string(v.first)
             << ",\"choice\":\"" << (v.second ? "yes" : "no") << "\"}";
    }
    body << "]";
}

// Full vote state (shared by GET /vote and the {"type":"vote"} broadcast). Caller holds the mutex.
std::string state_json_locked(bool as_ws_frame) {
    std::ostringstream body;
    body << "{";
    if (as_ws_frame) body << "\"type\":\"vote\",";
    body << "\"seq\":" << (g_active ? g_vote.id : (g_last.valid ? g_last.id : 0));
    if (g_active) {
        int yes = 0, no = 0;
        tally_locked(g_vote.votes, yes, no);
        body << ",\"active\":{\"id\":" << g_vote.id
             << ",\"topic\":" << json_string(g_vote.topic)
             << ",\"kind\":" << json_string(g_vote.kind)
             << ",\"openedBy\":" << json_string(g_vote.opened_by)
             << ",\"openedMs\":" << g_vote.opened_ms
             << ",\"yes\":" << yes << ",\"no\":" << no << ",\"votes\":";
        append_votes_json(body, g_vote.votes);
        body << "}";
    } else {
        body << ",\"active\":null";
    }
    if (g_last.valid) {
        body << ",\"lastResult\":{\"id\":" << g_last.id
             << ",\"topic\":" << json_string(g_last.topic)
             << ",\"kind\":" << json_string(g_last.kind)
             << ",\"openedBy\":" << json_string(g_last.opened_by)
             << ",\"closedBy\":" << json_string(g_last.closed_by)
             << ",\"result\":\"" << g_last.result << "\""
             << ",\"yes\":" << g_last.yes << ",\"no\":" << g_last.no
             << ",\"openedMs\":" << g_last.opened_ms
             << ",\"closedMs\":" << g_last.closed_ms << ",\"votes\":";
        append_votes_json(body, g_last.votes);
        body << "}";
    } else {
        body << ",\"lastResult\":null";
    }
    body << ",\"detection\":{\"pending\":" << (g_detect.pending ? "true" : "false")
         << ",\"topic\":" << json_string(g_detect.topic) << ",\"titles\":[";
    bool first = true;
    for (const auto& t : g_detect.titles) {
        if (!first) body << ",";
        first = false;
        body << json_string(t);
    }
    body << "]}}";
    return body.str();
}

// Push the current state to every connected player (pause_arbiter broadcast_all pattern --
// broadcast_to_player only enqueues on mutexed per-connection queues, safe from any thread).
// Callers pass a frame BUILT under the mutex, then send OUTSIDE it.
void broadcast_state(const std::string& frame) {
    auto connected = ws_connected_players();
    for (const auto& p : connected)
        broadcast_to_player(p, frame);
    std::lock_guard<std::mutex> lock(g_vote_mutex);
    g_synced.clear();
    g_synced.insert(connected.begin(), connected.end());
}

// Caller holds the mutex. Moves the active vote to the closed record with a computed result.
void close_vote_locked(const std::string& closed_by) {
    ClosedRecord rec;
    rec.valid = true;
    rec.id = g_vote.id;
    rec.topic = g_vote.topic;
    rec.kind = g_vote.kind;
    rec.opened_by = g_vote.opened_by;
    rec.closed_by = closed_by;
    tally_locked(g_vote.votes, rec.yes, rec.no);
    rec.result = rec.yes > rec.no ? "yes" : (rec.no > rec.yes ? "no" : "tie");
    rec.opened_ms = g_vote.opened_ms;
    rec.closed_ms = wall_ms();
    rec.votes = g_vote.votes;
    g_last = std::move(rec);
    g_active = false;
    g_vote = VoteRecord();
}

// Caller holds the mutex.
void open_vote_locked(const std::string& topic, const std::string& kind,
                      const std::string& opened_by, bool auto_opened) {
    g_vote = VoteRecord();
    g_vote.id = g_next_id++;
    g_vote.topic = topic;
    g_vote.kind = kind;
    g_vote.opened_by = opened_by;
    g_vote.opened_ms = wall_ms();
    g_vote.auto_opened = auto_opened;
    g_active = true;
}

// ---- detection ----------------------------------------------------------------------------------

// The tier label. entity_position.land_holder: 1=baron, 2=count, 3=duke (df.entity.xml). The
// mountainhome promotion is a monarch decision delivered by letter -- NOT a diplomacy popup --
// so it is not detectable here; the manual /vote-start covers it (stated in the module header).
std::string elevation_topic(int max_rank) {
    switch (max_rank) {
        case 1: return "Become a Barony?";
        case 2: return "Become a County?";
        case 3: return "Become a Duchy?";
        default: return "Elevate the fortress?";
    }
}

df::entity_position* find_entity_position(df::historical_entity* ent, int32_t pos_id) {
    if (!ent)
        return nullptr;
    for (auto pos : ent->positions.own)
        if (pos && pos->id == pos_id)
            return pos;
    return nullptr;
}

// Read the native diplomacy popup. MUST be called under a (Conditional)CoreSuspender -- game /
// historical entities are sim-owned. Null-guards everything (petitions-walk discipline).
Detection sample_native_offer_suspended() {
    Detection fresh;
    fresh.sampled_ms = steady_ms();
    auto game = df::global::game;
    if (!game)
        return fresh;
    auto& dip = game->main_interface.diplomacy;
    if (!dip.open || !dip.selecting_land_holder_position)
        return fresh;
    fresh.pending = true;
    int max_rank = 0;
    for (int32_t pos_id : dip.land_holder_pos_id) {
        // The offered position lives on the fort's own group entity (child) -- fall back to the
        // parent civ so a structure surprise degrades to a generic topic, never a crash.
        df::entity_position* pos = find_entity_position(dip.land_holder_child_civ, pos_id);
        if (!pos) pos = find_entity_position(dip.land_holder_parent_civ, pos_id);
        if (!pos)
            continue;
        std::string name = !pos->name[0].empty() ? pos->name[0]
                         : (!pos->name_male[0].empty() ? pos->name_male[0] : "");
        if (!name.empty())
            fresh.titles.push_back(name);
        if (pos->land_holder > max_rank)
            max_rank = pos->land_holder;
    }
    fresh.topic = elevation_topic(max_rank);
    return fresh;
}

} // namespace

// ---- push-loop tick ------------------------------------------------------------------------------

void vote_push_tick() {
    if (save_barrier_active())
        return;

    // <=1 Hz cadence for BOTH detection sampling and late-join sync; the push loop runs ~30 Hz.
    static long long last_pass = 0;
    const long long now = steady_ms();
    if (now - last_pass < 1000)
        return;
    last_pass = now;

    // 1) Sample the native popup OUTSIDE g_vote_mutex (never hold a plugin mutex across a
    //    suspender acquire). ConditionalCoreSuspender skips instantly while the core is blocked
    //    on a save -- we keep the previous detection in that case.
    bool sampled = false;
    Detection fresh;
    {
        DFHack::ConditionalCoreSuspender suspend;
        if (suspend) {
            fresh = sample_native_offer_suspended();
            sampled = true;
        }
    }

    // 2) Apply edges + build any broadcast frame under the mutex; send after releasing it.
    std::string frame;
    {
        std::lock_guard<std::mutex> lock(g_vote_mutex);
        if (sampled) {
            const bool was_pending = g_detect.pending;
            g_detect = fresh;
            if (fresh.pending && !was_pending && !g_active) {
                // Rising edge: the game is asking RIGHT NOW -- open the advisory vote.
                open_vote_locked(fresh.topic, "elevation", "server", /*auto_opened=*/true);
                frame = state_json_locked(/*as_ws_frame=*/true);
            } else if (!fresh.pending && was_pending && g_active && g_vote.auto_opened) {
                // Falling edge: the native popup was answered/dismissed at the keyboard --
                // the decision window is over, close with whatever tally stands.
                close_vote_locked("server");
                frame = state_json_locked(/*as_ws_frame=*/true);
            }
        }
    }
    if (!frame.empty())
        broadcast_state(frame);

    // 3) Late-join sync: push current state to connected players who have not seen it. Only
    //    when there is something to show; prune g_synced to the live set so a reconnect resyncs.
    auto connected = ws_connected_players();
    std::vector<std::string> to_sync;
    std::string sync_frame;
    {
        std::lock_guard<std::mutex> lock(g_vote_mutex);
        std::set<std::string> live(connected.begin(), connected.end());
        for (auto it = g_synced.begin(); it != g_synced.end();)
            it = live.count(*it) ? std::next(it) : g_synced.erase(it);
        if (g_active || g_last.valid) {
            for (const auto& p : connected)
                if (!g_synced.count(p)) { to_sync.push_back(p); g_synced.insert(p); }
            if (!to_sync.empty())
                sync_frame = state_json_locked(/*as_ws_frame=*/true);
        }
    }
    for (const auto& p : to_sync)
        broadcast_to_player(p, sync_frame);
}

// ---- routes --------------------------------------------------------------------------------------

namespace {

void set_no_store_json(httplib::Response& res, const std::string& json) {
    res.set_header("Cache-Control", "no-store");
    res.set_content(json, "application/json; charset=utf-8");
}

void vote_json_error(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content("{\"ok\":false,\"error\":" + json_string(message) + "}\n",
                    "application/json; charset=utf-8");
}

} // namespace

void register_vote_routes(httplib::Server& server) {
    // GET /vote -> full state + cached detection. Mutex-only (no CoreSuspender per request).
    server.Get("/vote", [](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        std::string json;
        {
            std::lock_guard<std::mutex> lock(g_vote_mutex);
            json = state_json_locked(/*as_ws_frame=*/false);
        }
        set_no_store_json(res, json + "\n");
    });

    // POST /vote-start?player=&topic= -> open a manual vote. Topic optional: defaults to the
    // detected native offer's topic when one is pending, else a generic elevation question.
    auto start_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player, err;
        if (!vote_query_player(req, player, &err)) { vote_json_error(res, 400, err); return; }
        std::string topic = req.has_param("topic")
            ? sanitize_topic(req.get_param_value("topic")) : "";
        std::string frame;
        {
            std::lock_guard<std::mutex> lock(g_vote_mutex);
            if (g_active) { vote_json_error(res, 409, "a vote is already open"); return; }
            std::string kind = "custom";
            if (topic.empty()) {
                if (g_detect.pending) { topic = g_detect.topic; kind = "elevation"; }
                else topic = "Elevate the fortress?";
            }
            open_vote_locked(topic, kind, player, /*auto_opened=*/false);
            frame = state_json_locked(/*as_ws_frame=*/true);
        }
        broadcast_state(frame);
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/vote-start", start_handler);
    server.Post("/vote-start", start_handler);

    // POST /vote-cast?player=&choice=yes|no -> one vote per player NAME, changeable while open.
    auto cast_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player, err;
        if (!vote_query_player(req, player, &err)) { vote_json_error(res, 400, err); return; }
        std::string choice = req.has_param("choice") ? req.get_param_value("choice") : "";
        if (choice != "yes" && choice != "no") {
            vote_json_error(res, 400, "choice must be yes or no");
            return;
        }
        const bool yes = choice == "yes";
        std::string frame;
        {
            std::lock_guard<std::mutex> lock(g_vote_mutex);
            if (!g_active) { vote_json_error(res, 400, "no vote is open"); return; }
            auto it = std::find_if(g_vote.votes.begin(), g_vote.votes.end(),
                [&](const std::pair<std::string, bool>& v) { return v.first == player; });
            if (it != g_vote.votes.end()) it->second = yes;      // change of mind, same one slot
            else g_vote.votes.emplace_back(player, yes);
            frame = state_json_locked(/*as_ws_frame=*/true);
        }
        broadcast_state(frame);
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/vote-cast", cast_handler);
    server.Post("/vote-cast", cast_handler);

    // POST /vote-close?player= -> close the open vote (any known player; small-crew trust model,
    // same posture as every other fort mutation route).
    auto close_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string player, err;
        if (!vote_query_player(req, player, &err)) { vote_json_error(res, 400, err); return; }
        std::string frame;
        {
            std::lock_guard<std::mutex> lock(g_vote_mutex);
            if (!g_active) { vote_json_error(res, 400, "no vote is open"); return; }
            close_vote_locked(player);
            frame = state_json_locked(/*as_ws_frame=*/true);
        }
        broadcast_state(frame);
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/vote-close", close_handler);
    server.Post("/vote-close", close_handler);
}

} // namespace dfcapture
