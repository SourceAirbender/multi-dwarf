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

#include "diplo.h"

#include "json_util.h"
#include "save_barrier.h"
#include "sdl_capture.h"
#include "ws_compat.h"

#include "Core.h"
#include "DataDefs.h"
#include "modules/Translation.h"
#include "modules/Units.h"

#include "df/dipscript_popup.h"
#include "df/diplomacy_interfacest.h"
#include "df/entity_position.h"
#include "df/entity_sell_category.h"
#include "df/entity_sell_requests.h"
#include "df/gamest.h"
#include "df/global_objects.h"
#include "df/historical_entity.h"
#include "df/historical_figure.h"
#include "df/main_interface.h"
#include "df/markup_text_boxst.h"
#include "df/markup_text_wordst.h"
#include "df/meeting_diplomat_info.h"
#include "df/meeting_topic.h"
#include "df/plotinfost.h"
#include "df/unit.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace dfcapture {
namespace {

// Diplomacy and petitions structure references:
//   * df.plotinfo.xml:856-873  plotinfost: `petitions` (original unapproved_agreement_id --
//       the vector fort_admin.cpp's accept/deny already mutate), `dipscript_popups` (original
//       `meetingmoment`, vector<dipscript_popup*>, "cause viewscreen_meetingst to pop up"),
//       `meeting_requests` (original noblequeue).
//   * df.d_interface.xml:807-850  diplomacy_interfacest ("main_interface.diplomacy"):
//       {open, actor, target, text (markup_text_boxst), selecting_land_holder_position,
//        taking_requests, land_holder_* vectors, taking_requests_tablist (entity_sell_category
//        values), taking_requests_selected_tab, dipev -> meeting_diplomat_info}.
//   * df.markup_text_box.xml  markup_text_boxst {word: vector<markup_text_wordst*>}; each word
//       carries {str, red, green, blue, flags{NEW_LINE, BLANK_LINE, INDENT}} -- the NATIVE
// text layout and coloring of the meeting dialog: white narration line plus
//       colored speech line), mirrored verbatim.
//   * df.diplomacy.xml:14-47  meeting_diplomat_info {topic_list: vector<meeting_topic>,
//       sell_requests -> entity_sell_requests, ...}.
//   * df.civagreement.xml:25-29  entity_sell_requests {priority: vector<int8_t>[per
//       entity_sell_category]} -- the export-agreement priorities.
//       DFHack scripts/internal/caravan/tradeagreement.lua writes priority[cat][i] = 0/4 on
//       the live Requests screen (its "Select all/none" overlay); native's Done commits them.
//   * DFHack library/modules/World.cpp ReadPauseState(): `game->main_interface.diplomacy.open`
//       is in the sim-blocking list -- while the meeting is up, unpause cannot resume.
//
// WHAT IS DELIBERATELY NOT DONE: advancing the meeting ("Okay"), picking the land holder, or
// committing/leaving the Requests screen. Those run through DF's dipscript VM
// (plotinfo->dipscripts script_stepst vmethods + mm->flags close_screen/new_screen edges) and
// their exact native transitions are not exposed safely. Guessing risks corrupting persistent
// agreement state, so the wire reports "advanceHostNative":true.

long long steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Defensive caps: a meeting dialog is a small text surface, not a bulk transport.
constexpr size_t kMaxWords = 600;        // dialogue words per frame
constexpr size_t kMaxWordChars = 120;    // chars per word
constexpr size_t kMaxCandidates = 60;    // land-holder candidate rows
constexpr size_t kMaxPositions = 8;      // offered land-holder positions
constexpr size_t kMaxPrioritiesPerTab = 512;  // export-agreement rows per category
constexpr size_t kMaxTopics = 16;        // meeting topic keys

// ---- plain-data snapshot (sampled under a suspender, serialized outside) -----------------------
struct DiploWord {
    std::string text;
    uint8_t r = 255, g = 255, b = 255;
    bool new_line = false, blank_line = false, indent = false;
};

struct DiploCandidate {
    int32_t hfid = -1;
    std::string name;
};

struct DiploTab {
    int cat = -1;
    std::string name;
    std::vector<int8_t> priorities;
    bool truncated = false;
};

struct DiploSnapshot {
    int petitions_pending = 0;
    int meetings_queued = 0;
    bool open = false;
    std::string mode;                       // "text" | "landHolder" | "requests"
    std::string actor, target;
    std::vector<DiploWord> words;
    bool words_truncated = false;
    std::vector<std::string> positions;     // offered land-holder position names
    std::vector<DiploCandidate> candidates; // land_holder_avail_hfid resolved
    std::vector<DiploTab> tabs;             // taking_requests tablist
    int selected_tab = -1;
    std::vector<std::string> topics;        // dipev->topic_list enum keys
};

// ---- module state (guarded by g_diplo_mutex) ----------------------------------------------------
std::mutex g_diplo_mutex;
uint64_t g_seq = 0;
std::string g_last_body;                   // serialized state MINUS seq/by (change detection)
std::set<std::string> g_synced;            // late-join sync bookkeeping (vote.cpp pattern)
std::atomic<bool> g_meeting_open{false};   // diplo_meeting_open() mirror for arbiter + /diag

std::string histfig_display_name(int32_t hf_id) {
    if (hf_id < 0)
        return "";
    auto hf = df::historical_figure::find(hf_id);
    if (!hf)
        return "";
    std::string name = DFHack::Translation::translateName(&hf->name, true);
    return name.empty() ? ("Figure " + std::to_string(hf_id)) : name;
}

// The offered position lives on the fort's own group entity (child) -- fall back to the parent
// civ so a structure surprise degrades to a missing name, never a crash (vote.cpp pattern).
df::entity_position* find_entity_position(df::historical_entity* ent, int32_t pos_id) {
    if (!ent)
        return nullptr;
    for (auto pos : ent->positions.own)
        if (pos && pos->id == pos_id)
            return pos;
    return nullptr;
}

// MUST be called under a (Conditional)CoreSuspender -- plotinfo/game are sim-owned heap.
// Null-guards everything (vote.cpp / native_popup.cpp sampling discipline).
DiploSnapshot sample_native_suspended() {
    DiploSnapshot s;
    auto plotinfo = df::global::plotinfo;
    if (plotinfo) {
        s.petitions_pending = static_cast<int>(plotinfo->petitions.size());
        s.meetings_queued = static_cast<int>(plotinfo->dipscript_popups.size());
    }
    auto game = df::global::game;
    if (!game)
        return s;
    auto& dip = game->main_interface.diplomacy;
    if (!dip.open)
        return s;
    s.open = true;
    s.mode = dip.selecting_land_holder_position ? "landHolder"
           : (dip.taking_requests ? "requests" : "text");
    if (dip.actor)
        s.actor = DFHack::Units::getReadableName(dip.actor);
    if (dip.target)
        s.target = DFHack::Units::getReadableName(dip.target);

    // Dialogue text: the native word stream with per-word color + layout flags.
    for (auto word : dip.text.word) {
        if (!word)
            continue;
        if (s.words.size() >= kMaxWords) { s.words_truncated = true; break; }
        DiploWord w;
        w.text = word->str;
        if (w.text.size() > kMaxWordChars) w.text.resize(kMaxWordChars);
        w.r = word->red; w.g = word->green; w.b = word->blue;
        w.new_line = word->flags.bits.NEW_LINE;
        w.blank_line = word->flags.bits.BLANK_LINE;
        w.indent = word->flags.bits.INDENT;
        s.words.push_back(std::move(w));
    }

    if (dip.selecting_land_holder_position) {
        for (int32_t pos_id : dip.land_holder_pos_id) {
            if (s.positions.size() >= kMaxPositions) break;
            df::entity_position* pos = find_entity_position(dip.land_holder_child_civ, pos_id);
            if (!pos) pos = find_entity_position(dip.land_holder_parent_civ, pos_id);
            if (!pos) continue;
            std::string name = !pos->name[0].empty() ? pos->name[0]
                             : (!pos->name_male[0].empty() ? pos->name_male[0] : "");
            if (!name.empty())
                s.positions.push_back(name);
        }
        for (int32_t hfid : dip.land_holder_avail_hfid) {
            if (s.candidates.size() >= kMaxCandidates) break;
            DiploCandidate c;
            c.hfid = hfid;
            c.name = histfig_display_name(hfid);
            s.candidates.push_back(std::move(c));
        }
    }

    if (dip.taking_requests && dip.dipev && dip.dipev->sell_requests) {
        auto* reqs = dip.dipev->sell_requests;
        constexpr int kCatCount =
            static_cast<int>(df::enum_traits<df::entity_sell_category>::last_item_value) + 1;
        for (int16_t cat : dip.taking_requests_tablist) {
            if (cat < 0 || cat >= kCatCount)
                continue;
            DiploTab tab;
            tab.cat = cat;
            const char* key = DFHack::enum_item_key_str(static_cast<df::entity_sell_category>(cat));
            tab.name = key ? key : std::to_string(cat);
            const auto& prio = reqs->priority[cat];
            size_t n = prio.size();
            if (n > kMaxPrioritiesPerTab) { n = kMaxPrioritiesPerTab; tab.truncated = true; }
            tab.priorities.assign(prio.begin(), prio.begin() + n);
            s.tabs.push_back(std::move(tab));
        }
        s.selected_tab = dip.taking_requests_selected_tab;
    }

    if (dip.dipev) {
        for (auto topic : dip.dipev->topic_list) {
            if (s.topics.size() >= kMaxTopics) break;
            const char* key = DFHack::enum_item_key_str(topic);
            s.topics.push_back(key ? key : std::to_string(static_cast<int>(topic)));
        }
    }
    return s;
}

// ---- serialization -------------------------------------------------------------------------------
// Body WITHOUT the seq/by envelope, so the same string doubles as the change detector.
std::string color_hex(uint8_t r, uint8_t g, uint8_t b) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    return buf;
}

