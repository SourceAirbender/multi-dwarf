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

  // ---- Fort Admin panel: nobles (appoint), justice (cases + pardon), petitions ----------------
  // openPanel routes "nobles" and "justice" here. Convict and interrogate remain native-only,
  // while case inspection and pardons are available in the browser.
  let faTab = "nobles";       // nobles | justice | petitions
  let faNobles = null;
  let faJustice = null;
  let faPetitions = null;
  let faAssignFor = null;     // positionId with the candidate picker open
  let faCandidates = null;

  function faStatus(msg, isErr) {
    const el = document.getElementById("faStatus");
    if (el) { el.textContent = msg || ""; el.className = "sq-status" + (isErr ? " err" : ""); }
  }

  async function faFetchJson(path) {
    const sep = path.includes("?") ? "&" : "?";
    const r = await fetch(`${path}${sep}player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
    const d = await r.json().catch(() => ({}));
    if (!r.ok || d.ok === false) throw new Error(d.error || d.err || `${path} failed`);
    return d;
  }

  async function openFortAdminPanel(tab) {
    setActiveToolbar(tab === "justice" ? "justice" : "nobles");
    clearBuildPlacement(false);
    activeInfoPanel = "fortadmin";
    if (tab) faTab = tab;
    clientPanel.className = "visible info-panel";
    if (!clientPanel.querySelector(".fa-body")) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading...</div></div></div>`;
    }
    await faRefresh();
  }

  async function faRefresh() {
    try {
      if (faTab === "nobles") faNobles = await faFetchJson("/nobles");
      else if (faTab === "justice") faJustice = await faFetchJson("/justice");
      else faPetitions = await faFetchJson("/petitions");
    } catch (_) {}
    renderFortAdmin();
  }

  function faRenderNobles() {
    const rows = (faNobles && Array.isArray(faNobles.positions)) ? faNobles.positions : [];
    if (!rows.length) return `<div class="sq-empty">No positions available.</div>`;
    return rows.map(p => {
      const held = Number(p.unitId) >= 0;
      const rooms = Array.isArray(p.rooms) && p.rooms.length
        ? `<span class="wm-dim">${p.roomsSatisfied ? "rooms ok" : "needs: " + p.rooms.join(", ")}</span>` : "";
      const mand = Number(p.mandates) > 0 ? `<span class="ann-badge alert" title="Active mandates">${p.mandates}m</span>` : "";
      const prec = (p.bookkeeperPrecision != null && Number(p.bookkeeperPrecision) >= 0)
        ? `<span class="fa-prec">${[1,2,3,4,5].map(n =>
            `<button class="fa-prec-btn${Number(p.bookkeeperPrecision) === n - 1 ? " sel" : ""}" data-fa-prec="${n - 1}" title="Record-keeping precision ${n}/5">${n}</button>`).join("")}</span>` : "";
      const picker = Number(faAssignFor) === Number(p.positionId) ? faRenderPicker(p) : "";
      return `<div class="fa-row">
          <span class="sq-pos">${escapeHtml(p.name || "")}</span>
          <span class="sq-mname" style="color:${held ? sqColor(p.professionColor) : "#6a6151"}">${held ? escapeHtml(p.holder || "") : "(vacant)"}</span>
          <span class="fa-meta">${mand}${rooms}${prec}</span>
          <span class="fa-actions">
            <button class="sq-btn tiny" data-fa-assign="${p.positionId}">${held ? "Replace" : "Assign"}</button>
            ${held ? `<button class="sq-btn tiny danger" data-fa-vacate="${p.positionId}">Vacate</button>` : ""}
          </span>
        </div>${picker}`;
    }).join("");
  }

  function faRenderPicker(p) {
    if (!faCandidates) return `<div class="sq-subtle">Loading candidates...</div>`;
    const cands = Array.isArray(faCandidates.candidates) ? faCandidates.candidates : [];
    if (!cands.length) return `<div class="sq-subtle">No eligible citizens.</div>`;
    return `<div class="fa-picker">
        <select id="faCand" class="sq-select">${cands.map(c =>
          `<option value="${c.unitId}">${escapeHtml(c.name)}${c.current ? " (current)" : ""} — ${escapeHtml(c.profession || "")}</option>`).join("")}</select>
        <button class="sq-btn primary tiny" data-fa-confirm="${p.positionId}">Appoint</button>
        <button class="sq-btn tiny" data-fa-cancel>Cancel</button>
      </div>`;
  }

  function faRenderJustice() {
    const crimes = (faJustice && Array.isArray(faJustice.crimes)) ? faJustice.crimes : [];
    if (!crimes.length) return `<div class="sq-empty">No open cases — an orderly fort.</div>`;
    return crimes.map(c => {
      const badges = `${c.sentenced ? `<span class="ann-badge rep">sentenced</span>` : ""}` +
                     `${c.needsTrial ? `<span class="ann-badge alert">needs trial</span>` : ""}` +
                     `${Number(c.witnessCount) > 0 ? `<span class="wm-dim">${c.witnessCount} witness(es)</span>` : ""}`;
      const who = c.criminal || c.accused || "unknown";
      const whoId = Number(c.criminalId >= 0 ? c.criminalId : c.accusedId);
      return `<div class="fa-row crime">
          <span class="sq-pos">${escapeHtml(c.name || "crime")}${Number(c.year) > 0 ? ` <span class="ann-when">y${c.year}</span>` : ""}</span>
          <span class="sq-mname" style="color:${sqColor(c.criminalProfessionColor != null ? c.criminalProfessionColor : c.accusedProfessionColor)}">${escapeHtml(who)}</span>
          <span class="fa-meta">${badges}${c.victim ? `<span class="wm-dim">victim: ${escapeHtml(c.victim)}</span>` : ""}</span>
          <span class="fa-actions">${c.sentenced && whoId >= 0 ? `<button class="sq-btn tiny" data-fa-pardon="${whoId}">Pardon</button>` : ""}</span>
        </div>`;
    }).join("");
  }

  function faRenderPetitions() {
    const pets = (faPetitions && Array.isArray(faPetitions.petitions)) ? faPetitions.petitions : [];
    if (!pets.length) return `<div class="sq-empty">No petitions.</div>`;
    return pets.map(p => `<div class="fa-row">
        <span class="sq-pos">${escapeHtml(p.purpose || "petition")}</span>
        <span class="sq-mname">${escapeHtml(p.petitioner || "")}</span>
        <span class="fa-meta"><span class="wm-dim">${escapeHtml(p.summary || "")}</span>
          ${p.pending ? `<span class="ann-badge alert">pending</span>` : `<span class="ann-badge rep">accepted</span>`}</span>
        <span class="fa-actions"></span>
      </div>`).join("");
  }

  function renderFortAdmin() {
    const tabs = [["nobles", "Nobles"], ["justice", "Justice"], ["petitions", "Petitions"]]
      .map(([k, label]) => `<button class="info-tab${faTab === k ? " active" : ""}" data-fa-tab="${k}">${escapeHtml(label)}</button>`).join("");
    let body, footer;
    if (faTab === "justice") {
      body = faRenderJustice();
      footer = "Pardons apply immediately. Convictions and interrogations remain native-only.";
    } else if (faTab === "petitions") {
      body = faRenderPetitions();
      footer = "Petitions are answered on the host's DF screen when they arrive; this is the ledger.";
    } else {
      body = faRenderNobles();
      footer = "Appointments apply to the host fort immediately. Bookkeeper precision is the 1-5 record-keeping goal.";
    }
    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs">${tabs}<span id="faStatus" class="sq-status"></span></div>
        <div class="info-body" style="grid-template-columns:1fr;"><div class="fa-body">${body}</div></div>
        <div class="info-footer"><div>${footer}</div></div>
      </div>`;

    clientPanel.querySelectorAll("[data-fa-tab]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation(); faTab = b.dataset.faTab; faAssignFor = null; faCandidates = null; faRefresh();
    }));
    clientPanel.querySelectorAll("[data-fa-assign]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      faAssignFor = Number(b.dataset.faAssign); faCandidates = null; renderFortAdmin();
      try { faCandidates = await faFetchJson(`/noble-candidates?position=${faAssignFor}`); } catch (_) { faCandidates = { candidates: [] }; }
      renderFortAdmin();
    }));
    const cancel = clientPanel.querySelector("[data-fa-cancel]");
    if (cancel) cancel.addEventListener("click", e => { e.preventDefault(); e.stopPropagation(); faAssignFor = null; faCandidates = null; renderFortAdmin(); });
    clientPanel.querySelectorAll("[data-fa-confirm]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const sel = document.getElementById("faCand");
      const unit = sel ? Number(sel.value) : -1;
      try {
        const r = await fetch(`/noble-assign?player=${encodeURIComponent(player)}&position=${b.dataset.faConfirm}&unit=${unit}&t=${Date.now()}`, { method: "POST", cache: "no-store" });
        const d = await r.json().catch(() => ({}));
        if (!r.ok || d.ok === false) throw new Error(d.error || "assign failed");
        faAssignFor = null; faCandidates = null;
        await faRefresh(); faStatus("Appointed.", false);
      } catch (err) { faStatus(err.message || "Could not appoint.", true); }
    }));
    clientPanel.querySelectorAll("[data-fa-vacate]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try {
        const r = await fetch(`/noble-assign?player=${encodeURIComponent(player)}&position=${b.dataset.faVacate}&unit=-1&t=${Date.now()}`, { method: "POST", cache: "no-store" });
        const d = await r.json().catch(() => ({}));
        if (!r.ok || d.ok === false) throw new Error(d.error || "vacate failed");
        await faRefresh(); faStatus("Position vacated.", false);
      } catch (err) { faStatus(err.message || "Could not vacate.", true); }
    }));
    clientPanel.querySelectorAll("[data-fa-prec]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try {
        const r = await fetch(`/noble-precision?player=${encodeURIComponent(player)}&level=${b.dataset.faPrec}&t=${Date.now()}`, { method: "POST", cache: "no-store" });
        const d = await r.json().catch(() => ({}));
        if (!r.ok || d.ok === false) throw new Error(d.error || "precision failed");
        await faRefresh(); faStatus("Precision set.", false);
      } catch (err) { faStatus(err.message || "Could not set precision.", true); }
    }));
    clientPanel.querySelectorAll("[data-fa-pardon]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      if (!confirm("Commute this unit's sentence?")) return;
      try {
        const r = await fetch(`/justice-pardon?player=${encodeURIComponent(player)}&unit=${b.dataset.faPardon}&t=${Date.now()}`, { method: "POST", cache: "no-store" });
        const d = await r.json().catch(() => ({}));
        if (!r.ok || d.ok === false) throw new Error(d.error || "pardon failed");
        await faRefresh(); faStatus("Sentence commuted.", false);
      } catch (err) { faStatus(err.message || "Could not pardon.", true); }
    }));
  }
