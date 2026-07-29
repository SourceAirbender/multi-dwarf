// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
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

  // ---- DFHack console panel: /console/commands + /console/run + /console-config ----------------
  // The catalog (helpdb's command list + the live server deny table) is fetched ONCE per panel
  // open; search-as-you-type filters it offline. Only Run touches the server. The deny table here
  // is DISPLAY ONLY -- the server re-checks every run against the same table.
  let conCatalog = null;     // [{name, short}]
  let conDeny = [];          // [{kind, token, reason}]
  let conEnabled = false;    // host setting (dfhack_console)
  let conIsHost = false;     // is this tab the host's own?
  let conLog = [];           // {cmd, output, status, err} entries, newest last
  let conHistory = [];       // command history for arrow keys
  let conHistIdx = -1;

  // Apply the command policy client-side (head token, exact/prefix, case-insensitive) so the
  // palette can grey out blocked commands with the server's own reason. Server stays authoritative.
  function conDeniedReason(cmd) {
    const head = (cmd.trim().split(/\s+/)[0] || "").toLowerCase();
    if (!head) return null;
    for (const r of conDeny) {
      const tok = String(r.token || "").toLowerCase();
      if (r.kind === "exact" ? head === tok : head.startsWith(tok)) return r.reason || "blocked";
    }
    return null;
  }

  async function openConsolePanel() {
    setActiveToolbar("console");
    clearBuildPlacement(false);
    activeInfoPanel = "console";
    clientPanel.className = "visible info-panel";
    if (!clientPanel.querySelector(".con-body")) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading console...</div></div></div>`;
    }
    try {
      const rc = await fetch(`/console-config?player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
      const dc = await rc.json().catch(() => ({}));
      conEnabled = dc.enabled === true;
      conIsHost = dc.host === true;
    } catch (_) { conEnabled = false; conIsHost = false; }
    if (conEnabled && !conCatalog) {
      try {
        const r = await fetch(`/console/commands?player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
        const d = await r.json().catch(() => ({}));
        if (d.ok) {
          conCatalog = Array.isArray(d.commands) ? d.commands : [];
          conDeny = Array.isArray(d.denyRules) ? d.denyRules : [];
        }
      } catch (_) {}
    }
    renderConsole();
  }

  function conRenderLog() {
    if (!conLog.length) return `<div class="sq-subtle">Output appears here. The catalog is DFHack's own command list; type to filter it.</div>`;
    return conLog.map(e => `
      <div class="con-entry">
        <div class="con-cmd">&gt; ${escapeHtml(e.cmd)}</div>
        ${e.err ? `<div class="con-err">${escapeHtml(e.err)}</div>`
                : `<pre class="con-out">${escapeHtml(e.output || "(no output)")}</pre>${Number(e.status) !== 0 ? `<div class="con-err">exit status ${e.status}</div>` : ""}`}
      </div>`).join("");
  }

  function renderConsole() {
    let body;
    const hostToggle = conIsHost
      ? `<label class="sq-check con-toggle"><input type="checkbox" id="conEnable"${conEnabled ? " checked" : ""}> Console enabled (host setting)</label>`
      : "";
    if (!conEnabled) {
      body = `
        <div class="sq-hint">The DFHack command console is <b>off</b>. Commands run on the host's machine,
        so it ships disabled until the host turns it on${conIsHost ? " below" : " (host's browser tab, or dfcapture-hostwrites.json next to the DF executable)"}.
        ${conIsHost ? "" : "<br><br>Ask the host to enable it from their tab."}</div>
        ${hostToggle}`;
    } else {
      body = `
        <div class="con-log" id="conLog">${conRenderLog()}</div>
        <div class="con-inputrow">
          <input id="conInput" class="sq-rename con-input" type="text" placeholder="DFHack command... (Enter to run)" spellcheck="false" autocomplete="off">
          <button class="sq-btn primary" id="conRun">Run</button>
        </div>
        <div id="conSuggest" class="con-suggest"></div>
        <div class="sq-subtle">A running command holds DF's core lock until it finishes and cannot be interrupted. Blocked commands (server policy) are refused with a reason.</div>
        ${hostToggle}`;
    }

    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs"><span class="info-tab active">DFHack Console</span><span id="conStatus" class="sq-status"></span></div>
        <div class="info-body" style="grid-template-columns:1fr;"><div class="con-body">${body}</div></div>
        <div class="info-footer"><div>Runs DFHack commands on the host fort. Contained by a server-side blocklist that binds every player, host included.</div></div>
      </div>`;

    const enable = document.getElementById("conEnable");
    if (enable) enable.addEventListener("change", async () => {
      try {
        const r = await fetch(`/console-config?player=${encodeURIComponent(player)}&enabled=${enable.checked ? "on" : "off"}&t=${Date.now()}`, { method: "POST", cache: "no-store" });
        const d = await r.json().catch(() => ({}));
        if (!r.ok || d.ok === false) throw new Error(d.err || "toggle failed");
        conEnabled = d.enabled === true;
        if (conEnabled && !conCatalog) await openConsolePanel(); else renderConsole();
      } catch (err) {
        const st = document.getElementById("conStatus");
        if (st) { st.textContent = err.message || "Could not toggle."; st.className = "sq-status err"; }
      }
    });

    const input = document.getElementById("conInput");
    const runBtn = document.getElementById("conRun");
    const suggest = document.getElementById("conSuggest");
    if (!input) return;
    input.addEventListener("click", ev => ev.stopPropagation());
    input.addEventListener("keydown", ev => {
      ev.stopPropagation();
      if (ev.key === "Enter") { ev.preventDefault(); conRunCommand(); }
      else if (ev.key === "ArrowUp") { ev.preventDefault(); if (conHistory.length) { conHistIdx = conHistIdx < 0 ? conHistory.length - 1 : Math.max(0, conHistIdx - 1); input.value = conHistory[conHistIdx]; } }
      else if (ev.key === "ArrowDown") { ev.preventDefault(); if (conHistIdx >= 0) { conHistIdx = conHistIdx + 1; if (conHistIdx >= conHistory.length) { conHistIdx = -1; input.value = ""; } else input.value = conHistory[conHistIdx]; } }
      else if (ev.key === "Escape") { ev.preventDefault(); if (suggest) suggest.innerHTML = ""; }
    });
    input.addEventListener("input", () => {
      if (!suggest || !conCatalog) return;
      const q = input.value.trim().toLowerCase();
      if (!q) { suggest.innerHTML = ""; return; }
      const hits = conCatalog.filter(c => c.name.toLowerCase().includes(q)).slice(0, 12);
      suggest.innerHTML = hits.map(c => {
        const reason = conDeniedReason(c.name);
        return `<div class="con-sug${reason ? " denied" : ""}" data-con-sug="${escapeHtml(c.name)}" title="${escapeHtml(reason || c.short || "")}">
            <span class="con-sug-name">${escapeHtml(c.name)}</span>
            <span class="con-sug-short">${escapeHtml(reason ? "blocked: " + reason : (c.short || ""))}</span>
          </div>`;
      }).join("");
      suggest.querySelectorAll("[data-con-sug]").forEach(el => el.addEventListener("click", e => {
        e.preventDefault(); e.stopPropagation();
        input.value = el.dataset.conSug + " ";
        suggest.innerHTML = "";
        input.focus();
      }));
    });
    if (runBtn) runBtn.addEventListener("click", e => { e.preventDefault(); e.stopPropagation(); conRunCommand(); });
    input.focus();
  }

  async function conRunCommand() {
    const input = document.getElementById("conInput");
    const st = document.getElementById("conStatus");
    if (!input) return;
    const cmd = input.value.trim();
    if (!cmd) return;
    const localDeny = conDeniedReason(cmd);
    if (localDeny) {
      conLog.push({ cmd, err: "blocked: " + localDeny });
      conRefreshLogOnly();
      return;
    }
    conHistory.push(cmd);
    conHistIdx = -1;
    input.value = "";
    if (st) { st.textContent = "Running... (DF is locked until it finishes)"; st.className = "sq-status"; }
    try {
      const r = await fetch(`/console/run?player=${encodeURIComponent(player)}&cmd=${encodeURIComponent(cmd)}&t=${Date.now()}`, { method: "POST", cache: "no-store" });
      const d = await r.json().catch(() => ({}));
      if (d.ok) conLog.push({ cmd, output: d.output, status: d.status });
      else conLog.push({ cmd, err: d.err || d.error || "command failed" });
      if (st) st.textContent = "";
    } catch (err) {
      conLog.push({ cmd, err: err.message || "request failed" });
      if (st) st.textContent = "";
    }
    if (conLog.length > 60) conLog = conLog.slice(-60);
    conRefreshLogOnly();
  }

  function conRefreshLogOnly() {
    const log = document.getElementById("conLog");
    if (log) { log.innerHTML = conRenderLog(); log.scrollTop = log.scrollHeight; }
    const input = document.getElementById("conInput");
    if (input) input.focus();
  }