std::string state_body_json(const DiploSnapshot& s) {
    std::ostringstream body;
    body << "\"petitionsPending\":" << s.petitions_pending
         << ",\"meetingsQueued\":" << s.meetings_queued
         << ",\"open\":" << (s.open ? "true" : "false")
         << ",\"meeting\":";
    if (!s.open) {
        body << "null";
        return body.str();
    }
    body << "{\"mode\":" << json_string(s.mode)
         << ",\"actor\":" << json_string(s.actor)
         << ",\"target\":" << json_string(s.target)
         // v1: Okay / land-holder pick / Requests Done stay host-native (see module banner).
         << ",\"advanceHostNative\":true"
         << ",\"words\":[";
    for (size_t i = 0; i < s.words.size(); ++i) {
        const auto& w = s.words[i];
        if (i) body << ",";
        body << "{\"t\":" << json_string(w.text);
        if (w.r != 255 || w.g != 255 || w.b != 255)
            body << ",\"c\":" << json_string(color_hex(w.r, w.g, w.b));
        if (w.new_line) body << ",\"nl\":1";
        if (w.blank_line) body << ",\"blank\":1";
        if (w.indent) body << ",\"ind\":1";
        body << "}";
    }
    body << "]";
    if (s.words_truncated)
        body << ",\"wordsTruncated\":true";
    if (s.mode == "landHolder") {
        body << ",\"landHolder\":{\"positions\":[";
        for (size_t i = 0; i < s.positions.size(); ++i) {
            if (i) body << ",";
            body << json_string(s.positions[i]);
        }
        body << "],\"candidates\":[";
        for (size_t i = 0; i < s.candidates.size(); ++i) {
            if (i) body << ",";
            body << "{\"hfid\":" << s.candidates[i].hfid
                 << ",\"name\":" << json_string(s.candidates[i].name) << "}";
        }
        body << "]}";
    }
    if (s.mode == "requests") {
        body << ",\"requests\":{\"selectedTab\":" << s.selected_tab << ",\"tabs\":[";
        for (size_t i = 0; i < s.tabs.size(); ++i) {
            const auto& tab = s.tabs[i];
            if (i) body << ",";
            body << "{\"cat\":" << tab.cat
                 << ",\"name\":" << json_string(tab.name)
                 << ",\"priorities\":[";
            for (size_t j = 0; j < tab.priorities.size(); ++j) {
                if (j) body << ",";
                body << static_cast<int>(tab.priorities[j]);
            }
            body << "]";
            if (tab.truncated) body << ",\"truncated\":true";
            body << "}";
        }
        body << "]}";
    }
    body << ",\"topics\":[";
    for (size_t i = 0; i < s.topics.size(); ++i) {
        if (i) body << ",";
        body << json_string(s.topics[i]);
    }
    body << "]}";
    return body.str();
}

