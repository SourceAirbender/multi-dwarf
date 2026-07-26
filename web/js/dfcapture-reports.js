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

  // ---- Announcements/Reports panel + per-unit combat log ---------------------------------------
  // GET /reports (sections, since/before paging, counts) and GET /combat-reports?unit= (a unit's
  // Combat/Sparring/Hunting log, continuation-joined). Reports are read on DF's render thread by
  // the backend; this panel is pure fetch + DOM.
  const ANN_SECTIONS = [["all", "All"], ["combat", "Combat"], ["sieges", "Sieges"], ["artifacts", "Artifacts"],
                        ["trade", "Trade"], ["nobles", "Nobles"], ["deaths", "Deaths"], ["misc", "Misc"]];
  let annSection = "all";
  let annReports = [];       // newest-first page
  let annCounts = null;      // {sectionKey: n}
  let annUnitLog = null;     // {unitId, name, entries[]} when viewing one unit's combat log
  let annLoading = false;

  function annColor(idx, bright) {
    const i = Number(idx);
    return sqColor((i >= 0 && i < 8) ? (bright ? i + 8 : i) : 7);
  }

  async function openReportsPanel() {
    setActiveToolbar("reports");
    clearBuildPlacement(false);
    activeInfoPanel = "reports";
    annUnitLog = null;
    clientPanel.className = "visible info-panel";
    if (!clientPanel.querySelector(".ann-body")) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading reports...</div></div></div>`;
    }
    await annFetch(true);
  }

  async function annFetch(fresh) {
    if (annLoading) return;
    annLoading = true;
    try {
      const qs = new URLSearchParams();
      qs.set("player", player);
      qs.set("section", annSection);
      qs.set("max", "120");
      qs.set("counts", "1");
      if (!fresh && annReports.length) qs.set("before", String(annReports[annReports.length - 1].id));
      qs.set("t", Date.now());
      const r = await fetch(`/reports?${qs.toString()}`, { cache: "no-store" });
      const d = await r.json().catch(() => ({}));
      if (d && Array.isArray(d.reports)) {
        annCounts = d.counts || annCounts;
        annReports = fresh ? d.reports : annReports.concat(d.reports);
      } else if (fresh) {
        annReports = [];
      }
    } catch (_) { if (fresh) annReports = []; }
    annLoading = false;
    renderReports();
  }

  async function annOpenUnitLog(unitId, name) {
    annUnitLog = { unitId, name: name || "unit", entries: null };
    renderReports();
    try {
      const r = await fetch(`/combat-reports?player=${encodeURIComponent(player)}&unit=${unitId}&max=200&t=${Date.now()}`, { cache: "no-store" });
      const d = await r.json().catch(() => ({}));
      annUnitLog.entries = Array.isArray(d.reports) ? d.reports : [];
    } catch (_) { annUnitLog.entries = []; }
    renderReports();
  }

  function annRow(e) {
    const when = (Number(e.year) > 0) ? `<span class="ann-when">y${e.year}</span>` : "";
    const badges = `${e.box ? `<span class="ann-badge box" title="Paused the game">!</span>` : ""}` +
                   `${e.alert ? `<span class="ann-badge alert" title="Lit the alert button">A</span>` : ""}` +
                   `${Number(e.repeatCount) > 0 ? `<span class="ann-badge rep">x${Number(e.repeatCount) + 1}</span>` : ""}`;
    const logBtn = Number(e.speakerId) >= 0
      ? `<button class="sq-btn tiny" data-ann-unitlog="${e.speakerId}" title="This unit's combat log">log</button>` : "";
    return `<div class="ann-row${e.continuation ? " cont" : ""}">
        <span class="ann-text" style="color:${annColor(e.color, e.bright)}">${escapeHtml(e.text || "")}</span>
        <span class="ann-meta">${badges}${when}${logBtn}</span>
      </div>`;
  }

  function renderReports() {
    let body;
    if (annUnitLog) {
      const entries = annUnitLog.entries;
      const rows = entries === null ? `<div class="info-message">Loading combat log...</div>`
        : (entries.length ? entries.map(annRow).join("") : `<div class="sq-empty">No combat, sparring, or hunting reports for this unit.</div>`);
      body = `
        <div class="sq-form-row"><button class="sq-btn" data-ann-back>&larr; All reports</button>
          <span class="hosp-title">Combat log — unit ${annUnitLog.unitId}</span></div>
        <div class="ann-log">${rows}</div>`;
    } else {
      const chips = ANN_SECTIONS.map(([k, label]) => {
        const n = annCounts && annCounts[k] != null ? ` (${annCounts[k]})` : "";
        return `<button class="wo-chip${annSection === k ? " sel" : ""}" data-ann-sec="${k}">${escapeHtml(label + n)}</button>`;
      }).join("");
      const rows = annReports.length ? annReports.map(annRow).join("") : `<div class="sq-empty">No reports in this section yet.</div>`;
      body = `
        <div class="ann-chips">${chips}</div>
        <div class="ann-log">${rows}</div>
        <div class="sq-form-row">
          <button class="sq-btn" data-ann-older>Load older</button>
          <button class="sq-btn" data-ann-refresh>Refresh</button>
        </div>`;
    }

    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs"><span class="info-tab active">Reports</span></div>
        <div class="info-body" style="grid-template-columns:1fr;"><div class="ann-body">${body}</div></div>
        <div class="info-footer"><div>Your fort's full announcement log, straight from DF's reports — sectioned like the native screen.</div></div>
      </div>`;

    clientPanel.querySelectorAll("[data-ann-sec]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation(); annSection = b.dataset.annSec; annFetch(true);
    }));
    const older = clientPanel.querySelector("[data-ann-older]");
    if (older) older.addEventListener("click", e => { e.preventDefault(); e.stopPropagation(); annFetch(false); });
    const refresh = clientPanel.querySelector("[data-ann-refresh]");
    if (refresh) refresh.addEventListener("click", e => { e.preventDefault(); e.stopPropagation(); annFetch(true); });
    const back = clientPanel.querySelector("[data-ann-back]");
    if (back) back.addEventListener("click", e => { e.preventDefault(); e.stopPropagation(); annUnitLog = null; renderReports(); });
    clientPanel.querySelectorAll("[data-ann-unitlog]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation(); annOpenUnitLog(Number(b.dataset.annUnitlog));
    }));
  }
