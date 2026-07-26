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

  // ---- Burrows panel: /burrows + create/rename/delete/action/symbol/unit/paint ----------------
  // Manage fort burrows and PAINT their tiles by dragging on the map (the same px/py/w/h tile-grid
  // contract as /designate). controls-placement.js's pointerdown/up call burrowPaint* when a paint
  // is armed, so a drag paints a rectangle into the selected burrow.
  let burrowList = [];
  let burrowSel = null;        // selected burrow id (detail shown)
  let burrowDetail = null;
  let burrowPalette = [];
  let burrowPaintId = null;    // burrow currently armed for map painting
  let burrowPaintMode = "add"; // add | remove
  let burrowPaintAnchor = null;
  let burrowSeq = -1;

  function burrowPaintArmed() { return burrowPaintId != null; }

  async function burrowFetch(path) {
    const sep = path.includes("?") ? "&" : "?";
    const r = await fetch(`${path}${sep}player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
    const d = await r.json().catch(() => ({}));
    if (!r.ok || d.ok === false) throw new Error(d.error || `${path} failed`);
    return d;
  }
  async function burrowPost(path, params) {
    const qs = new URLSearchParams(); qs.set("player", player);
    Object.entries(params || {}).forEach(([k, v]) => qs.set(k, v == null ? "" : String(v)));
    qs.set("t", Date.now());
    const r = await fetch(`${path}?${qs.toString()}`, { method: "POST", cache: "no-store" });
    const d = await r.json().catch(() => ({}));
    if (!r.ok || d.ok === false) throw new Error(d.error || "request failed");
    return d;
  }
  function burrowStatus(msg, isErr) {
    const el = document.getElementById("brStatus");
    if (el) { el.textContent = msg || ""; el.className = "sq-status" + (isErr ? " err" : ""); }
  }

  async function openBurrowsPanel() {
    setActiveToolbar("burrows");
    clearBuildPlacement(false);
    activeInfoPanel = "burrows";
    clientPanel.className = "visible info-panel";
    if (!clientPanel.querySelector(".br-body")) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading burrows...</div></div></div>`;
    }
    await refreshBurrows();
  }

  async function refreshBurrows() {
    try {
      const d = await burrowFetch(burrowSel != null ? `/burrows?detail=${burrowSel}` : "/burrows");
      burrowList = Array.isArray(d.burrows) ? d.burrows : [];
      burrowPalette = Array.isArray(d.palette) ? d.palette : burrowPalette;
      burrowSeq = Number(d.seq);
      if (burrowSel != null && !burrowList.some(b => Number(b.id) === Number(burrowSel))) burrowSel = null;
      burrowDetail = burrowSel != null ? burrowList.find(b => Number(b.id) === Number(burrowSel)) : null;
    } catch (_) { burrowList = []; }
    renderBurrows();
  }

  function burrowSwatch(b) {
    const fg = b.fgRgb || {}, bg = b.bgRgb || {};
    return `background:rgb(${bg.r || 0},${bg.g || 0},${bg.b || 0});color:rgb(${fg.r != null ? fg.r : 200},${fg.g != null ? fg.g : 200},${fg.b != null ? fg.b : 200})`;
  }

  function renderBurrows() {
    const rows = burrowList.length ? burrowList.map(b => `
        <div class="sq-item${Number(b.id) === Number(burrowSel) ? " selected" : ""}" data-br-sel="${b.id}">
          <span class="sq-emblem" style="${burrowSwatch(b)}">&#9632;</span>
          <span class="sq-item-name">${escapeHtml(b.name || "Burrow")}${b.suspended ? ' <span class="ann-badge box">off</span>' : ""}</span>
          <span class="sq-item-count">${Number(b.count) || 0}t · ${Number(b.memberCount) || 0}u</span>
        </div>`).join("") : `<div class="sq-empty">No burrows.</div>`;

    let right;
    const b = burrowDetail;
    if (!b) {
      right = `<div class="sq-hint">Select a burrow, or create one. A burrow is a named area you can restrict dwarves and workshops to.</div>`;
    } else {
      const members = Array.isArray(b.members) ? b.members : [];
      right = `
        <div class="sq-detail-head">
          <input id="brName" class="sq-rename" type="text" value="${escapeHtml(b.name || "")}" maxlength="48" spellcheck="false">
          <button class="sq-btn tiny" data-br-rename>Rename</button>
          <button class="sq-btn tiny danger" data-br-delete="${b.id}">Delete</button>
        </div>
        <div class="sq-form-row">
          <button class="sq-btn primary" data-br-paint="add">Paint tiles</button>
          <button class="sq-btn" data-br-paint="remove">Erase tiles</button>
          <span class="sq-subtle">${Number(b.count) || 0} tiles</span>
        </div>
        <div class="sq-form-row">
          <button class="sq-btn tiny" data-br-action="${b.suspended ? "resume" : "suspend"}">${b.suspended ? "Resume" : "Suspend"}</button>
          <button class="sq-btn tiny" data-br-action="${b.civAlert ? "civalert-off" : "civalert-on"}">${b.civAlert ? "Clear civ alert" : "Set civ alert"}</button>
          <button class="sq-btn tiny" data-br-action="${b.limitWorkshops ? "workshops-all" : "workshops-limit"}">${b.limitWorkshops ? "All workshops" : "Limit workshops"}</button>
        </div>
        <div class="sq-section-title">Symbol</div>
        <div class="br-palette">${burrowPalette.map((c, i) =>
          `<button class="br-pal${Number(b.symbolIndex) === i ? " sel" : ""}" data-br-sym="${i}" style="background:rgb(${c.r},${c.g},${c.b})" title="colour ${i}"></button>`).join("")}</div>
        <div class="sq-section-title">Members (${members.length})</div>
        <div class="br-members">${members.length ? members.map(m =>
          `<div class="sq-cand"><span class="sq-mname" style="color:${sqColor(m.professionColor)}">unit ${m.unitId}</span><span class="sq-prof">${escapeHtml(m.profession || "")}</span></div>`).join("")
          : `<div class="sq-subtle">No dwarves assigned. Assign from a unit's panel, or paint the area they live in.</div>`}</div>`;
    }

    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs"><span class="info-tab active">Burrows</span><span id="brStatus" class="sq-status"></span></div>
        <div class="info-body" style="grid-template-columns:1fr;">
          <div class="sq-cols">
            <div class="sq-left"><div class="sq-list-head"><button class="sq-btn primary" data-br-new>+ New burrow</button></div><div class="sq-list br-body">${rows}</div></div>
            <div class="sq-right">${right}</div>
          </div>
        </div>
        <div class="info-footer"><div>Paint drags a rectangle into the burrow. Changes apply to the host fort immediately.</div></div>
      </div>`;

    clientPanel.querySelectorAll("[data-br-sel]").forEach(x => x.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation(); burrowSel = Number(x.dataset.brSel); refreshBurrows();
    }));
    const nb = clientPanel.querySelector("[data-br-new]");
    if (nb) nb.addEventListener("click", async e => { e.preventDefault(); e.stopPropagation();
      try { const d = await burrowPost("/burrow-create", { name: "New Burrow" }); if (d.id != null) burrowSel = Number(d.id); await refreshBurrows(); burrowStatus("Burrow created.", false); }
      catch (err) { burrowStatus(err.message, true); } });
    const rn = clientPanel.querySelector("[data-br-rename]");
    if (rn) rn.addEventListener("click", async e => { e.preventDefault(); e.stopPropagation();
      const inp = document.getElementById("brName");
      try { await burrowPost("/burrow-rename", { id: burrowSel, name: inp ? inp.value.trim() : "" }); await refreshBurrows(); burrowStatus("Renamed.", false); }
      catch (err) { burrowStatus(err.message, true); } });
    clientPanel.querySelectorAll("[data-br-delete]").forEach(x => x.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      if (!confirm("Delete this burrow?")) return;
      try { await burrowPost("/burrow-delete", { id: x.dataset.brDelete }); burrowSel = null; await refreshBurrows(); burrowStatus("Deleted.", false); }
      catch (err) { burrowStatus(err.message, true); } }));
    clientPanel.querySelectorAll("[data-br-action]").forEach(x => x.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await burrowPost("/burrow-action", { id: burrowSel, action: x.dataset.brAction }); await refreshBurrows(); burrowStatus("Updated.", false); }
      catch (err) { burrowStatus(err.message, true); } }));
    clientPanel.querySelectorAll("[data-br-sym]").forEach(x => x.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await burrowPost("/burrow-symbol", { id: burrowSel, fg: x.dataset.brSym }); await refreshBurrows(); }
      catch (err) { burrowStatus(err.message, true); } }));
    clientPanel.querySelectorAll("[data-br-paint]").forEach(x => x.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation(); burrowArmPaint(Number(burrowSel), x.dataset.brPaint);
    }));
    const inp = document.getElementById("brName");
    if (inp) { inp.addEventListener("click", ev => ev.stopPropagation()); inp.addEventListener("keydown", ev => ev.stopPropagation()); }
  }

  // ---- Map paint: arm, then drag on the map to add/remove a rectangle of tiles ----------------
  function burrowArmPaint(id, mode) {
    burrowPaintId = id;
    burrowPaintMode = mode === "remove" ? "remove" : "add";
    burrowPaintAnchor = null;
    clientPanel.className = "";     // clear the panel so the map is fully clickable
    let b = document.getElementById("brPaintBanner");
    if (!b) { b = document.createElement("div"); b.id = "brPaintBanner"; document.body.appendChild(b); }
    b.textContent = (burrowPaintMode === "remove" ? "Drag to ERASE burrow tiles" : "Drag to paint burrow tiles") + "  ·  Esc when done";
    b.className = "visible" + (burrowPaintMode === "remove" ? " kill" : "");
    document.addEventListener("keydown", burrowPaintKey, true);
  }
  function burrowDisarmPaint() {
    burrowPaintId = null; burrowPaintAnchor = null;
    const b = document.getElementById("brPaintBanner"); if (b) b.className = "";
    document.removeEventListener("keydown", burrowPaintKey, true);
  }
  function burrowPaintKey(e) {
    if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); burrowDisarmPaint(); openBurrowsPanel(); }
  }
  function burrowPaintDown(event) { burrowPaintAnchor = imagePixelFromEvent(event); }
  function burrowPaintUp(event) {
    const a = burrowPaintAnchor; burrowPaintAnchor = null;
    const end = imagePixelFromEvent(event);
    if (!a || !end) return;
    burrowPost("/burrow-paint", { id: burrowPaintId, px: a.x, py: a.y, w: a.w, h: a.h, px2: end.x, py2: end.y, mode: burrowPaintMode })
      .catch(() => {});
  }
