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

  // ---- Hospital panel: /hospitals (discover) + /hospital-patients (patients + treatment queue) ----
  // Reuses the shared sqColor() profession-colour helper. This panel shows who is hurt and who is
  // treating them; supply editing remains native-only.
  let hospList = [];       // [{locationId, name}]
  let hospSelLoc = null;   // selected hospital locationId
  let hospData = null;     // /hospital-patients payload

  async function openHospitalPanel() {
    setActiveToolbar("hospital");
    clearBuildPlacement(false);
    activeInfoPanel = "hospital";
    clientPanel.className = "visible info-panel";
    if (!clientPanel.querySelector(".hosp-body")) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading hospital...</div></div></div>`;
    }
    try {
      const r = await fetch(`/hospitals?player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
      const d = await r.json();
      hospList = Array.isArray(d.hospitals) ? d.hospitals : [];
      if (hospSelLoc == null || !hospList.some(h => Number(h.locationId) === Number(hospSelLoc)))
        hospSelLoc = hospList.length ? Number(hospList[0].locationId) : null;
      await refreshHospital();
    } catch (_) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Hospital data unavailable.</div></div></div>`;
    }
  }

  async function refreshHospital() {
    if (hospSelLoc == null) { hospData = null; renderHospital(); return; }
    try {
      const r = await fetch(`/hospital-patients?player=${encodeURIComponent(player)}&location=${hospSelLoc}&t=${Date.now()}`, { cache: "no-store" });
      hospData = await r.json();
    } catch (_) { hospData = null; }
    renderHospital();
  }

  function renderHospital() {
    const picker = hospList.length > 1
      ? `<select id="hospPick" class="sq-select">${hospList.map(h => `<option value="${h.locationId}"${Number(h.locationId) === Number(hospSelLoc) ? " selected" : ""}>${escapeHtml(h.name)}</option>`).join("")}</select>`
      : (hospList.length === 1 ? `<span class="hosp-title">${escapeHtml(hospList[0].name)}</span>` : "");

    let body;
    if (!hospList.length) {
      body = `<div class="sq-hint">No hospital yet. Designate a Hospital zone and attach a location in Dwarf Fortress, then it will appear here.</div>`;
    } else if (!hospData) {
      body = `<div class="info-message">Loading patients...</div>`;
    } else {
      const patients = Array.isArray(hospData.patients) ? hospData.patients : [];
      const queue = Array.isArray(hospData.queue) ? hospData.queue : [];
      const pRows = patients.length ? patients.map(p => `
          <div class="hosp-patient">
            <span class="sq-mname" style="color:${sqColor(p.professionColor)}">${escapeHtml(p.name || "")}</span>
            <span class="sq-prof">${escapeHtml(p.profession || "")}${Number(p.woundCount) > 0 ? ` · ${p.woundCount} wound(s)` : ""}</span>
            <div class="hosp-flags">${(p.flags || []).map(f => `<span class="hosp-flag">${escapeHtml(f)}</span>`).join("")}${p.inTraction ? `<span class="hosp-flag traction">In traction</span>` : ""}</div>
          </div>`).join("") : `<div class="sq-empty">No patients — everyone is healthy.</div>`;
      const qRows = queue.length ? queue.map(q => `
          <div class="hosp-job">
            <span class="hosp-jobtype">${escapeHtml(q.jobType || "")}</span>
            <span class="sq-prof">${q.patient ? "patient: " + escapeHtml(q.patient) : ""}${q.worker ? " · " + escapeHtml(q.worker) : ""}</span>
          </div>`).join("") : `<div class="sq-subtle">No active treatment jobs.</div>`;
      body = `
        <div class="sq-section-title">Patients (${patients.length})</div>
        <div class="hosp-list">${pRows}</div>
        <div class="sq-section-title">Treatment queue</div>
        <div class="hosp-list">${qRows}</div>`;
    }

    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs"><span class="info-tab active">Hospital</span>${picker}</div>
        <div class="info-body" style="grid-template-columns:1fr;"><div class="hosp-body">${body}</div></div>
        <div class="info-footer"><div>Patients needing care and the doctors treating them, live from your fort.</div></div>
      </div>`;

    const pick = document.getElementById("hospPick");
    if (pick) pick.addEventListener("change", () => { hospSelLoc = Number(pick.value); refreshHospital(); });
  }