// {"type":"diplo",...} frame / GET /diplo body from a serialized state body.
std::string frame_json(uint64_t seq, const std::string& body, bool as_ws_frame,
                       const std::string& by) {
    std::ostringstream out;
    out << "{";
    if (as_ws_frame) out << "\"type\":\"diplo\",";
    out << "\"seq\":" << seq;
    if (!by.empty()) out << ",\"by\":" << json_string(by);
    out << "," << body << "}";
    return out.str();
}

// Push a frame to every connected player and mark them synced (native_popup.cpp
// broadcast_state: frame built under the module mutex, sent OUTSIDE it).
void broadcast_state(const std::string& frame) {
    auto connected = ws_connected_players();
    for (const auto& p : connected)
        broadcast_to_player(p, frame);
    std::lock_guard<std::mutex> lock(g_diplo_mutex);
    g_synced.clear();
    g_synced.insert(connected.begin(), connected.end());
}

void diplo_json_error(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content("{\"ok\":false,\"error\":" + json_string(message) + "}\n",
                    "application/json; charset=utf-8");
}

} // namespace

// ---- push-loop tick ------------------------------------------------------------------------------

void diplo_push_tick() {
    if (save_barrier_active())
        return;

    // <=1 Hz cadence for BOTH sampling and late-join sync (vote/popup posture).
    static long long last_pass = 0;
    const long long now = steady_ms();
    if (now - last_pass < 1000)
        return;
    last_pass = now;

    // 1) Sample OUTSIDE g_diplo_mutex (never hold a plugin mutex across a suspender acquire).
    //    ConditionalCoreSuspender skips instantly while the core is blocked on a save -- we
    //    keep the previous mirrored state in that case.
    bool sampled = false;
    DiploSnapshot snap;
    {
        DFHack::ConditionalCoreSuspender suspend;
        if (suspend) {
            snap = sample_native_suspended();
            sampled = true;
        }
    }

    // 2) Detect change under the mutex; broadcast after releasing it.
    std::string frame;
    if (sampled) {
        std::string body = state_body_json(snap);
        std::lock_guard<std::mutex> lock(g_diplo_mutex);
        g_meeting_open.store(snap.open);
        if (body != g_last_body) {
            g_last_body = std::move(body);
            ++g_seq;
            frame = frame_json(g_seq, g_last_body, /*as_ws_frame=*/true, /*by=*/"");
        }
    }
    if (!frame.empty())
        broadcast_state(frame);

    // 3) Late-join sync: once anything has ever been mirrored (seq > 0), a player who has not
    //    seen the CURRENT state gets it -- including the all-clear state, so a reconnecting tab
    //    never keeps a stale plaque. Prune g_synced to the live roster so a reconnect resyncs.
    auto connected = ws_connected_players();
    std::vector<std::string> to_sync;
    std::string sync_frame;
    {
        std::lock_guard<std::mutex> lock(g_diplo_mutex);
        std::set<std::string> live(connected.begin(), connected.end());
        for (auto it = g_synced.begin(); it != g_synced.end();)
            it = live.count(*it) ? std::next(it) : g_synced.erase(it);
        if (g_seq > 0) {
            for (const auto& p : connected)
                if (!g_synced.count(p)) { to_sync.push_back(p); g_synced.insert(p); }
            if (!to_sync.empty())
                sync_frame = frame_json(g_seq, g_last_body, /*as_ws_frame=*/true, /*by=*/"");
        }
    }
    for (const auto& p : to_sync)
        broadcast_to_player(p, sync_frame);
}

