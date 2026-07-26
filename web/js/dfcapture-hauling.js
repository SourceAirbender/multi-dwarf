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

  // ---- Hauling panel: /hauling routes + stops + vehicles ---------------------------------------
  // Create hauling routes and add stops by clicking the map (px/py/w/h tile-grid contract). Each
  // stop's ITEM FILTER (what the cart loads/unloads) is edited here: a stop's settings IS a
  // stockpile_settings, so the same category/item editor the stockpile panel uses is pointed at
  // the stop through /hauling-stop-* (dfcapture.lua hauling_stop_*).
  let haulData = null;
  let haulSel = null;          // selected route id
  let haulStopArmRoute = null; // route id armed for a map-click stop-add
  // Per-stop item-filter editor state (haulItemsStop null when the editor is closed).
  let haulItemsStop = null;    // { id, name } of the stop being edited
  let haulItemsCat = null;     // active category key (SP_CATEGORIES key)
  let haulItemsGroup = null;   // active sub-group key within the category
  let haulItemsCats = {};      // { catKey: enabled } from /hauling-stop-settings-snapshot
  let haulItemsGroupList = []; // [{key,label}] for the active category (from /stockpile-cat-groups)
  let haulItemsList = [];      // [{idx,name,on}] for the active group
  let haulItemsBusy = false;

  const HAUL_STOP_CATS = [
    ["Animals", "animals"], ["Food", "food"], ["Furniture", "furniture"], ["Corpses", "corpses"],
    ["Refuse", "refuse"], ["Stone", "stone"], ["Ammo", "ammo"], ["Coins", "coins"],
    ["Bars/Blocks", "bars"], ["Gems", "gems"], ["Finished goods", "finished"],
    ["Leather", "leather"], ["Cloth", "cloth"], ["Wood", "wood"], ["Weapons", "weapons"],
    ["Armor", "armor"], ["Sheets", "sheets"]
  ];

  function haulStopArmed() { return haulStopArmRoute != null; }

  async function haulFetch(path) {
    const sep = path.includes("?") ? "&" : "?";
    const r = await fetch(`${path}${sep}player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
    const d = await r.json().catch(() => ({}));
    if (!r.ok || d.ok === false) throw new Error(d.error || `${path} failed`);
    return d;
  }
  async function haulPost(path, params) {
    const qs = new URLSearchParams(); qs.set("player", player);
    Object.entries(params || {}).forEach(([k, v]) => qs.set(k, v == null ? "" : String(v)));
    qs.set("t", Date.now());
    const r = await fetch(`${path}?${qs.toString()}`, { method: "POST", cache: "no-store" });
    const d = await r.json().catch(() => ({}));
    if (!r.ok || d.ok === false) throw new Error(d.error || "request failed");
    return d;
  }
  function haulStatus(msg, isErr) {
    const el = document.getElementById("haStatus");
    if (el) { el.textContent = msg || ""; el.className = "sq-status" + (isErr ? " err" : ""); }
  }

  async function openHaulingPanel() {
    setActiveToolbar("hauling");
    clearBuildPlacement(false);
    activeInfoPanel = "hauling";
    clientPanel.className = "visible info-panel";
    if (!clientPanel.querySelector(".ha-body")) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading hauling...</div></div></div>`;
    }
    await refreshHauling();
  }

  async function refreshHauling() {
    try { haulData = await haulFetch("/hauling"); } catch (_) { haulData = null; }
    const routes = (haulData && Array.isArray(haulData.routes)) ? haulData.routes : [];
    if (haulSel != null && !routes.some(r => Number(r.id) === Number(haulSel))) haulSel = null;
    renderHauling();
  }

  // ---- Per-stop item-filter editor (reuses the stockpile category/item machinery) --------------
  async function openStopItems(stop) {
    haulItemsStop = { id: Number(stop.id), name: stop.name || "" };
    haulItemsCat = null; haulItemsGroup = null; haulItemsGroupList = []; haulItemsList = [];
    haulItemsCats = {};
    renderStopItems();
    try {
      const d = await haulFetch(`/hauling-stop-settings-snapshot?route=${haulSel}&stop=${haulItemsStop.id}`);
      haulItemsCats = (d && d.cats) || {};
    } catch (err) { haulStatus(err.message, true); }
    const first = HAUL_STOP_CATS.find(([, k]) => haulItemsCats[k]) || HAUL_STOP_CATS[0];
    await selectStopCat(first[1]);
  }

  async function selectStopCat(cat) {
    haulItemsCat = cat; haulItemsGroup = null; haulItemsGroupList = []; haulItemsList = [];
    renderStopItems();
    try {
      const r = await fetch(`/stockpile-cat-groups?cat=${encodeURIComponent(cat)}&t=${Date.now()}`, { cache: "no-store" });
      const d = await r.json().catch(() => ({}));
      haulItemsGroupList = (d && Array.isArray(d.groups)) ? d.groups : [];
    } catch (_) { haulItemsGroupList = []; }
    haulItemsGroup = haulItemsGroupList[0] ? haulItemsGroupList[0].key : "";
    await loadStopItems();
  }

  async function loadStopItems() {
    if (!haulItemsStop || haulItemsCat == null) return;
    try {
      const d = await haulFetch(`/hauling-stop-items?route=${haulSel}&stop=${haulItemsStop.id}&cat=${encodeURIComponent(haulItemsCat)}&group=${encodeURIComponent(haulItemsGroup || "")}`);
      haulItemsList = (d && Array.isArray(d.items)) ? d.items : [];
    } catch (err) { haulItemsList = []; haulStatus(err.message, true); }
    renderStopItems();
  }

  async function stopItemsPost(path, params) {
    if (haulItemsBusy) return;
    haulItemsBusy = true;
    try { await haulPost(path, params); await loadStopItems(); }
    catch (err) { haulStatus(err.message, true); }
    finally { haulItemsBusy = false; }
  }

  async function applyStopPreset(preset) {
    if (haulItemsBusy || !haulItemsStop) return;
    haulItemsBusy = true;
    try {
      await haulPost("/hauling-stop-preset", {
        route: haulSel, stop: haulItemsStop.id, preset, mode: "set"
      });
      const snapshot = await haulFetch(
        `/hauling-stop-settings-snapshot?route=${haulSel}&stop=${haulItemsStop.id}`);
      haulItemsCats = snapshot?.cats || {};
      const next = preset === "all" || preset === "none"
        ? (HAUL_STOP_CATS.find(([, key]) => haulItemsCats[key]) || HAUL_STOP_CATS[0])[1]
        : preset;
      await selectStopCat(next);
      haulStatus(`Preset applied: ${preset}.`, false);
    } catch (error) {
      haulStatus(error.message || "Preset failed", true);
    } finally {
      haulItemsBusy = false;
    }
  }

  function renderStopItems() {
    if (!haulItemsStop) return;
    const catTabs = HAUL_STOP_CATS.map(([label, key]) =>
      `<button class="sp-cat${key === haulItemsCat ? " active" : ""}${haulItemsCats[key] ? " on" : ""}" data-hi-cat="${key}">${escapeHtml(label)}</button>`).join("");
    const groupTabs = haulItemsGroupList.length > 1 ? haulItemsGroupList.map(g =>
      `<button class="sp-cat${g.key === haulItemsGroup ? " active" : ""}" data-hi-group="${escapeHtml(g.key)}">${escapeHtml(g.label)}</button>`).join("") : "";
    const items = haulItemsList.length ? haulItemsList.map(it =>
      `<label class="hi-item-row"><input type="checkbox" data-hi-item="${it.idx}"${it.on ? " checked" : ""}><span>${escapeHtml(it.name)}</span></label>`).join("")
      : `<div class="sq-subtle">No items in this group.</div>`;
    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs"><span class="info-tab active">Stop items</span><span id="haStatus" class="sq-status"></span></div>
        <div class="info-body" style="grid-template-columns:1fr;">
          <div class="sq-detail-head">
            <button class="sq-btn tiny" data-hi-back>&larr; Back</button>
            <span class="sq-mname">Carried items &mdash; ${escapeHtml(haulItemsStop.name || ("stop " + haulItemsStop.id))}</span>
          </div>
          <div class="sq-section-title">Quick presets</div>
          <div class="hi-preset-row">
            <button class="sp-small-button" data-hi-preset="all">All items</button>
            <button class="sp-small-button" data-hi-preset="none">No items</button>
            ${HAUL_STOP_CATS.map(([label, key]) =>
              `<button class="sp-small-button${haulItemsCats[key] ? " active" : ""}" data-hi-preset="${key}">${escapeHtml(label)}</button>`).join("")}
          </div>
          <div class="hi-cat-row">${catTabs}</div>
          ${groupTabs ? `<div class="hi-cat-row hi-group-row">${groupTabs}</div>` : ""}
          <div class="sq-form-row">
            <button class="sp-small-button" data-hi-all="1">Enable all in group</button>
            <button class="sp-small-button" data-hi-all="0">Disable all in group</button>
          </div>
          <div class="hi-item-list">${items}</div>
        </div>
        <div class="info-footer"><div>The cart carries only the item types checked here. A category with any item checked is on for this stop.</div></div>
      </div>`;
    clientPanel.querySelector("[data-hi-back]").addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation(); haulItemsStop = null; renderHauling();
    });
    clientPanel.querySelectorAll("[data-hi-cat]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation(); selectStopCat(b.dataset.hiCat);
    }));
    clientPanel.querySelectorAll("[data-hi-preset]").forEach(button =>
      button.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        applyStopPreset(button.dataset.hiPreset || "none");
      }));
    clientPanel.querySelectorAll("[data-hi-group]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation(); haulItemsGroup = b.dataset.hiGroup; loadStopItems();
    }));
    clientPanel.querySelectorAll("[data-hi-item]").forEach(b => b.addEventListener("change", e => {
      e.stopPropagation();
      if (b.checked && haulItemsCat) haulItemsCats[haulItemsCat] = true;
      stopItemsPost("/hauling-stop-toggle-item", { route: haulSel, stop: haulItemsStop.id,
        cat: haulItemsCat, group: haulItemsGroup || "", idx: b.dataset.hiItem, on: b.checked ? 1 : 0 });
    }));
    clientPanel.querySelectorAll("[data-hi-all]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      if (b.dataset.hiAll === "1" && haulItemsCat) haulItemsCats[haulItemsCat] = true;
      stopItemsPost("/hauling-stop-toggle-all", { route: haulSel, stop: haulItemsStop.id,
        cat: haulItemsCat, group: haulItemsGroup || "", on: b.dataset.hiAll });
    }));
  }

  function renderHauling() {
    if (haulItemsStop != null) { renderStopItems(); return; }
    const routes = (haulData && Array.isArray(haulData.routes)) ? haulData.routes : [];
    const vehicles = (haulData && Array.isArray(haulData.vehicles)) ? haulData.vehicles : [];
    const route = haulSel != null ? routes.find(r => Number(r.id) === Number(haulSel)) : null;

    const listRows = routes.length ? routes.map(r => `
        <div class="sq-item${Number(r.id) === Number(haulSel) ? " selected" : ""}" data-ha-sel="${r.id}">
          <span class="sq-item-name">${escapeHtml(r.name || "Route")}</span>
          <span class="sq-item-count">${(r.stops || []).length} stops</span>
        </div>`).join("") : `<div class="sq-empty">No hauling routes.</div>`;

    let right;
    if (!route) {
      right = `<div class="sq-hint">Select a route, or create one. A route is an ordered list of stops a minecart or hauler visits.</div>`;
    } else {
      const stops = Array.isArray(route.stops) ? route.stops : [];
      const stopRows = stops.length ? stops.map((s, i) => `
          <div class="fa-row">
            <span class="sq-pos">Stop ${i + 1}</span>
            <span class="sq-mname">${escapeHtml(s.name || "")} <span class="wm-dim">(${s.x}, ${s.y}, ${s.z})</span></span>
            <span class="fa-actions"><button class="sq-btn tiny" data-ha-stop-items="${s.id}">Items&hellip;</button><button class="sq-btn tiny danger" data-ha-stop-remove="${s.id}">Remove</button></span>
          </div>`).join("") : `<div class="sq-subtle">No stops yet. Click "Add stop", then click a tile on the map.</div>`;
      const vids = route.vehicleIds || [];
      right = `
        <div class="sq-detail-head">
          <input id="haName" class="sq-rename" type="text" value="${escapeHtml(route.name || "")}" maxlength="48" spellcheck="false">
          <button class="sq-btn tiny" data-ha-rename>Rename</button>
          <button class="sq-btn tiny danger" data-ha-delete="${route.id}">Delete</button>
        </div>
        <div class="sq-form-row"><button class="sq-btn primary" data-ha-stop-add="${route.id}">+ Add stop (click map)</button></div>
        <div class="sq-section-title">Stops (${stops.length})</div>
        <div class="ha-stops">${stopRows}</div>
        <div class="sq-section-title">Vehicles</div>
        <div class="ha-veh">${vids.length ? `${vids.length} assigned` : `<span class="sq-subtle">No vehicle assigned. Assign a minecart below.</span>`}</div>
        ${vehicles.length ? `<div class="sq-form-row"><select id="haVeh" class="sq-select">${vehicles.map(v => `<option value="${v.vehicleId}">${escapeHtml(v.name || ("cart " + v.vehicleId))}${v.onTrack ? " (on track)" : ""}</option>`).join("")}</select><button class="sq-btn" data-ha-veh-assign="${route.id}">Assign</button></div>` : ""}`;
    }

    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs"><span class="info-tab active">Hauling</span><span id="haStatus" class="sq-status"></span></div>
        <div class="info-body" style="grid-template-columns:1fr;">
          <div class="sq-cols">
            <div class="sq-left"><div class="sq-list-head"><button class="sq-btn primary" data-ha-new>+ New route</button></div><div class="sq-list ha-body">${listRows}</div></div>
            <div class="sq-right">${right}</div>
          </div>
        </div>
        <div class="info-footer"><div>Stops are placed by clicking the map. Use a stop's Items button to choose what the cart carries.</div></div>
      </div>`;

    clientPanel.querySelectorAll("[data-ha-sel]").forEach(x => x.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation(); haulSel = Number(x.dataset.haSel); renderHauling();
    }));
    const nb = clientPanel.querySelector("[data-ha-new]");
    if (nb) nb.addEventListener("click", async e => { e.preventDefault(); e.stopPropagation();
      try { const d = await haulPost("/hauling-route-create", { name: "New Route" }); if (d.id != null) haulSel = Number(d.id); await refreshHauling(); haulStatus("Route created.", false); }
      catch (err) { haulStatus(err.message, true); } });
    const rn = clientPanel.querySelector("[data-ha-rename]");
    if (rn) rn.addEventListener("click", async e => { e.preventDefault(); e.stopPropagation();
      const inp = document.getElementById("haName");
      try { await haulPost("/hauling-route-rename", { id: haulSel, name: inp ? inp.value.trim() : "" }); await refreshHauling(); haulStatus("Renamed.", false); }
      catch (err) { haulStatus(err.message, true); } });
    clientPanel.querySelectorAll("[data-ha-delete]").forEach(x => x.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation(); if (!confirm("Delete this route?")) return;
      try { await haulPost("/hauling-route-remove", { id: x.dataset.haDelete }); haulSel = null; await refreshHauling(); haulStatus("Deleted.", false); }
      catch (err) { haulStatus(err.message, true); } }));
    clientPanel.querySelectorAll("[data-ha-stop-remove]").forEach(x => x.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await haulPost("/hauling-stop-remove", { route: haulSel, stop: x.dataset.haStopRemove }); await refreshHauling(); haulStatus("Stop removed.", false); }
      catch (err) { haulStatus(err.message, true); } }));
    clientPanel.querySelectorAll("[data-ha-stop-items]").forEach(x => x.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      const rt = (haulData && haulData.routes || []).find(r => Number(r.id) === Number(haulSel));
      const st = rt && (rt.stops || []).find(s => Number(s.id) === Number(x.dataset.haStopItems));
      if (st) openStopItems(st);
    }));
    const add = clientPanel.querySelector("[data-ha-stop-add]");
    if (add) add.addEventListener("click", e => { e.preventDefault(); e.stopPropagation(); haulArmStopAdd(Number(add.dataset.haStopAdd)); });
    const va = clientPanel.querySelector("[data-ha-veh-assign]");
    if (va) va.addEventListener("click", async e => { e.preventDefault(); e.stopPropagation();
      const sel = document.getElementById("haVeh");
      try { await haulPost("/hauling-vehicle-assign", { route: va.dataset.haVehAssign, vehicle: sel ? sel.value : -1 }); await refreshHauling(); haulStatus("Vehicle assigned.", false); }
      catch (err) { haulStatus(err.message, true); } });
    const inp = document.getElementById("haName");
    if (inp) { inp.addEventListener("click", ev => ev.stopPropagation()); inp.addEventListener("keydown", ev => ev.stopPropagation()); }
  }

  // ---- Map click adds a stop at the clicked tile ----------------------------------------------
  function haulArmStopAdd(routeId) {
    haulStopArmRoute = routeId;
    clientPanel.className = "";
    let b = document.getElementById("haStopBanner");
    if (!b) { b = document.createElement("div"); b.id = "haStopBanner"; document.body.appendChild(b); }
    b.textContent = "Click a tile to add a hauling stop  ·  Esc to finish";
    b.className = "visible";
    document.addEventListener("keydown", haulStopKey, true);
  }
  function haulDisarmStop() {
    haulStopArmRoute = null;
    const b = document.getElementById("haStopBanner"); if (b) b.className = "";
    document.removeEventListener("keydown", haulStopKey, true);
  }
  function haulStopKey(e) { if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); haulDisarmStop(); openHaulingPanel(); } }
  function haulConsumeStopClick(event) {
    if (haulStopArmRoute == null) return false;
    const route = haulStopArmRoute;
    const pixel = imagePixelFromEvent(event);
    if (pixel) haulPost("/hauling-stop-add", { route, px: pixel.x, py: pixel.y, w: pixel.w, h: pixel.h })
      .then(() => haulStatus("Stop added.", false)).catch(() => {});
    return true;
  }
