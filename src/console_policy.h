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

// DFHack command-console containment policy.
//
// The host must explicitly enable the browser console. Once enabled, every connected player is
// subject to the same server-side blocklist; capture lifecycle, save, shutdown, and other unsafe
// commands remain available only from the host's local DFHack console.
//
// The decision logic is header-only and DF/httplib-free so policy tests call the same
// command_denied() implementation used by the HTTP route.
//
// The HTTP handler checks this policy before entering Lua, and the native execution bridge checks
// the same table again before running a command.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace dfcapture {
namespace console {

// A single deny rule. `Prefix` matches when the command head starts with `token` (namespace deny,
// e.g. "gui/"); `Exact` matches when the head equals `token`. Matching is case-insensitive so
// `DIE`/`Gui/foo` cannot slip past. `reason` is returned verbatim to the client.
struct DenyRule {
    enum Kind { Prefix, Exact } kind;
    const char* token;
    const char* reason;
};

// Result of a policy check.
struct Denial {
    bool denied = false;
    std::string reason;   // human-readable, safe to show the client (valid only when denied)
};

// ---- THE BLOCKLIST (named, edit here) --------------------------------------------------------
// Grouped by intent. To loosen or tighten containment, edit THIS table -- it is the single source
// of truth for both enforcement sites. Default posture is deny-what-is-listed; keep it
// conservative (the mandate: deny at minimum the categories below).
inline const std::vector<DenyRule>& deny_table() {
    static const std::vector<DenyRule> kRules = {
        // Plugin lifecycle commands remain local to the host's DFHack console.
        // The plugin also registers a bare `capture` command
        //    plugin_init), which the "capture-" prefix does not match. Keep the bare rule.
        { DenyRule::Prefix, "capture-", "capture-* commands control the server itself and are host-console only" },
        { DenyRule::Exact,  "capture",  "capture commands control the server itself and are host-console only" },

        // 2. STOP-THE-WORLD / STOP-THE-SERVER -- anything that halts DF, the plugin, or writes/ends
        //    the save.
        { DenyRule::Exact, "die",         "would kill the Dwarf Fortress process" },
        { DenyRule::Exact, "kill-lua",    "would tear down the running Lua state (can crash DF/the plugin)" },
        { DenyRule::Exact, "quicksave",   "save/shutdown commands are disabled in the browser console" },
        { DenyRule::Exact, "save",        "save/shutdown commands are disabled in the browser console" },
        { DenyRule::Exact, "quit",        "would exit Dwarf Fortress" },
        { DenyRule::Exact, "quit!",       "would exit Dwarf Fortress" },
        { DenyRule::Exact, "forcequit",   "would force-exit Dwarf Fortress" },
        //    Plugin lifecycle verbs can stop the server or load arbitrary side-effecting code.
        //    `enable`/`load`/`plug`/`reload`/`restart`/`script` can load or run
        //    arbitrary side-effecting code. Deny the verbs outright (deny-at-minimum > selective).
        { DenyRule::Exact, "disable",     "plugin/feature lifecycle commands are disabled in the browser console" },
        { DenyRule::Exact, "enable",      "plugin/feature lifecycle commands are disabled in the browser console" },
        { DenyRule::Exact, "load",        "plugin-load commands are disabled in the browser console" },
        { DenyRule::Exact, "unload",      "plugin-unload commands are disabled in the browser console" },
        { DenyRule::Exact, "plug",        "plugin management is disabled in the browser console" },
        { DenyRule::Exact, "reload",      "reload commands are disabled in the browser console" },
        { DenyRule::Exact, "restart",     "restart commands are disabled in the browser console" },
        { DenyRule::Exact, "script",      "running arbitrary script files is disabled in the browser console" },
        { DenyRule::Exact, "sc-script",   "running arbitrary script files is disabled in the browser console" },

        // 3. ARBITRARY CODE -- a raw lua/eval one-liner is unbounded RCE and can be arbitrarily slow
        //    under the core lock. (`:lua` is the colon-prefixed form.)
        { DenyRule::Exact, "lua",         "arbitrary Lua execution is disabled in the browser console" },
        { DenyRule::Prefix, ":lua",       "arbitrary Lua execution is disabled in the browser console" },
        { DenyRule::Exact, "eval",        "arbitrary evaluation is disabled in the browser console" },

        // 4. INTERACTIVE / SCREEN-PUSHING -- gui/* scripts (incl. gui/launcher itself) expect a
        //    native keyboard/viewscreen the headless capture server does not have; they hang holding
        //    the core lock or shove a modal into every player's frame. Deny the whole namespace.
        { DenyRule::Prefix, "gui/",       "interactive gui/ scripts need a native screen the server does not have" },
        { DenyRule::Exact, "command-prompt", "interactive prompt commands are not usable headless" },

        // 5. DEVELOPER TOOLS -- devel/* are unsupported internals; many are heavy whole-state walks.
        { DenyRule::Prefix, "devel/",     "devel/ internals are disabled in the browser console" },

        // 6. WHOLE-WORLD SCANS / MASS EFFECTS -- reliably breach the 1.5s busy threshold under the
        //    core lock (freezing every player, and a CoreSuspender command CANNOT be interrupted),
        //    or mass-mutate the fort. `prospect all` is handled specially below (bare `prospect` on
        //    the current tile is cheap and stays allowed).
        { DenyRule::Exact, "reveal",      "map-wide reveal freezes the fort and is disabled" },
        { DenyRule::Exact, "unreveal",    "map-wide unreveal freezes the fort and is disabled" },
        { DenyRule::Exact, "exterminate", "mass-kill commands are disabled in the browser console" },
        { DenyRule::Exact, "extinguish",  "mass-effect commands are disabled in the browser console" },

        // 7. INDIRECTION -- DFHack expands these BEFORE dispatch, so each is a way to invoke a denied
        //    command under a name this table never sees (a host-defined alias pointing at capture-*,
        //    a keybinding, a repeat/multicmd wrapper). Denying the wrappers is what makes rule 1
        // Keep aliases within the same containment boundary as capture-* commands.
        { DenyRule::Exact, "alias",       "alias can invoke a denied command under another name" },
        { DenyRule::Exact, "keybinding",  "keybinding can bind a denied command" },
        { DenyRule::Exact, "repeat",      "repeat can schedule a denied command" },
        { DenyRule::Exact, "multicmd",    "multicmd can chain a denied command" },
    };
    return kRules;
}

// ---- helpers (pure) --------------------------------------------------------------------------

inline char to_lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::string lower_ascii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(to_lower_ascii(c));
    return out;
}

