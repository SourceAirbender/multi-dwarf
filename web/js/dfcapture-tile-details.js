// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

(function () {
  "use strict";

  const { esc, jsonFetch } = window.dfDomainUi;

  async function showTileOccupants(tile) {
    if (!tile) return;
    const params = new URLSearchParams({ x: tile.x, y: tile.y, z: tile.z });
    const data = await jsonFetch(`/tile-occupants?${params}`);
    const rows = Array.isArray(data.occupants) ? data.occupants : [];
    selection.className = "visible occupant-panel";
    selection.innerHTML = `
      <button class="unit-close-button" data-occupants-close title="Close">X</button>
      <h1>Tile occupants</h1>
      <div class="line">Tile: ${esc(tile.x)}, ${esc(tile.y)}, ${esc(tile.z)}</div>
      <div class="occupant-list">${rows.length ? rows.map((row, index) => `
        <button type="button" data-occupant-index="${index}">
          <span>${esc(row.name)}</span><small>${esc(row.kind)}</small>
        </button>`).join("") : `<div class="info-message">Nothing selectable on this tile.</div>`}</div>`;
    selection.querySelector("[data-occupants-close]")?.addEventListener("click", closeSelection);
    selection.querySelectorAll("[data-occupant-index]").forEach(button => {
      button.addEventListener("click", async event => {
        const row = rows[Number(event.currentTarget.dataset.occupantIndex)];
        if (!row) return;
        if (row.kind === "unit" && typeof openUnitById === "function") openUnitById(row.id);
        else if (row.kind === "item" && typeof openItemPanel === "function") openItemPanel(row.id);
        else if (row.kind === "stockpile" && typeof openStockpilePanel === "function") openStockpilePanel(row.id);
        else if (row.kind === "zone" && typeof openZonePanel === "function") openZonePanel(row.id);
        else if (row.kind === "workshop" && typeof openWorkshopPanel === "function") openWorkshopPanel(row.id);
        else if (row.kind === "building" && typeof openBuildingPanel === "function") openBuildingPanel(row.id);
        else if (row.kind === "engraving") await showEngraving(tile);
      });
    });
  }

  async function showEngraving(tile) {
    const params = new URLSearchParams({ x: tile.x, y: tile.y, z: tile.z });
    const engraving = await jsonFetch(`/engraving-info?${params}`);
    selection.className = "visible engraving-panel";
    selection.innerHTML = `
      <button class="unit-close-button" data-engraving-close title="Close">X</button>
      <div class="kind">engraving</div><h1>${esc(engraving.qualityName)} engraving</h1>
      ${engraving.description ? `<div class="artwork-prose">${esc(engraving.description)}</div>` : ""}
      <div class="line">Surface: ${esc(engraving.surface)}</div>
      <div class="line">Artist: ${esc(engraving.artist || "Unknown")}</div>
      ${engraving.artName ? `<div class="line">Title: ${esc(engraving.artName)}</div>` : ""}
      <div class="line">Artwork record: ${esc(engraving.artId)} / ${esc(engraving.artSubId)}</div>
      <div class="line">Tile: ${esc(tile.x)}, ${esc(tile.y)}, ${esc(tile.z)}</div>`;
    selection.querySelector("[data-engraving-close]")?.addEventListener("click", closeSelection);
  }

  async function installTrafficCosts() {
    const menu = document.getElementById("trafficSubmenu");
    if (!menu || menu.querySelector(".traffic-cost-editor")) return;
    const editor = document.createElement("div");
    editor.className = "traffic-cost-editor";
    menu.appendChild(editor);
    try {
      const data = await jsonFetch("/traffic-costs");
      const costs = data.costs || {};
      editor.innerHTML = ["high", "normal", "low", "restricted"].map(kind => `
        <label title="${kind} traffic pathfinding cost">${kind.slice(0, 2).toUpperCase()}
          <input type="number" min="1" max="10000" value="${Number(costs[kind]) || 1}"
            data-traffic-cost="${kind}">
        </label>`).join("");
      editor.querySelectorAll("[data-traffic-cost]").forEach(input => {
        input.addEventListener("change", async () => {
          const params = new URLSearchParams({ [input.dataset.trafficCost]: input.value });
          await jsonFetch(`/traffic-costs?${params}`, { method: "POST" }).catch(() => {});
        });
      });
    } catch (_) {
      editor.remove();
    }
  }

  window.showTileOccupants = showTileOccupants;
  window.showEngraving = showEngraving;
  addEventListener("DOMContentLoaded", installTrafficCosts);
})();
