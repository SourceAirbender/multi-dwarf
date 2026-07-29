// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// Copyright (C) 2026 Reynaldo Reyes
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

  // ---- Squads (military) panel, backed by the /squad* endpoints -----------------------------
  // Read: GET /squads (list + free/creatable command positions), GET /squad?id= (one squad's
  // members, candidate dwarves, and orders). Write: /squad-create, /squad-rename, /squad-delete,
  // /squad-assign, /squad-remove, /squad-order. The backend does the DF work under CoreSuspender;
  // This panel uses the shared open -> refresh -> render lifecycle.
  // loop and reusing the shared info-window chrome.
  let sqSquads = [];             // squads from /squads
  let sqFreePositions = [];      // command seats a squad can be created under (assignmentId-based)
  let sqCreatablePositions = []; // position types that still need appointing (informational)
  let sqSelId = null;            // selected squad id
  let sqDetail = null;           // detail payload from /squad?id=
  let sqCreating = false;        // is the create form showing?
  let sqCreatePos = null;        // chosen assignmentId in the create form
  let sqOrderMode = null;        // armed map-targeting mode: null | "move" | "kill"
  let sqOrderSquadId = null;     // squad the armed order applies to
  let sqUniformMgr = false;      // is the uniform-template editor open?
  let sqCatalog = null;          // /uniforms catalog payload (templates + option lists)
  let sqEditTemplateId = null;   // template being edited in the manager

  // Classic DF 16-colour palette, indexed by profession colour (0-15); falls back to light grey.
  const SQ_DF_COLORS = ["#111111", "#0000aa", "#00aa00", "#00aaaa", "#aa0000", "#aa00aa",
                        "#aa5500", "#aaaaaa", "#555555", "#5555ff", "#55ff55", "#55ffff",
                        "#ff5555", "#ff55ff", "#ffff55", "#ffffff"];
  function sqColor(i) { i = Number(i); return SQ_DF_COLORS[(i >= 0 && i < 16) ? i : 7]; }

  // DF calendar months by index 0-11 (three per season).
  const SQ_MONTHS = ["Granite", "Slate", "Felsite", "Hematite", "Malachite", "Galena",
                     "Limestone", "Sandstone", "Timber", "Moonstone", "Opal", "Obsidian"];

  // Uniform item categories (cat 0-6) as sent to /uniform-item-add.
  const SQ_CATS = [[0, "Body"], [1, "Head"], [2, "Legs"], [3, "Gloves"], [4, "Shoes"], [5, "Shield"], [6, "Weapon"]];

  async function squadFetch(path) {
    const sep = path.includes("?") ? "&" : "?";
    const r = await fetch(`${path}${sep}player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
    const text = await r.text();
    let data = {};
    try { data = text ? JSON.parse(text) : {}; } catch (_) {}
    if (!r.ok || data.ok === false) throw new Error(data.error || data.msg || text.trim() || `${path} failed`);
    return data;
  }

  async function squadApi(path, params = {}) {
    const qs = new URLSearchParams();
    qs.set("player", player);
    Object.entries(params).forEach(([k, v]) => qs.set(k, v == null ? "" : String(v)));
    qs.set("t", Date.now());
    const r = await fetch(`${path}?${qs.toString()}`, { method: "POST", cache: "no-store" });
    const text = await r.text();
    let data = {};
    try { data = text ? JSON.parse(text) : {}; } catch (_) {}
    if (!r.ok || data.ok === false) throw new Error(data.error || data.msg || text.trim() || "request failed");
    return data;
  }

  function squadSetStatus(msg, isErr) {
    const el = document.getElementById("sqStatus");
    if (el) { el.textContent = msg || ""; el.className = "sq-status" + (isErr ? " err" : ""); }
  }

  async function openSquadsPanel() {
    setActiveToolbar("squads");
    clearBuildPlacement(false);
    activeInfoPanel = "squads";
    clientPanel.className = "visible info-panel";
    if (!clientPanel.querySelector(".sq-cols")) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading squads...</div></div></div>`;
    }
    try {
      await refreshSquads();
    } catch (_) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Squad data unavailable.</div></div></div>`;
    }
  }

  async function refreshSquads() {
    const data = await squadFetch("/squads");
    sqSquads = Array.isArray(data.squads) ? data.squads : [];
    sqFreePositions = Array.isArray(data.freePositions) ? data.freePositions : [];
    sqCreatablePositions = Array.isArray(data.creatablePositions) ? data.creatablePositions : [];
    if (sqSelId != null && !sqSquads.some(s => Number(s.id) === Number(sqSelId))) { sqSelId = null; sqDetail = null; }
    if (sqSelId != null) {
      try { sqDetail = await squadFetch(`/squad?id=${sqSelId}`); } catch (_) { sqDetail = null; }
    }
    renderSquads();
  }

  async function sqSelect(id) {
    sqSelId = Number(id);
    sqCreating = false;
    try { sqDetail = await squadFetch(`/squad?id=${sqSelId}`); } catch (_) { sqDetail = null; }
    renderSquads();
  }

  function renderSquads() {
    if (sqUniformMgr) { renderUniformMgr(); return; }
    // ---- left column: the squad list + a New Squad button ----
    const listRows = sqSquads.length ? sqSquads.map(s => {
      const em = s.emblem || {}, fg = em.fg || {}, bg = em.bg || {};
      const swatch = `background:rgb(${bg.r || 0},${bg.g || 0},${bg.b || 0});` +
                     `color:rgb(${fg.r != null ? fg.r : 200},${fg.g != null ? fg.g : 200},${fg.b != null ? fg.b : 200})`;
      return `<div class="sq-item${Number(s.id) === Number(sqSelId) ? " selected" : ""}" data-sq-sel="${s.id}">
          <span class="sq-emblem" style="${swatch}">&#9733;</span>
          <span class="sq-item-name">${escapeHtml(s.alias || s.name || "Squad")}</span>
          <span class="sq-item-count">${Number(s.memberCount) || 0}/${Number(s.positionCount) || 0}</span>
        </div>`;
    }).join("") : `<div class="sq-empty">No squads yet.</div>`;

    const left = `
      <div class="sq-list-head"><button class="sq-btn primary" data-sq-new>+ New Squad</button></div>
      <div class="sq-list">${listRows}</div>`;

    // ---- right column: create form, selected-squad detail, or a hint ----
    let right;
    if (sqCreating) right = sqRenderCreate();
    else if (sqDetail && sqDetail.squad) right = sqRenderDetail(sqDetail);
    else right = `<div class="sq-hint">Select a squad on the left, or create one to command your fort's military.</div>`;

    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs"><span class="info-tab active">Squads</span><span id="sqStatus" class="sq-status"></span></div>
        <div class="info-body" style="grid-template-columns:1fr;">
          <div class="sq-cols">
            <div class="sq-left">${left}</div>
            <div class="sq-right">${right}</div>
          </div>
        </div>
        <div class="info-footer"><div>Squads command your fort's military. Changes apply to the host fort immediately.</div></div>
      </div>`;

    // ---- wiring ----
    clientPanel.querySelectorAll("[data-sq-sel]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation(); sqSelect(b.dataset.sqSel);
    }));
    const newBtn = clientPanel.querySelector("[data-sq-new]");
    if (newBtn) newBtn.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation(); sqCreating = true; sqCreatePos = null; renderSquads();
    });
    sqWireCreate();
    sqWireDetail();
  }

  function sqRenderCreate() {
    if (!sqFreePositions.length) {
      const need = sqCreatablePositions.length
        ? `A squad must be led by a militia commander or captain. Appoint one on the
           <button class="sq-link" data-sq-goto="nobles">Nobles</button> screen, then come back.`
        : `No command position is available yet. Appoint a militia commander on the
           <button class="sq-link" data-sq-goto="nobles">Nobles</button> screen first.`;
      return `<div class="sq-detail-head"><b>New Squad</b></div>
        <div class="sq-hint">${need}</div>
        <div class="sq-form-row"><button class="sq-btn" data-sq-createcancel>Back</button></div>`;
    }
    const opts = sqFreePositions.map(p => {
      const who = p.holderName ? escapeHtml(p.holderName) : escapeHtml(p.appointLabel || "(vacant — appointed on create)");
      return `<label class="sq-radio">
          <input type="radio" name="sqCreatePos" value="${p.assignmentId}"${Number(sqCreatePos) === Number(p.assignmentId) ? " checked" : ""}>
          <span><b>${escapeHtml(p.title)}</b> — ${who}</span>
        </label>`;
    }).join("");
    return `<div class="sq-detail-head"><b>New Squad</b></div>
      <div class="sq-form">
        <div class="sq-section-title">Led by</div>
        ${opts}
        <div class="sq-form-row">
          <button class="sq-btn primary" data-sq-creatego>Create Squad</button>
          <button class="sq-btn" data-sq-createcancel>Cancel</button>
        </div>
      </div>`;
  }

  function sqWireCreate() {
    clientPanel.querySelectorAll('input[name="sqCreatePos"]').forEach(r =>
      r.addEventListener("change", () => { sqCreatePos = Number(r.value); }));
    const go = clientPanel.querySelector("[data-sq-creatego]");
    if (go) go.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const pos = sqCreatePos != null ? sqCreatePos : (sqFreePositions[0] && sqFreePositions[0].assignmentId);
      try {
        const d = await squadApi("/squad-create", { position: pos });
        sqCreating = false;
        await refreshSquads();
        if (d.id != null) await sqSelect(d.id);
        squadSetStatus("Squad created.", false);
      } catch (err) { squadSetStatus(err.message || "Could not create squad.", true); }
    });
    const cancel = clientPanel.querySelector("[data-sq-createcancel]");
    if (cancel) cancel.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation(); sqCreating = false; renderSquads();
    });
    const goto = clientPanel.querySelector("[data-sq-goto]");
    if (goto) goto.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation(); openPanel(goto.dataset.sqGoto);
    });
  }

  function sqRenderDetail(detail) {
    const s = detail.squad;
    const members = Array.isArray(s.members) ? s.members : [];
    const uniforms = Array.isArray(detail.uniforms) ? detail.uniforms : [];
    const equippedCount = members.filter(m => Number(m.uniformItems) > 0).length;
    const routines = Array.isArray(detail.routines) ? detail.routines : [];
    const schedule = Array.isArray(detail.schedule) ? detail.schedule : [];
    const currentRoutineIdx = Number(s.routineIdx);
    const ammo = Array.isArray(detail.ammo) ? detail.ammo : [];
    const ammoDefs = Array.isArray(detail.ammoDefs) ? detail.ammoDefs : [];
    const memberRows = members.length ? members.map(m => {
      if (m.filled) {
        return `<div class="sq-member">
            <span class="sq-pos">${escapeHtml(m.positionName || "")}</span>
            <span class="sq-mname" style="color:${sqColor(m.professionColor)}">${escapeHtml(m.name || "")}</span>
            <span class="sq-prof">${escapeHtml(m.profession || "")}</span>
            <button class="sq-btn tiny" data-sq-remove="${m.unitId}">Remove</button>
          </div>`;
      }
      return `<div class="sq-member empty">
          <span class="sq-pos">${escapeHtml(m.positionName || "")}</span>
          <span class="sq-mname muted">(empty)</span>
        </div>`;
    }).join("") : `<div class="sq-empty">No positions.</div>`;

    const cands = Array.isArray(detail.candidates) ? detail.candidates : [];
    const candRows = cands.length ? cands.map(c =>
      `<div class="sq-cand">
          <span class="sq-mname" style="color:${sqColor(c.professionColor)}">${escapeHtml(c.name || "")}</span>
          <span class="sq-prof">${escapeHtml(c.profession || "")}</span>
          <button class="sq-btn tiny" data-sq-assign="${c.unitId}">Assign</button>
        </div>`).join("") : `<div class="sq-empty">No available dwarves.</div>`;

    const orders = Array.isArray(s.orders) ? s.orders : [];
    const orderRows = orders.length
      ? orders.map(o => `<div class="sq-order">${escapeHtml(o.description || o.type || "")}</div>`).join("")
      : `<div class="sq-subtle">No active orders.</div>`;

    return `
      <div class="sq-detail-head">
        <input id="sqRenameInput" class="sq-rename" type="text" value="${escapeHtml(s.alias || s.name || "")}" maxlength="48" spellcheck="false">
        <button class="sq-btn tiny" data-sq-rename>Rename</button>
        <button class="sq-btn tiny danger" data-sq-delete="${s.id}">Disband</button>
      </div>
      <div class="sq-section-title">Members (${Number(s.memberCount) || 0}/${Number(s.positionCount) || 0})</div>
      <div class="sq-members">${memberRows}</div>
      <div class="sq-section-title">Add a dwarf</div>
      <div class="sq-cands">${candRows}</div>
      <div class="sq-section-title">Uniform</div>
      ${uniforms.length ? `
        <div class="sq-form-row">
          <select id="sqUniformSelect" class="sq-select">${uniforms.map(u => `<option value="${u.id}">${escapeHtml(u.name)}</option>`).join("")}</select>
          <button class="sq-btn primary" data-sq-uniform-apply="${s.id}">Equip squad</button>
          <button class="sq-btn" data-sq-uniform-clear="${s.id}">Clear</button>
          <button class="sq-btn" data-sq-uniform-mgr>Manage templates…</button>
        </div>
        <div class="sq-subtle">Equipped: ${equippedCount}/${members.length} positions. Applies the template to the whole squad; members equip over time.</div>
      ` : `<div class="sq-form-row"><button class="sq-btn primary" data-sq-uniform-mgr>Create a uniform template…</button></div>`}
      <div class="sq-section-title">Ammunition</div>
      ${ammoDefs.length ? `
        <div class="sq-ammo-list">${ammo.length ? ammo.map(a => `
          <div class="sq-ammo">
            <span class="sq-mname">${escapeHtml(a.ammoName || "ammo")}</span>
            <span class="sq-prof">${escapeHtml(a.materialName || "any material")} · ${Number(a.amount) || 0}${a.combat ? " · combat" : ""}${a.training ? " · train" : ""}</span>
            <button class="sq-btn tiny" data-sq-ammo-remove="${a.index}">Remove</button>
          </div>`).join("") : `<div class="sq-subtle">No ammunition assigned.</div>`}</div>
        <div class="sq-form-row sq-ammo-add">
          <select id="sqAmmoSubtype" class="sq-select">${ammoDefs.map(d => `<option value="${d.subtype}">${escapeHtml(d.name)}${d.ammoClass ? ` (${escapeHtml(d.ammoClass)})` : ""}</option>`).join("")}</select>
          <input id="sqAmmoAmount" class="sq-num" type="number" min="0" max="9999" value="100" spellcheck="false">
          <label class="sq-check"><input id="sqAmmoCombat" type="checkbox" checked> combat</label>
          <label class="sq-check"><input id="sqAmmoTrain" type="checkbox"> train</label>
          <button class="sq-btn primary" data-sq-ammo-add="${s.id}">Add</button>
        </div>` : `<div class="sq-subtle">No ammunition types available on this fort.</div>`}
      <div class="sq-section-title">Schedule</div>
      ${routines.length ? `
        <div class="sq-form-row">
          <select id="sqRoutineSelect" class="sq-select">${routines.map(r => `<option value="${r.idx}"${Number(r.idx) === currentRoutineIdx ? " selected" : ""}>${escapeHtml(r.name)}</option>`).join("")}</select>
          <button class="sq-btn primary" data-sq-routine-set="${s.id}">Set routine</button>
        </div>` : ""}
      <div class="sq-subtle">Current routine: ${escapeHtml(s.routineName || "—")}${schedule.length ? " · click a month to toggle training" : ""}</div>
      ${schedule.length ? `<div class="sq-months">${schedule.map(m => `
        <button class="sq-month${m.hasTrain ? " train" : ""}" data-sq-month="${m.month}" data-sq-hastrain="${m.hasTrain ? 1 : 0}" title="${escapeHtml(SQ_MONTHS[m.month] || m.name || "")}${m.orderLabel ? " — " + escapeHtml(m.orderLabel) : ""}">
          <span class="sq-month-name">${escapeHtml((SQ_MONTHS[m.month] || "?").slice(0, 3))}</span>
          <span class="sq-month-ind">${m.hasTrain ? "tr " + (Number(m.minCount) || 0) : "—"}</span>
        </button>`).join("")}</div>` : ""}
      <div class="sq-section-title">Orders</div>
      <div class="sq-form-row sq-order-actions">
        <button class="sq-btn" data-sq-move="${s.id}">Move to…</button>
        <button class="sq-btn danger" data-sq-attack="${s.id}">Attack…</button>
        <button class="sq-btn" data-sq-train="${s.id}">Train</button>
      </div>
      <div class="sq-orders">${orderRows}</div>
      ${orders.length ? `<div class="sq-form-row"><button class="sq-btn" data-sq-standdown="${s.id}">Stand down (clear orders)</button></div>` : ""}`;
  }

  function sqWireDetail() {
    const rn = clientPanel.querySelector("[data-sq-rename]");
    if (rn) rn.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const inp = document.getElementById("sqRenameInput");
      const name = inp ? inp.value.trim() : "";
      try { await squadApi("/squad-rename", { id: sqSelId, name }); await refreshSquads(); squadSetStatus("Renamed.", false); }
      catch (err) { squadSetStatus(err.message || "Could not rename.", true); }
    });
    clientPanel.querySelectorAll("[data-sq-delete]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      if (!confirm("Disband this squad? This cannot be undone.")) return;
      try {
        await squadApi("/squad-delete", { squad: b.dataset.sqDelete });
        sqSelId = null; sqDetail = null;
        await refreshSquads();
        squadSetStatus("Squad disbanded.", false);
      } catch (err) { squadSetStatus(err.message || "Could not disband.", true); }
    }));
    clientPanel.querySelectorAll("[data-sq-remove]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await squadApi("/squad-remove", { unit: b.dataset.sqRemove }); await refreshSquads(); squadSetStatus("Removed from squad.", false); }
      catch (err) { squadSetStatus(err.message || "Could not remove.", true); }
    }));
    clientPanel.querySelectorAll("[data-sq-assign]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await squadApi("/squad-assign", { squad: sqSelId, unit: b.dataset.sqAssign }); await refreshSquads(); squadSetStatus("Dwarf assigned.", false); }
      catch (err) { squadSetStatus(err.message || "Could not assign.", true); }
    }));
    clientPanel.querySelectorAll("[data-sq-standdown]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await squadApi("/squad-order", { squad: b.dataset.sqStanddown, action: "cancel", all: 1 }); await refreshSquads(); squadSetStatus("Orders cleared.", false); }
      catch (err) { squadSetStatus(err.message || "Could not clear orders.", true); }
    }));
    const mv = clientPanel.querySelector("[data-sq-move]");
    if (mv) mv.addEventListener("click", e => { e.preventDefault(); e.stopPropagation(); sqArmOrder("move", mv.dataset.sqMove); });
    const atk = clientPanel.querySelector("[data-sq-attack]");
    if (atk) atk.addEventListener("click", e => { e.preventDefault(); e.stopPropagation(); sqArmOrder("kill", atk.dataset.sqAttack); });
    const tr = clientPanel.querySelector("[data-sq-train]");
    if (tr) tr.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await squadApi("/squad-order", { squad: tr.dataset.sqTrain, action: "train" }); await refreshSquads(); squadSetStatus("Training order issued.", false); }
      catch (err) { squadSetStatus(err.message || "Could not start training.", true); }
    });
    const uniApply = clientPanel.querySelector("[data-sq-uniform-apply]");
    if (uniApply) uniApply.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const sel = document.getElementById("sqUniformSelect");
      await sqApplyUniformAll(uniApply.dataset.sqUniformApply, sel ? Number(sel.value) : -1, false);
    });
    const uniClear = clientPanel.querySelector("[data-sq-uniform-clear]");
    if (uniClear) uniClear.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      await sqApplyUniformAll(uniClear.dataset.sqUniformClear, -1, true);
    });
    const uniMgr = clientPanel.querySelector("[data-sq-uniform-mgr]");
    if (uniMgr) uniMgr.addEventListener("click", e => { e.preventDefault(); e.stopPropagation(); openUniformManager(); });
    const rset = clientPanel.querySelector("[data-sq-routine-set]");
    if (rset) rset.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const sel = document.getElementById("sqRoutineSelect");
      const idx = sel ? Number(sel.value) : -1;
      try { await squadApi("/squad-schedule", { squad: rset.dataset.sqRoutineSet, action: "set-routine", routine: idx }); await refreshSquads(); squadSetStatus("Routine set.", false); }
      catch (err) { squadSetStatus(err.message || "Could not set routine.", true); }
    });
    clientPanel.querySelectorAll("[data-sq-month]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const month = Number(b.dataset.sqMonth);
      const hasTrain = b.dataset.sqHastrain === "1";
      const posCount = (sqDetail && sqDetail.squad) ? (Number(sqDetail.squad.positionCount) || 1) : 1;
      try {
        await squadApi("/squad-schedule", hasTrain
          ? { squad: sqSelId, action: "set-month-order", month, order: "none" }
          : { squad: sqSelId, action: "set-month-order", month, order: "train", min: posCount });
        await refreshSquads();
        squadSetStatus(hasTrain ? "Training cleared for that month." : "Training scheduled for that month.", false);
      } catch (err) { squadSetStatus(err.message || "Could not update schedule.", true); }
    }));
    const aAdd = clientPanel.querySelector("[data-sq-ammo-add]");
    if (aAdd) aAdd.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const sub = Number((document.getElementById("sqAmmoSubtype") || {}).value);
      const amt = Math.max(0, Math.min(9999, Number((document.getElementById("sqAmmoAmount") || {}).value) || 0));
      const combat = (document.getElementById("sqAmmoCombat") || {}).checked ? 1 : 0;
      const train = (document.getElementById("sqAmmoTrain") || {}).checked ? 1 : 0;
      if (!(sub >= 0)) { squadSetStatus("Pick an ammunition type.", true); return; }
      try { await squadApi("/squad-ammo", { squad: aAdd.dataset.sqAmmoAdd, action: "add", subtype: sub, amount: amt, matclass: -1, mattype: -1, matindex: -1, combat, training: train }); await refreshSquads(); squadSetStatus("Ammunition added.", false); }
      catch (err) { squadSetStatus(err.message || "Could not add ammunition.", true); }
    });
    clientPanel.querySelectorAll("[data-sq-ammo-remove]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await squadApi("/squad-ammo", { squad: sqSelId, action: "remove", index: Number(b.dataset.sqAmmoRemove) }); await refreshSquads(); squadSetStatus("Ammunition removed.", false); }
      catch (err) { squadSetStatus(err.message || "Could not remove ammunition.", true); }
    }));
    const ammtIn = document.getElementById("sqAmmoAmount");
    if (ammtIn) { ammtIn.addEventListener("click", ev => ev.stopPropagation()); ammtIn.addEventListener("keydown", ev => ev.stopPropagation()); }
    // Keep the rename box from leaking keystrokes to the map hotkeys; Enter submits.
    const inp = document.getElementById("sqRenameInput");
    if (inp) {
      inp.addEventListener("click", e => e.stopPropagation());
      inp.addEventListener("keydown", e => {
        e.stopPropagation();
        if (e.key === "Enter") { e.preventDefault(); const rb = clientPanel.querySelector("[data-sq-rename]"); if (rb) rb.click(); }
      });
    }
  }

  // ---- Map-targeted orders (move / attack): arm a single-shot click on the map ---------------
  // Reuses the pixel tile-grid contract (imagePixelFromEvent -> px/py/w/h) that /designate uses.
  // controls-placement.js's map pointerdown calls sqConsumeMapClick() before its own drag/inspect
  // logic, so an armed order captures the next left-click on the map.
  function sqArmOrder(mode, squadId) {
    sqOrderMode = mode;
    sqOrderSquadId = Number(squadId);
    sqShowAimBanner(mode);
    clientPanel.className = "";   // hide the panel so the whole map is clickable
    document.addEventListener("keydown", sqAimKeyHandler, true);
  }

  function sqDisarmOrder() {
    sqOrderMode = null;
    sqOrderSquadId = null;
    sqHideAimBanner();
    document.removeEventListener("keydown", sqAimKeyHandler, true);
  }

  function sqAimKeyHandler(e) {
    if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); sqDisarmOrder(); openSquadsPanel(); }
  }

  function sqShowAimBanner(mode) {
    let b = document.getElementById("sqAimBanner");
    if (!b) { b = document.createElement("div"); b.id = "sqAimBanner"; document.body.appendChild(b); }
    b.textContent = mode === "kill"
      ? "Click a creature to attack  ·  Esc to cancel"
      : "Click a destination to move the squad  ·  Esc to cancel";
    b.className = "visible" + (mode === "kill" ? " kill" : "");
  }

  function sqHideAimBanner() {
    const b = document.getElementById("sqAimBanner");
    if (b) b.className = "";
  }

  // Called synchronously from the map pointerdown handler. Returns true if it consumed the click.
  function sqConsumeMapClick(event) {
    if (!sqOrderMode) return false;
    const mode = sqOrderMode, squadId = sqOrderSquadId;
    const pixel = imagePixelFromEvent(event);
    sqDisarmOrder();
    sqIssueMapOrder(mode, squadId, pixel);
    return true;
  }

  async function sqIssueMapOrder(mode, squadId, pixel) {
    let msg = "", isErr = false;
    try {
      if (!pixel) throw new Error("Clicked off the map — order cancelled.");
      if (mode === "move") {
        await squadApi("/squad-order", { squad: squadId, action: "move", px: pixel.x, py: pixel.y, w: pixel.w, h: pixel.h });
        msg = "Move order issued.";
      } else if (mode === "kill") {
        const r = await fetch(`/inspect?player=${encodeURIComponent(player)}&px=${pixel.x}&py=${pixel.y}&w=${pixel.w}&h=${pixel.h}`, { cache: "no-store" });
        const data = await r.json().catch(() => ({}));
        const uid = (data && data.kind === "unit" && data.unit)
          ? Number(data.unit.id != null ? data.unit.id : (data.unit.unitId != null ? data.unit.unitId : -1))
          : -1;
        if (!(uid >= 0)) throw new Error("That tile has no creature — click a unit to attack.");
        await squadApi("/squad-order", { squad: squadId, action: "kill", target: uid });
        msg = "Attack order issued.";
      }
    } catch (e) {
      msg = e.message || "Order failed."; isErr = true;
    }
    if (squadId != null) sqSelId = Number(squadId);
    await openSquadsPanel();     // reopen the panel + refresh so the new order shows
    squadSetStatus(msg, isErr);
  }

  // ---- Uniforms: apply/clear a fort uniform template across every squad position -------------
  // /squad-uniform is per-position, so equipping the whole squad loops over its positions.
  async function sqApplyUniformAll(squadId, uniformId, clear) {
    const detail = sqDetail && sqDetail.squad;
    const positions = detail && Array.isArray(detail.members) ? detail.members.map(m => Number(m.idx)) : [];
    if (!positions.length) { squadSetStatus("Squad has no positions to equip.", true); return; }
    if (!clear && !(uniformId >= 0)) { squadSetStatus("Pick a uniform template first.", true); return; }
    let done = 0, failed = 0;
    for (const pos of positions) {
      try {
        await squadApi("/squad-uniform", clear
          ? { squad: squadId, pos, action: "clear" }
          : { squad: squadId, pos, action: "apply", uniform: uniformId });
        done++;
      } catch (_) { failed++; }
    }
    await refreshSquads();
    squadSetStatus(
      clear ? `Cleared uniforms on ${done} position(s).`
            : `Equipped ${done} position(s)${failed ? `, ${failed} failed` : ""}.`,
      failed > 0);
  }

  // ---- Uniform-template authoring (/uniform-*): a full-width editor over the /uniforms catalog ---
  async function openUniformManager() {
    try {
      sqCatalog = await squadFetch("/uniforms");
    } catch (_) {
      squadSetStatus("Could not load uniform templates.", true);
      return;
    }
    const list = sqCatalog && Array.isArray(sqCatalog.uniforms) ? sqCatalog.uniforms : [];
    if (sqEditTemplateId == null || !list.some(u => Number(u.id) === Number(sqEditTemplateId)))
      sqEditTemplateId = list.length ? Number(list[0].id) : null;
    sqUniformMgr = true;
    renderUniformMgr();
  }

  function closeUniformManager() {
    sqUniformMgr = false;
    sqCatalog = null;
    openSquadsPanel();
  }

  async function refreshCatalog() {
    try { sqCatalog = await squadFetch("/uniforms"); } catch (_) {}
    renderUniformMgr();
  }

  function renderUniformMgr() {
    const cat = sqCatalog || {};
    const templates = Array.isArray(cat.uniforms) ? cat.uniforms : [];
    const subtypes = cat.subtypes || {};
    const tpl = templates.find(u => Number(u.id) === Number(sqEditTemplateId)) || null;

    const tplList = templates.length ? templates.map(u =>
      `<div class="sq-item${Number(u.id) === Number(sqEditTemplateId) ? " selected" : ""}" data-uni-sel="${u.id}">
          <span class="sq-item-name">${escapeHtml(u.name || "uniform")}</span>
          <span class="sq-item-count">${(u.items || []).length}</span>
        </div>`).join("") : `<div class="sq-empty">No templates.</div>`;

    let editor;
    if (!tpl) {
      editor = `<div class="sq-hint">Create a uniform template to start authoring.</div>`;
    } else {
      const items = Array.isArray(tpl.items) ? tpl.items : [];
      const catBlocks = SQ_CATS.map(([c, label]) => {
        const inCat = items.filter(it => Number(it.cat) === c);
        const opts = [`<option value="-1">(any ${label.toLowerCase()})</option>`]
          .concat((subtypes[c] || []).map(st => `<option value="${st.subtype}">${escapeHtml(st.name)}</option>`)).join("");
        return `<div class="sq-uni-cat">
            <div class="sq-uni-cat-head">${label}</div>
            ${inCat.length ? inCat.map((it, i) => `<div class="sq-uni-item">
              <span class="sq-mname">${escapeHtml(it.itemTypeName || label)}${Number(it.subtype) < 0 ? " (any)" : ""}</span>
              <span class="sq-prof">${escapeHtml(it.materialName || "any material")}</span>
              <button class="sq-btn tiny danger" data-uni-item-remove="${c}" data-uni-index="${i}" title="Remove">✕</button>
            </div>`).join("") : `<div class="sq-subtle">none</div>`}
            <div class="sq-uni-add">
              <select class="sq-select sq-uni-subtype" data-uni-cat="${c}">${opts}</select>
              <button class="sq-btn tiny" data-uni-item-add="${c}">+ Add</button>
            </div>
          </div>`;
      }).join("");
      editor = `
        <div class="sq-detail-head">
          <input id="sqUniName" class="sq-rename" type="text" value="${escapeHtml(tpl.name || "")}" maxlength="48" spellcheck="false">
          <button class="sq-btn tiny" data-uni-rename="${tpl.id}">Rename</button>
          <button class="sq-btn tiny danger" data-uni-delete="${tpl.id}">Delete</button>
        </div>
        <div class="sq-form-row">
          <label class="sq-check"><input type="checkbox" id="sqUniReplace"${tpl.replaceClothing ? " checked" : ""}> Replace clothing</label>
          <label class="sq-check"><input type="checkbox" id="sqUniExact"${tpl.exactMatches ? " checked" : ""}> Exact matches</label>
          <button class="sq-btn tiny" data-uni-flags="${tpl.id}">Apply flags</button>
        </div>
        <div class="sq-uni-cats">${catBlocks}</div>`;
    }

    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs">
          <span class="info-tab active">Uniform Templates</span>
          <button class="sq-btn tiny" data-uni-back>← Back to squads</button>
          <span id="sqStatus" class="sq-status"></span>
        </div>
        <div class="info-body" style="grid-template-columns:1fr;">
          <div class="sq-cols">
            <div class="sq-left">
              <div class="sq-list-head"><button class="sq-btn primary" data-uni-create>+ New template</button></div>
              <div class="sq-list">${tplList}</div>
            </div>
            <div class="sq-right">${editor}</div>
          </div>
        </div>
        <div class="info-footer"><div>Templates are assigned to squads from each squad's Uniform section.</div></div>
      </div>`;

    clientPanel.querySelectorAll("[data-uni-sel]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation(); sqEditTemplateId = Number(b.dataset.uniSel); renderUniformMgr();
    }));
    const back = clientPanel.querySelector("[data-uni-back]");
    if (back) back.addEventListener("click", e => { e.preventDefault(); e.stopPropagation(); closeUniformManager(); });
    const create = clientPanel.querySelector("[data-uni-create]");
    if (create) create.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { const d = await squadApi("/uniform-create", { name: "New uniform" }); if (d.id != null) sqEditTemplateId = Number(d.id); await refreshCatalog(); squadSetStatus("Template created.", false); }
      catch (err) { squadSetStatus(err.message || "Could not create template.", true); }
    });
    const uren = clientPanel.querySelector("[data-uni-rename]");
    if (uren) uren.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const nm = ((document.getElementById("sqUniName") || {}).value || "").trim();
      try { await squadApi("/uniform-rename", { id: uren.dataset.uniRename, name: nm }); await refreshCatalog(); squadSetStatus("Renamed.", false); }
      catch (err) { squadSetStatus(err.message || "Could not rename.", true); }
    });
    const udel = clientPanel.querySelector("[data-uni-delete]");
    if (udel) udel.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      if (!confirm("Delete this uniform template?")) return;
      try { await squadApi("/uniform-delete", { id: udel.dataset.uniDelete }); sqEditTemplateId = null; await refreshCatalog(); squadSetStatus("Template deleted.", false); }
      catch (err) { squadSetStatus(err.message || "Could not delete.", true); }
    });
    const uflags = clientPanel.querySelector("[data-uni-flags]");
    if (uflags) uflags.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const rc = (document.getElementById("sqUniReplace") || {}).checked ? 1 : 0;
      const ex = (document.getElementById("sqUniExact") || {}).checked ? 1 : 0;
      try { await squadApi("/uniform-flags", { id: uflags.dataset.uniFlags, replaceClothing: rc, exactMatches: ex }); await refreshCatalog(); squadSetStatus("Flags updated.", false); }
      catch (err) { squadSetStatus(err.message || "Could not update flags.", true); }
    });
    clientPanel.querySelectorAll("[data-uni-item-add]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const c = Number(b.dataset.uniItemAdd);
      const sel = clientPanel.querySelector(`.sq-uni-subtype[data-uni-cat="${c}"]`);
      const st = sel ? Number(sel.value) : -1;
      const choice = (c === 6 && st < 0) ? 1 : 0;   // any-weapon -> individual choice "any"
      try { await squadApi("/uniform-item-add", { id: sqEditTemplateId, cat: c, subtype: st, matclass: -1, mattype: -1, matindex: -1, color: -1, choice }); await refreshCatalog(); squadSetStatus("Item added.", false); }
      catch (err) { squadSetStatus(err.message || "Could not add item.", true); }
    }));
    clientPanel.querySelectorAll("[data-uni-item-remove]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await squadApi("/uniform-item-remove", { id: sqEditTemplateId, cat: Number(b.dataset.uniItemRemove), index: Number(b.dataset.uniIndex) }); await refreshCatalog(); squadSetStatus("Item removed.", false); }
      catch (err) { squadSetStatus(err.message || "Could not remove item.", true); }
    }));
    const nameIn = document.getElementById("sqUniName");
    if (nameIn) {
      nameIn.addEventListener("click", ev => ev.stopPropagation());
      nameIn.addEventListener("keydown", ev => { ev.stopPropagation(); if (ev.key === "Enter") { ev.preventDefault(); const rb = clientPanel.querySelector("[data-uni-rename]"); if (rb) rb.click(); } });
    }
  }