// Split a raw command line into whitespace-delimited tokens (space/tab). No quoting semantics --
// the deny check only needs the head and a coarse arg scan (e.g. "all" for prospect), and treating
// a quoted arg as one-or-more tokens can only ADD matches, never hide one.
inline std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// The leading command token (first word), or "" for a blank line.
inline std::string command_head(const std::string& line) {
    auto toks = tokenize(line);
    return toks.empty() ? std::string() : toks.front();
}

// THE GATE. Returns {denied,reason}; applies to EVERY caller (no host parameter by construction).
// A blank command is denied (nothing to run). Matching is case-insensitive on the head.
inline Denial command_denied(const std::string& line) {
    Denial d;
    auto toks = tokenize(line);
    if (toks.empty()) {
        d.denied = true;
        d.reason = "empty command";
        return d;
    }
    const std::string head = lower_ascii(toks.front());

    for (const auto& rule : deny_table()) {
        const std::string tok = lower_ascii(rule.token);
        bool hit = (rule.kind == DenyRule::Exact)
            ? (head == tok)
            : (head.size() >= tok.size() && head.compare(0, tok.size(), tok) == 0);
        if (hit) {
            d.denied = true;
            d.reason = rule.reason;
            return d;
        }
    }

    // Special case: `prospect all` (and `prospect ... all`) is a whole-embark scan that freezes the
    // fort; bare `prospect` (current tile) is cheap and stays allowed. Checked here rather than as a
    // head rule so the useful form survives.
    if (head == "prospect") {
        for (size_t i = 1; i < toks.size(); ++i) {
            if (lower_ascii(toks[i]) == "all") {
                d.denied = true;
                d.reason = "`prospect all` scans the whole embark and freezes the fort";
                return d;
            }
        }
    }

    return d;   // allowed
}

}  // namespace console
}  // namespace dfcapture
