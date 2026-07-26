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

  // ---- Diplomacy panel: GET /diplo (poll) + POST /diplo-request-priority -----------------------
  // Reads the live diplomat-meeting state (petition/meeting counts, the dialogue, and -- on the
  // export-agreement Requests screen -- editable priorities). Advancing the meeting (Okay / land-
  // holder pick / Requests Done) stays on the host's native DF screen. The panel polls while
  // open because this build has no push transport and only re-renders when
  // the state seq changes so an open priority selector is never clobbered mid-click.
  let diploState = null;
  let diploSeq = -1;

  async function openDiploPanel() {
    setActiveToolbar("diplo");
    clearBuildPlacement(false);
    activeInfoPanel = "diplo";
    diploSeq = -1;
    clientPanel.className = "visible info-panel";
    if (!clientPanel.querySelector(".dip-body")) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading diplomacy...</div></div></div>`;
    }
    await refreshDiplo(false);
    diploPoll();
  }

  function diploPoll() {
    if (activeInfoPanel !== "diplo") return;
    setTimeout(() => {
      if (activeInfoPanel !== "diplo") return;
      refreshDiplo(true).finally(diploPoll);
    }, 2500);
  }

  async function refreshDiplo(fromPoll) {
    try {
      const r = await fetch(`/diplo?player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
      const d = await r.json().catch(() => null);
      if (!d) { if (!fromPoll) renderDiplo(); return; }
      const changed = Number(d.seq) !== diploSeq;
      diploState = d;
      diploSeq = Number(d.seq);
      if (!fromPoll || changed) renderDiplo();
    } catch (_) { if (!fromPoll) renderDiplo(); }
  }

  function diploWords(words, truncated) {
    if (!Array.isArray(words) || !words.length) return `<div class="sq-subtle">No dialogue.</div>`;
    let html = "";
    for (const w of words) {
      if (w.blank) html += "<br><br>";
      else if (w.nl) html += "<br>";
      const ind = w.ind ? "dip-ind" : "";
      html += `<span class="${ind}"${w.c ? ` style="color:${escapeHtml(w.c)}"` : ""}>${escapeHtml(w.t || "")} </span>`;
    }
    if (truncated) html += `<span class="sq-subtle">…(truncated)</span>`;
    return `<div class="dip-dialogue">${html}</div>`;
  }

  function diploRequests(reqs) {
    const tabs = Array.isArray(reqs.tabs) ? reqs.tabs : [];
    if (!tabs.length) return "";
    return `<div class="sq-section-title">Export agreement priorities</div>` + tabs.map(tab => {
      const rows = (Array.isArray(tab.priorities) ? tab.priorities : []).map((lvl, j) =>
        `<div class="dip-prio-row">
           <span class="dip-prio-idx">#${j + 1}</span>
           <span class="fa-prec">${[0, 1, 2, 3, 4].map(v =>
             `<button class="fa-prec-btn${Number(lvl) === v ? " sel" : ""}" data-dip-cat="${tab.cat}" data-dip-idx="${j}" data-dip-val="${v}" title="Priority ${v}">${v}</button>`).join("")}</span>
         </div>`).join("");
      return `<div class="dip-tab"><div class="dip-tab-name">${escapeHtml(tab.name || ("category " + tab.cat))}${tab.truncated ? ' <span class="sq-subtle">(list truncated)</span>' : ""}</div>${rows || `<div class="sq-subtle">No items.</div>`}</div>`;
    }).join("");
  }

  function renderDiplo() {
    const s = diploState || {};
    const m = s.meeting;
    let body;
    if (!m) {
      body = `<div class="dip-counts">
          <div class="dip-count"><span class="dip-num">${Number(s.meetingsQueued) || 0}</span> meeting(s) queued</div>
          <div class="dip-count"><span class="dip-num">${Number(s.petitionsPending) || 0}</span> petition(s) pending</div>
        </div>
        <div class="sq-hint">No diplomat meeting is open. When an envoy arrives and the host opens the meeting, its dialogue and any export-agreement requests appear here.</div>`;
    } else {
      const topics = Array.isArray(m.topics) && m.topics.length
        ? `<div class="dip-topics">${m.topics.map(t => `<span class="wo-chip">${escapeHtml(t)}</span>`).join("")}</div>` : "";
      const landHolder = (m.mode === "landHolder" && m.landHolder)
        ? `<div class="sq-section-title">Land holder</div><div class="sq-subtle">Positions: ${(m.landHolder.positions || []).map(escapeHtml).join(", ") || "—"}. Choose the appointee on the host's DF screen.</div>` : "";
      const requests = (m.mode === "requests" && m.requests) ? diploRequests(m.requests) : "";
      body = `
        <div class="dip-parties"><b>${escapeHtml(m.actor || "Envoy")}</b> <span class="wm-dim">→ ${escapeHtml(m.target || "the fort")}</span> <span class="ann-badge alert">${escapeHtml(m.mode || "text")}</span></div>
        ${diploWords(m.words, m.wordsTruncated)}
        ${topics}
        ${landHolder}
        ${requests}`;
    }

    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs"><span class="info-tab active">Diplomacy</span><span id="dipStatus" class="sq-status"></span></div>
        <div class="info-body" style="grid-template-columns:1fr;"><div class="dip-body">${body}</div></div>
        <div class="info-footer"><div>Meeting dialogue is live. Okay / land-holder pick / Requests Done are pressed on the host's DF screen.</div></div>
      </div>`;

    clientPanel.querySelectorAll("[data-dip-val]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try {
        const r = await fetch(`/diplo-request-priority?player=${encodeURIComponent(player)}&cat=${b.dataset.dipCat}&index=${b.dataset.dipIdx}&value=${b.dataset.dipVal}&t=${Date.now()}`, { method: "POST", cache: "no-store" });
        const d = await r.json().catch(() => ({}));
        if (!r.ok || d.ok === false) throw new Error(d.error || "priority failed");
        diploSeq = -1;              // force a re-render on the next poll
        await refreshDiplo(false);
        const st = document.getElementById("dipStatus");
        if (st) { st.textContent = "Priority set."; st.className = "sq-status"; }
      } catch (err) {
        const st = document.getElementById("dipStatus");
        if (st) { st.textContent = err.message || "Could not set priority."; st.className = "sq-status err"; }
      }
    }));
  }