bool diplo_meeting_open() {
    return g_meeting_open.load();
}

// ---- routes --------------------------------------------------------------------------------------

void register_diplo_routes(httplib::Server& server) {
    // GET /diplo -> current mirrored state. Mutex-only cache read (no CoreSuspender per
    // request); callers do not need a streaming connection.
    server.Get("/diplo", [](const httplib::Request&, httplib::Response& res) {
        std::string json;
        {
            std::lock_guard<std::mutex> lock(g_diplo_mutex);
            // Before the first sample tick the body is empty -- serve the empty state so the
            // route never emits invalid JSON.
            std::string body = g_last_body.empty()
                ? "\"petitionsPending\":0,\"meetingsQueued\":0,\"open\":false,\"meeting\":null"
                : g_last_body;
            json = frame_json(g_seq, body, /*as_ws_frame=*/false, /*by=*/"");
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json + "\n", "application/json; charset=utf-8");
    });

    // POST /diplo-request-priority?player=&cat=&index=&value=0..4 -> set one export-agreement
    // priority on the OPEN Requests screen. This is the exact write DFHack's own
    // tradeagreement.lua overlay performs (priority[cat][i]); native's Done button commits.
    // Everything else about the meeting is read-only in v1 (see module banner).
    server.Post("/diplo-request-priority", [](const httplib::Request& req,
                                              httplib::Response& res) {
        int cat = -1, index = -1, value = -1;
        if (!query_int(req, "cat", cat) || !query_int(req, "index", index) ||
            !query_int(req, "value", value)) {
            diplo_json_error(res, 400, "cat, index and value are required");
            return;
        }
        if (value < 0 || value > 4) {
            diplo_json_error(res, 400, "value must be 0..4");
            return;
        }
        std::string err;
        bool ok = false;
        {
            // Same lock order as every other DF mutation (capture mutex -> CoreSuspender;
            // fort_admin.cpp run_admin_locked posture). Re-verifies the native state under
            // the suspender (TOCTOU: the meeting may have advanced since the client's frame).
            std::lock_guard<std::recursive_mutex> capture_lock(capture_state_mutex());
            DFHack::CoreSuspender suspend;
            if (save_barrier_active()) {
                diplo_json_error(res, 503, "Dwarf Fortress is saving or unloading");
                return;
            }
            auto game = df::global::game;
            constexpr int kCatCount =
                static_cast<int>(df::enum_traits<df::entity_sell_category>::last_item_value) + 1;
            if (!game || !game->main_interface.diplomacy.open)
                err = "no diplomacy meeting is open";
            else if (!game->main_interface.diplomacy.taking_requests)
                err = "the meeting is not on the requests screen";
            else if (!game->main_interface.diplomacy.dipev ||
                     !game->main_interface.diplomacy.dipev->sell_requests)
                err = "no request data on this meeting";
            else if (cat < 0 || cat >= kCatCount)
                err = "unknown category";
            else {
                auto& prio = game->main_interface.diplomacy.dipev->sell_requests->priority[cat];
                if (index < 0 || static_cast<size_t>(index) >= prio.size())
                    err = "index out of range";
                else {
                    prio[index] = static_cast<int8_t>(value);
                    ok = true;
                }
            }
        }
        if (!ok) {
            diplo_json_error(res, 409, err);
            return;
        }
        // Make the next tick rebroadcast even to the writer (the priorities are part of the
        // state body, so clearing the change detector forces the fresh frame out).
        {
            std::lock_guard<std::mutex> lock(g_diplo_mutex);
            g_last_body.clear();
        }
        res.set_header("Cache-Control", "no-store");
        std::ostringstream out;
        out << "{\"ok\":true,\"cat\":" << cat << ",\"index\":" << index
            << ",\"value\":" << value << "}\n";
        res.set_content(out.str(), "application/json; charset=utf-8");
    });
}

} // namespace dfcapture
