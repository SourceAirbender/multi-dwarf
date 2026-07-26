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

  // ---- Vote panel: GET /vote (poll) + /vote-start /vote-cast /vote-close ------------------------
  // The multiplayer fortress vote. Everyone polls the same state; casting is attributed to your
  // player name. "elevation" votes are auto-detected from a native land-holder offer; "custom"
  // votes are anything you type. Re-renders only when a signature (id + tally + detection) changes,
  // so a live tally updates but a half-typed custom topic is not clobbered.
  let voteState = null;
  let voteSig = "";

  async function openVotePanel() {
    setActiveToolbar("vote");
    clearBuildPlacement(false);
    activeInfoPanel = "vote";
    voteSig = "";
    clientPanel.className = "visible info-panel";
    if (!clientPanel.querySelector(".vote-body")) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading vote...</div></div></div>`;
    }
    await refreshVote(false);
    votePoll();
  }

  function votePoll() {
    if (activeInfoPanel !== "vote") return;
    setTimeout(() => {
      if (activeInfoPanel !== "vote") return;
      refreshVote(true).finally(votePoll);
    }, 2000);
  }

  function voteSignature(d) {
    const a = d.active, l = d.lastResult, det = d.detection || {};
    return [a ? a.id + ":" + a.yes + ":" + a.no : "none",
            l ? l.id + ":" + l.result : "none",
            det.pending ? "det:" + (det.titles || []).join(",") : "nodet"].join("|");
  }

  async function refreshVote(fromPoll) {
    try {
      const r = await fetch(`/vote?player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
      const d = await r.json().catch(() => null);
      if (!d) { if (!fromPoll) renderVote(); return; }
      const sig = voteSignature(d);
      voteState = d;
      if (!fromPoll || sig !== voteSig) { voteSig = sig; renderVote(); }
    } catch (_) { if (!fromPoll) renderVote(); }
  }

  function voteStatus(msg, isErr) {
    const el = document.getElementById("voteStatus");
    if (el) { el.textContent = msg || ""; el.className = "sq-status" + (isErr ? " err" : ""); }
  }

  async function votePost(path, params) {
    const qs = new URLSearchParams();
    qs.set("player", player);
    Object.entries(params || {}).forEach(([k, v]) => qs.set(k, v == null ? "" : String(v)));
    qs.set("t", Date.now());
    const r = await fetch(`${path}?${qs.toString()}`, { method: "POST", cache: "no-store" });
    const d = await r.json().catch(() => ({}));
    if (!r.ok || d.ok === false) throw new Error(d.error || d.err || "request failed");
    return d;
  }

  function renderVote() {
    const s = voteState || {};
    const a = s.active, last = s.lastResult, det = s.detection || {};
    let body;

    if (a) {
      const total = (Number(a.yes) || 0) + (Number(a.no) || 0);
      const yesPct = total ? Math.round((Number(a.yes) || 0) / total * 100) : 0;
      const votes = Array.isArray(a.votes) ? a.votes : [];
      const mine = votes.find(v => v.player === player);
      const list = votes.length ? votes.map(v =>
        `<span class="vote-chip ${v.choice}">${escapeHtml(v.player)}: ${v.choice}</span>`).join("") : `<span class="sq-subtle">No votes cast yet.</span>`;
      body = `
        <div class="vote-topic"><span class="ann-badge alert">${escapeHtml(a.kind || "custom")}</span> ${escapeHtml(a.topic || "Vote")}</div>
        <div class="vote-tally">
          <div class="vote-bar"><div class="vote-bar-yes" style="width:${yesPct}%"></div></div>
          <div class="vote-nums"><span class="vote-yes">Yes ${Number(a.yes) || 0}</span> · <span class="vote-no">No ${Number(a.no) || 0}</span></div>
        </div>
        <div class="vote-cast">
          <button class="sq-btn primary${mine && mine.choice === "yes" ? " sel" : ""}" data-vote-cast="yes">Vote Yes</button>
          <button class="sq-btn danger${mine && mine.choice === "no" ? " sel" : ""}" data-vote-cast="no">Vote No</button>
          <button class="sq-btn" data-vote-close>Close vote</button>
        </div>
        <div class="sq-subtle">Opened by ${escapeHtml(a.openedBy || "?")}${mine ? ` · you voted ${mine.choice}` : " · you haven't voted"}</div>
        <div class="vote-list">${list}</div>`;
    } else {
      const detBlock = det.pending
        ? `<div class="vote-detect">
             <div><b>A land-holder offer is pending.</b> <span class="wm-dim">${(det.titles || []).map(escapeHtml).join(", ")}</span></div>
             <button class="sq-btn primary" data-vote-start-elev>Start elevation vote</button>
           </div>`
        : "";
      body = `
        ${detBlock}
        <div class="sq-section-title">Start a vote</div>
        <div class="vote-cast">
          <input id="voteTopic" class="sq-rename" type="text" placeholder="Vote topic..." maxlength="120" spellcheck="false" style="flex:1;max-width:none;">
          <button class="sq-btn primary" data-vote-start-custom>Open</button>
        </div>`;
    }

    if (last) {
      body += `
        <div class="sq-section-title">Last result</div>
        <div class="vote-last">
          <span class="ann-badge ${last.result === "passed" ? "" : "box"}">${escapeHtml(last.result || "")}</span>
          ${escapeHtml(last.topic || "")} <span class="wm-dim">— ${Number(last.yes) || 0} yes / ${Number(last.no) || 0} no, closed by ${escapeHtml(last.closedBy || "?")}</span>
        </div>`;
    }

    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs"><span class="info-tab active">Vote</span><span id="voteStatus" class="sq-status"></span></div>
        <div class="info-body" style="grid-template-columns:1fr;"><div class="vote-body">${body}</div></div>
        <div class="info-footer"><div>A shared fortress vote for the whole table. Anyone may open, cast, or close.</div></div>
      </div>`;

    clientPanel.querySelectorAll("[data-vote-cast]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await votePost("/vote-cast", { choice: b.dataset.voteCast }); voteSig = ""; await refreshVote(false); }
      catch (err) { voteStatus(err.message || "Could not vote.", true); }
    }));
    const closeBtn = clientPanel.querySelector("[data-vote-close]");
    if (closeBtn) closeBtn.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await votePost("/vote-close", {}); voteSig = ""; await refreshVote(false); voteStatus("Vote closed.", false); }
      catch (err) { voteStatus(err.message || "Could not close.", true); }
    });
    const elev = clientPanel.querySelector("[data-vote-start-elev]");
    if (elev) elev.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await votePost("/vote-start", {}); voteSig = ""; await refreshVote(false); }
      catch (err) { voteStatus(err.message || "Could not start.", true); }
    });
    const custom = clientPanel.querySelector("[data-vote-start-custom]");
    if (custom) custom.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const inp = document.getElementById("voteTopic");
      const topic = inp ? inp.value.trim() : "";
      if (!topic) { voteStatus("Enter a topic first.", true); return; }
      try { await votePost("/vote-start", { topic }); voteSig = ""; await refreshVote(false); }
      catch (err) { voteStatus(err.message || "Could not start.", true); }
    });
    const topicIn = document.getElementById("voteTopic");
    if (topicIn) {
      topicIn.addEventListener("click", ev => ev.stopPropagation());
      topicIn.addEventListener("keydown", ev => { ev.stopPropagation(); if (ev.key === "Enter") { ev.preventDefault(); const c = clientPanel.querySelector("[data-vote-start-custom]"); if (c) c.click(); } });
    }
  }
