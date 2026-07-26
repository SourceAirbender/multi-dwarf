// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

(function () {
  "use strict";

  const esc = value => typeof escapeHtml === "function"
    ? escapeHtml(String(value ?? ""))
    : String(value ?? "").replace(/[&<>"']/g, ch => ({
        "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
      }[ch]));

  async function jsonFetch(path, options) {
    const response = await fetch(path, { cache: "no-store", ...(options || {}) });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false)
      throw new Error(data.error || data.err || `${path} failed`);
    return data;
  }

  function openCoopWindow(title, body, footer = "") {
    if (typeof clearBuildPlacement === "function") clearBuildPlacement(false);
    clientPanel.className = "visible info-panel coop-info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="coop-window-head" data-panel-drag-handle>
          <strong>${esc(title)}</strong>
          <button type="button" class="sq-btn tiny" data-coop-close title="Close">X</button>
        </div>
        <div class="info-body coop-window-body" style="grid-template-columns:1fr;">${body}</div>
        ${footer ? `<div class="info-footer">${esc(footer)}</div>` : ""}
      </div>`;
    clientPanel.querySelector("[data-coop-close]")?.addEventListener("click", event => {
      event.preventDefault();
      clientPanel.className = "";
      clientPanel.innerHTML = "";
      if (typeof focusPage === "function") focusPage();
    });
  }

  // Native mega-announcements block DF globally. Mirror the current popup into every browser
  // without opening the announcement center or navigating the host's viewscreen.
  let popupSignature = "";
  let popupBusy = false;

  function popupHost() {
    let host = document.getElementById("nativePopupSync");
    if (!host) {
      host = document.createElement("div");
      host.id = "nativePopupSync";
      host.setAttribute("aria-live", "assertive");
      document.body.appendChild(host);
    }
    return host;
  }

  function renderNativePopup(data) {
    const host = popupHost();
    const popups = Array.isArray(data?.popups) ? data.popups : [];
    const popup = popups[0];
    if (!popup) {
      host.className = "";
      host.replaceChildren();
      popupSignature = "";
      return;
    }
    const signature = `${popup.id}:${JSON.stringify(popup.text || [])}`;
    if (signature === popupSignature && host.classList.contains("visible")) return;
    popupSignature = signature;
    host.className = "visible";
    host.innerHTML = `
      <div class="native-popup-window" role="alertdialog" aria-modal="true">
        <div class="native-popup-title">Dwarf Fortress</div>
        <div class="native-popup-copy">${(popup.text || []).map(line =>
          `<div>${esc(line)}</div>`).join("")}</div>
        <button type="button" data-popup-dismiss="${Number(popup.id)}">Dismiss</button>
      </div>`;
    host.querySelector("[data-popup-dismiss]")?.addEventListener("click", async event => {
      event.preventDefault();
      const id = Number(event.currentTarget.dataset.popupDismiss);
      if (!Number.isFinite(id) || popupBusy) return;
      popupBusy = true;
      try {
        await jsonFetch(`/popup/dismiss?id=${id}`, { method: "POST" });
        popupSignature = "";
        await pollNativePopup();
      } catch (_) {
        popupSignature = "";
      } finally {
        popupBusy = false;
      }
    });
  }

  async function pollNativePopup() {
    try { renderNativePopup(await jsonFetch(`/popup?t=${Date.now()}`)); }
    catch (_) {}
  }

  // Camera locations can use DF's 16 save-persisted hotkeys or a private browser store.
  // Keep the stores separate so switching modes can never overwrite the other set.
  const hotkeyModeKey = "dfcapture.cameraLocations.mode";
  let hotkeys = [];

  function useFortressHotkeys() {
    try { return localStorage.getItem(hotkeyModeKey) !== "local"; }
    catch (_) { return true; }
  }

  function localHotkeyKey() {
    return `dfcapture.cameraLocations.local.${String(player || "player")}`;
  }

  function loadLocalHotkeys() {
    try {
      const saved = JSON.parse(localStorage.getItem(localHotkeyKey()) || "[]");
      return Array.isArray(saved) ? saved.filter(entry =>
        entry && Number.isInteger(Number(entry.slot)) &&
        Number(entry.slot) >= 0 && Number(entry.slot) < 16) : [];
    } catch (_) {
      return [];
    }
  }

  function saveLocalHotkeys(entries) {
    localStorage.setItem(localHotkeyKey(), JSON.stringify(entries));
  }

  async function reloadHotkeys() {
    if (!useFortressHotkeys()) {
      hotkeys = loadLocalHotkeys();
      return;
    }
    try {
      const data = await jsonFetch(`/hotkeys?t=${Date.now()}`);
      hotkeys = Array.isArray(data.hotkeys) ? data.hotkeys : [];
    } catch (_) {
      hotkeys = [];
    }
  }

  async function writeHotkey(slot, action, extra = {}) {
    if (!useFortressHotkeys()) {
      const entries = loadLocalHotkeys();
      const index = entries.findIndex(entry => Number(entry.slot) === slot);
      if (action === "set") {
        const prior = index >= 0 ? entries[index] : null;
        const entry = {
          slot,
          set: true,
          name: prior?.name || `Location ${slot + 1}`,
          x: Number(extra.x),
          y: Number(extra.y),
          z: Number(extra.z)
        };
        if (index >= 0) entries[index] = entry;
        else entries.push(entry);
      } else if (action === "clear") {
        if (index >= 0) entries.splice(index, 1);
      } else if (action === "rename" && index >= 0) {
        entries[index].name = String(extra.name || `Location ${slot + 1}`);
      }
      saveLocalHotkeys(entries);
      hotkeys = entries;
      return;
    }
    const params = new URLSearchParams({ slot: String(slot), action, ...extra });
    await jsonFetch(`/hotkey-action?${params}`, { method: "POST" });
    await reloadHotkeys();
  }

  function renderFortressHotkeys(row) {
    const shared = useFortressHotkeys();
    row.title = shared
      ? "Shared fortress camera locations: click to save or jump; right-click clears"
      : "Private browser camera locations: click to save or jump; right-click clears";
    row.innerHTML = Array.from({ length: 16 }, (_, slot) => {
      const hotkey = hotkeys.find(entry => Number(entry.slot) === slot);
      const set = !!hotkey?.set;
      const name = hotkey?.name || `Location ${slot + 1}`;
      return `<button class="bm-slot${set ? " set" : ""}" data-fort-hotkey="${slot}"
        title="${set ? `Jump to ${esc(name)}; right-click clears; double-click renames`
          : `Save current camera as ${esc(name)}`}">${slot + 1}</button>`;
    }).join("");
    row.querySelectorAll("[data-fort-hotkey]").forEach(button => {
      const slot = Number(button.dataset.fortHotkey);
      button.addEventListener("click", async event => {
        event.preventDefault(); event.stopPropagation();
        const hotkey = hotkeys.find(entry => Number(entry.slot) === slot);
        if (hotkey?.set) {
          cameraGoto(hotkey.x, hotkey.y, hotkey.z);
        } else {
          const camera = currentHud?.camera;
          if (!camera) return;
          await writeHotkey(slot, "set", {
            x: String(camera.x), y: String(camera.y), z: String(camera.z)
          }).catch(() => {});
          renderFortressHotkeys(row);
        }
      });
      button.addEventListener("contextmenu", async event => {
        event.preventDefault(); event.stopPropagation();
        await writeHotkey(slot, "clear").catch(() => {});
        renderFortressHotkeys(row);
      });
      button.addEventListener("dblclick", async event => {
        event.preventDefault(); event.stopPropagation();
        const hotkey = hotkeys.find(entry => Number(entry.slot) === slot);
        if (!hotkey?.set) return;
        const name = prompt("Location name", hotkey.name || `Location ${slot + 1}`);
        if (name == null) return;
        await writeHotkey(slot, "rename", { name }).catch(() => {});
        renderFortressHotkeys(row);
      });
    });
  }

  async function installFortressHotkeys() {
    const oldRow = document.getElementById("bmRow");
    const trigger = document.getElementById("recenterLocationsBtn");
    if (!oldRow || !trigger) return;
    const row = oldRow.cloneNode(false);
    row.id = "bmRow";
    row.hidden = true;
    oldRow.replaceWith(row);
    await reloadHotkeys();
    renderFortressHotkeys(row);
    const cleanTrigger = trigger.cloneNode(true);
    trigger.replaceWith(cleanTrigger);
    cleanTrigger.addEventListener("click", event => {
      event.preventDefault(); event.stopPropagation();
      row.hidden = !row.hidden;
      if (!row.hidden) reloadHotkeys().then(() => renderFortressHotkeys(row));
    });
    document.addEventListener("pointerdown", event => {
      if (!row.hidden && !event.target.closest("#bmRow, #recenterLocationsBtn"))
        row.hidden = true;
    });

    const modeRow = document.getElementById("setFortressHotkeys");
    const modeDescription = document.getElementById("hotkeyModeDescription");
    const syncModeSetting = () => {
      const shared = useFortressHotkeys();
      modeRow?.classList.toggle("on", shared);
      if (modeDescription) modeDescription.textContent = shared
        ? "Using the fort's 16 shared locations stored in this save."
        : "Using 16 private locations stored only for this player in this browser.";
    };
    syncModeSetting();
    modeRow?.addEventListener("click", async event => {
      event.preventDefault();
      try {
        localStorage.setItem(hotkeyModeKey, useFortressHotkeys() ? "local" : "fortress");
      } catch (_) {}
      syncModeSetting();
      await reloadHotkeys();
      renderFortressHotkeys(row);
    });
  }

  function attributionRows(data) {
    const kinds = ["buildings", "stockpiles", "zones", "orders"];
    const players = new Map();
    kinds.forEach(kind => {
      Object.entries(data?.[kind] || {}).forEach(([id, owner]) => {
        const name = String(owner || "unknown");
        if (!players.has(name)) players.set(name, { total: 0, ids: {} });
        const row = players.get(name);
        row.total++;
        (row.ids[kind] ||= []).push(id);
      });
    });
    return Array.from(players.entries()).sort((a, b) => b[1].total - a[1].total);
  }

  async function openAttributionPanel() {
    openCoopWindow("Player activity", `<div class="info-message">Loading...</div>`);
    try {
      const data = await jsonFetch(`/attrib?t=${Date.now()}`);
      const rows = attributionRows(data);
      const body = rows.length ? rows.map(([name, stats]) => `
        <div class="coop-data-row">
          <strong>${esc(name)}</strong>
          <span>${stats.total} actions</span>
          <span class="coop-detail">${["buildings", "stockpiles", "zones", "orders"]
            .filter(kind => stats.ids[kind]?.length)
            .map(kind => `${kind}: ${stats.ids[kind].length}`).join(" | ")}</span>
        </div>`).join("") : `<div class="info-message">No attributed browser actions yet.</div>`;
      openCoopWindow("Player activity", body,
        "Records browser-created buildings, stockpiles, zones, and work orders for this session.");
    } catch (error) {
      openCoopWindow("Player activity", `<div class="info-message">${esc(error.message)}</div>`);
    }
  }

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
    openCoopWindow("Obligations", `<div class="info-message">Loading...</div>`);
    const [nobles, petitions] = await Promise.all([
      jsonFetch(`/nobles?player=${encodeURIComponent(player)}&t=${Date.now()}`).catch(() => ({})),
      jsonFetch(`/petitions?player=${encodeURIComponent(player)}&t=${Date.now()}`).catch(() => ({}))
    ]);
    const mandates = mandateRows(nobles);
    const agreements = Array.isArray(petitions?.petitions) ? petitions.petitions : [];
    const section = (title, rows, renderer) => `
      <h3 class="coop-section-title">${esc(title)}</h3>
      ${rows.length ? rows.map(renderer).join("") : `<div class="info-message">None.</div>`}`;
    openCoopWindow("Obligations",
      section("Noble mandates", mandates, row => `<div class="coop-data-row">
        <strong>${esc(row.title)}</strong><span>${esc(row.owner)}</span>
        <span class="coop-detail">${esc(row.detail)}</span></div>`) +
      section("Petitions and agreements", agreements, row => `<div class="coop-data-row">
        <strong>${esc(row.purpose || row.summary || "Agreement")}</strong>
        <span>${esc(row.petitioner || "")}</span>
        <span class="coop-detail">${esc(row.summary || "")} ${row.pending ? "(pending)" : "(accepted)"}</span>
      </div>`),
      "Mandates and petition agreements are combined here so obligations are not hidden across panels.");
  }

  function missionRows(data) {
    const active = Array.isArray(data.active) ? data.active : [];
    const stuck = Array.isArray(data.stuckSquads) ? data.stuckSquads : [];
    let html = `<h3 class="coop-section-title">Active missions</h3>`;
    html += active.length ? active.map(mission => `
      <div class="coop-data-row">
        <strong>${esc(mission.goal || "Mission")} - ${esc(mission.targetSite || "unknown target")}</strong>
        <span>${(mission.squads || []).map(s => esc(s.name || `Squad ${s.id}`)).join(", ") || "No squads"}</span>
        <span class="coop-detail">${esc(mission.targetKind || "")} ${esc(mission.targetName || "")}
          ${mission.returning ? "| returning" : ""}${mission.stuck ? "| stranded" : ""}</span>
      </div>`).join("") : `<div class="info-message">No active missions.</div>`;
    html += `<h3 class="coop-section-title">Stranded squads</h3>`;
    html += stuck.length ? stuck.map(squad => `<div class="coop-data-row">
      <strong>${esc(squad.squadName || `Squad ${squad.squadId}`)}</strong>
      <span>Army ${esc(squad.armyId)}</span></div>`).join("") :
      `<div class="info-message">No stranded squads.</div>`;
    if (data.rescue) {
      html += `<div class="coop-mission-rescue">
        <span>${esc(data.rescue.reason || "")}</span>
        <button type="button" class="sq-btn" data-mission-rescue
          ${data.rescue.available ? "" : "disabled"}>Rescue stranded squads</button>
      </div>`;
    }
    return html;
  }

  async function openMissionsPanel() {
    openCoopWindow("Missions", `<div class="info-message">Loading...</div>`);
    try {
      const data = await jsonFetch(`/missions?player=${encodeURIComponent(player)}&t=${Date.now()}`);
      openCoopWindow("Missions", missionRows(data),
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
      openCoopWindow("Missions", `<div class="info-message">${esc(error.message)}</div>`);
    }
  }

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
      <div class="line">Surface: ${esc(engraving.surface)}</div>
      <div class="line">Artist: ${esc(engraving.artist || "Unknown")}</div>
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

  window.openAttributionPanel = openAttributionPanel;
  window.openObligationsPanel = openObligationsPanel;
  window.openMissionsPanel = openMissionsPanel;
  window.showTileOccupants = showTileOccupants;
  window.showEngraving = showEngraving;

  addEventListener("DOMContentLoaded", () => {
    installFortressHotkeys();
    installTrafficCosts();
    pollNativePopup();
    setInterval(pollNativePopup, 600);
    document.getElementById("openAnalyticsRow")?.addEventListener("click", openAttributionPanel);
    document.getElementById("openObligationsRow")?.addEventListener("click", openObligationsPanel);
  });
})();
