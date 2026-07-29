// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

(function () {
  "use strict";

  const { esc, jsonFetch, openWindow } = window.dfDomainUi;

  function mandateRows(nobles) {
    const rows = [];
    (nobles?.positions || []).forEach(position => {
      const mandates = Array.isArray(position.mandateRows) ? position.mandateRows : [];
      mandates.forEach(mandate => rows.push({
        title: mandate.item || mandate.title || mandate.type || "Mandate",
        detail: mandate.deadline || mandate.status || "",
        owner: position.holder || position.name || ""
      }));
      const count = Number(position.mandates || 0);
      if (!mandates.length && count > 0) rows.push({
        title: `${count} active mandate${count === 1 ? "" : "s"}`,
        detail: "", owner: position.holder || position.name || ""
      });
    });
    return rows;
  }

  async function openObligationsPanel() {
    openWindow("Obligations", `<div class="info-message">Loading...</div>`);
    const [nobles, petitions] = await Promise.all([
      jsonFetch(`/nobles?player=${encodeURIComponent(player)}&t=${Date.now()}`).catch(() => ({})),
      jsonFetch(`/petitions?player=${encodeURIComponent(player)}&t=${Date.now()}`).catch(() => ({}))
    ]);
    const mandates = mandateRows(nobles);
    const agreements = Array.isArray(petitions?.petitions) ? petitions.petitions : [];
    const section = (title, rows, renderer) => `
      <h3 class="domain-section-title">${esc(title)}</h3>
      ${rows.length ? rows.map(renderer).join("") : `<div class="info-message">None.</div>`}`;
    openWindow("Obligations",
      section("Noble mandates", mandates, row => `<div class="domain-data-row">
        <strong>${esc(row.title)}</strong><span>${esc(row.owner)}</span>
        <span class="domain-detail">${esc(row.detail)}</span></div>`) +
      section("Petitions and agreements", agreements, row => `<div class="domain-data-row">
        <strong>${esc(row.purpose || row.summary || "Agreement")}</strong>
        <span>${esc(row.petitioner || "")}</span>
        <span class="domain-detail">${esc(row.summary || "")} ${row.pending ? "(pending)" : "(accepted)"}</span>
      </div>`),
      "Mandates and petition agreements are combined here so obligations are not hidden across panels.");
  }

  function missionRows(data) {
    const active = Array.isArray(data.active) ? data.active : [];
    const stuck = Array.isArray(data.stuckSquads) ? data.stuckSquads : [];
    let html = `<h3 class="domain-section-title">Active missions</h3>`;
    html += active.length ? active.map(mission => `
      <div class="domain-data-row">
        <strong>${esc(mission.goal || "Mission")} - ${esc(mission.targetSite || "unknown target")}</strong>
        <span>${(mission.squads || []).map(s => esc(s.name || `Squad ${s.id}`)).join(", ") || "No squads"}</span>
        <span class="domain-detail">${esc(mission.targetKind || "")} ${esc(mission.targetName || "")}
          ${mission.returning ? "| returning" : ""}${mission.stuck ? "| stranded" : ""}</span>
      </div>`).join("") : `<div class="info-message">No active missions.</div>`;
    html += `<h3 class="domain-section-title">Stranded squads</h3>`;
    html += stuck.length ? stuck.map(squad => `<div class="domain-data-row">
      <strong>${esc(squad.squadName || `Squad ${squad.squadId}`)}</strong>
      <span>Army ${esc(squad.armyId)}</span></div>`).join("") :
      `<div class="info-message">No stranded squads.</div>`;
    if (data.rescue) {
      html += `<div class="domain-mission-rescue">
        <span>${esc(data.rescue.reason || "")}</span>
        <button type="button" class="sq-btn" data-mission-rescue
          ${data.rescue.available ? "" : "disabled"}>Rescue stranded squads</button>
      </div>`;
    }
    return html;
  }

  async function openMissionsPanel() {
    openWindow("Missions", `<div class="info-message">Loading...</div>`);
    try {
      const data = await jsonFetch(`/missions?player=${encodeURIComponent(player)}&t=${Date.now()}`);
      openWindow("Missions", missionRows(data),
        data.create?.supported ? "" : (data.create?.reason || "Mission creation remains on the host world screen."));
      clientPanel.querySelector("[data-mission-rescue]")?.addEventListener("click", async event => {
        event.currentTarget.disabled = true;
        try {
          await jsonFetch(`/mission-rescue?player=${encodeURIComponent(player)}`, { method: "POST" });
          await openMissionsPanel();
        } catch (error) {
          event.currentTarget.disabled = false;
          event.currentTarget.title = error.message;
        }
      });
    } catch (error) {
      openWindow("Missions", `<div class="info-message">${esc(error.message)}</div>`);
    }
  }

  window.openObligationsPanel = openObligationsPanel;
  window.openMissionsPanel = openMissionsPanel;
  addEventListener("DOMContentLoaded", () => {
    document.getElementById("openObligationsRow")?.addEventListener("click", openObligationsPanel);
  });
})();
