// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios
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

  async function openBuildingPanel(id) {
    let info = null;
    try {
      const r = await fetch(`/building-info?id=${id}&t=${Date.now()}`, { cache: "no-store" });
      if (r.ok) info = await r.json();
    } catch (_) {}
    if (!info || info.error || info.id < 0) { closeSelection(); return; }
    const underConstruction = !info.built;
    const statusLine = info.built ? "Constructed."
      : (info.suspended ? "Construction suspended." : "Waiting for constructionÃ¢â‚¬Â¦");
    const suspendBtn = (underConstruction && info.hasJobs)
      ? `<button class="bld-btn" data-bld-act="${info.suspended ? "resume" : "suspend"}">${info.suspended ? "Resume construction" : "Suspend construction"}</button>`
      : "";
    const passageBtn = info.passageControl
      ? `<button class="bld-btn${info.passageForbidden ? " active" : ""}" data-bld-act="toggle-passage">${info.passageForbidden ? "Allow passage" : "Close to passage"}</button>
         <div class="bld-note">Passage: ${info.passageForbidden ? "Closed to traffic" : "Allowed"}${info.passageClosed ? " (physically closed)" : " (currently open)"}</div>`
      : "";
    const cancelLabel = info.built ? "Remove building" : "Cancel construction";
    selection.className = "visible building-panel";
    selection.innerHTML = `
      <div class="bld-head"><div class="bld-name">${escapeHtml(info.name || "Building")}</div>
        <button class="bld-x" data-bld-close title="Close">Ã¢Å“â€¢</button></div>
      <div class="bld-status${info.suspended ? " suspended" : ""}">${escapeHtml(statusLine)}</div>
      ${suspendBtn}
      ${passageBtn}
      <button class="bld-btn danger" data-bld-act="cancel">${escapeHtml(cancelLabel)}</button>
    `;
    selection.querySelectorAll("[data-bld-act]").forEach(btn => btn.addEventListener("click", async event => {
      event.stopPropagation();
      const action = btn.dataset.bldAct;
      try { await fetch(`/building-action?id=${info.id}&action=${action}`, { method: "POST", cache: "no-store" }); } catch (_) {}
      if (action === "cancel") closeSelection();
      else openBuildingPanel(info.id); // refresh suspend/resume state
      focusPage();
    }));
    selection.querySelector("[data-bld-close]").addEventListener("click", event => {
      event.stopPropagation(); closeSelection(); focusPage();
    });
  }

  function workshopIconName(info) {
    const label = `${info?.subtype || ""} ${info?.name || ""} ${info?.kind || ""}`;
    return itemIconName({ label, category: "workshops" }) || (String(info?.kind || "").toLowerCase() === "furnace" ? "workshops_furnaces" : "workshops");
  }

  function workshopItemIconName(item) {
    const label = String(item?.name || item?.role || "");
    return itemIconName({ label, category: "workshops" }) || null;
  }

  async function workshopPost(path, params = {}) {
    const qs = new URLSearchParams();
    Object.entries(params).forEach(([k, v]) => qs.set(k, v == null ? "" : String(v)));
    qs.set("t", Date.now());
    const r = await fetch(`${path}?${qs.toString()}`, { method: "POST", cache: "no-store" });
    const text = await r.text();
    let data = {};
    try { data = text ? JSON.parse(text) : {}; } catch (_) {}
    if (!r.ok || data.ok === false)
      throw new Error(data.error || data.msg || text.trim() || "request failed");
    return data;
  }

  async function openWorkshopPanel(id, tab = activeWorkshopTab) {
    activeWorkshopTab = tab || "tasks";
    let info = null;
    let errMsg = "";
    try {
      const r = await fetch(`/workshop-info?id=${id}&t=${Date.now()}`, { cache: "no-store" });
      const text = await r.text();
      try { info = text ? JSON.parse(text) : null; } catch (_) {}
      if (!r.ok) errMsg = (info && (info.error || info.msg)) || text.trim() || "workshop info failed";
    } catch (err) {
      errMsg = err.message || "workshop info failed";
    }
    if (!info || info.ok === false || Number(info.id) < 0) {
      const msg = errMsg || (info && (info.error || info.msg)) || "Workshop data unavailable.";
      selection.className = "visible building-panel workshop-panel";
      selection.innerHTML = `
        <div class="bld-head"><div class="bld-name">Workshop</div><button class="bld-x" data-bld-close title="Close">X</button></div>
        <div class="workshop-body"><div class="workshop-status err">${escapeHtml(msg)}</div></div>
      `;
      selection.querySelector("[data-bld-close]")?.addEventListener("click", event => {
        event.stopPropagation(); closeSelection(); focusPage();
      });
      return;
    }
    renderWorkshopPanel(info);
  }

  function renderWorkshopPanel(info) {
    const jobs = Array.isArray(info.jobs) ? info.jobs : [];
    const tasks = Array.isArray(info.tasks) ? info.tasks : [];
    const orders = Array.isArray(info.orders) ? info.orders : [];
    const workers = Array.isArray(info.workers) ? info.workers : [];
    const items = Array.isArray(info.items) ? info.items : [];
    const tab = activeWorkshopTab || "tasks";
    const tabs = [["tasks", "Tasks"], ["workers", "Workers"], ["orders", "Work orders"]];
    const wsIcon = workshopIconName(info);
    const wsStyle = wsIcon ? bldIconStyle(wsIcon, 28) : "";
    const statusHtml = workshopStatusMsg
      ? `<div class="workshop-status${workshopStatusIsError ? " err" : ""}">${escapeHtml(workshopStatusMsg)}</div>`
      : "";

    const taskOptions = tasks.slice(0, 180).map(t =>
      `<button class="workshop-task-option" data-ws-add-task="${escapeHtml(t.key)}">
        <span>${escapeHtml(t.name || t.job || "Task")}</span>
        <span class="workshop-meta">${escapeHtml(t.reaction || t.job || "")}</span>
      </button>`).join("");

    const tasksBody = (() => {
      const rows = jobs.length ? jobs.map(job => {
        const meta = [];
        if (job.worker) meta.push(`Worker: ${escapeHtml(job.worker)}`);
        else if (job.working) meta.push("Being worked");
        else meta.push("Waiting");
        if (job.byManager) meta.push("Manager order");
        if (job.suspended) meta.push("Suspended");
        if (job.repeat) meta.push("Repeating");
        return `<div class="workshop-row">
          <div>
            <div class="workshop-name${job.suspended ? "" : " cyan"}">${escapeHtml(job.name || "Workshop task")}</div>
            <div class="workshop-meta">${meta.join(" &middot; ")}</div>
          </div>
          <div class="workshop-actions">
            <button class="workshop-icon-btn${job.suspended ? " active" : ""}" data-ws-job="${Number(job.id)}" data-ws-job-action="${job.suspended ? "resume" : "suspend"}" title="${job.suspended ? "Resume task" : "Suspend task"}">${job.suspended ? ">" : "||"}</button>
            <button class="workshop-icon-btn${job.repeat ? " active" : ""}" data-ws-job="${Number(job.id)}" data-ws-job-action="repeat" title="Toggle repeat">R</button>
            <button class="workshop-icon-btn${job.doNow ? " active" : ""}" data-ws-job="${Number(job.id)}" data-ws-job-action="now" title="Do now">!</button>
            <button class="workshop-icon-btn danger" data-ws-job="${Number(job.id)}" data-ws-job-action="cancel" title="Cancel task">X</button>
          </div>
        </div>`;
      }).join("") : `<div class="workshop-note">No queued tasks at this station.</div>`;
      const addBtn = info.canAddTasks
        ? `<button class="bld-btn" data-ws-toggle-add>${workshopAddMode ? "Hide task list" : "Queue shop task"}</button>`
        : `<div class="workshop-note">No orderable tasks reported for this station.</div>`;
      const picker = workshopAddMode && info.canAddTasks
        ? `<div class="workshop-section-title">Queue shop task</div><div class="workshop-task-grid">${taskOptions || `<div class="workshop-note">No orderable tasks reported for this station.</div>`}</div>`
        : "";
      return `<div class="workshop-section-title">Queued tasks (${jobs.length}/10)</div>
        <div class="workshop-list">${rows}</div>
        ${addBtn}
        ${picker}`;
    })();

    const workersBody = (() => {
      const profile = info.profile || {};
      const unrestricted = Number(profile.permittedCount || 0) === 0;
      const rows = workers.length ? workers.map(u => `
        <div class="workshop-worker-row">
          <div>
            <div class="workshop-name">${escapeHtml(u.name || `Unit ${u.id}`)}</div>
            <div class="workshop-meta">${escapeHtml(u.profession || "")}</div>
          </div>
          <button class="workshop-icon-btn${u.assigned ? " active" : ""}" data-ws-worker="${Number(u.id)}" data-ws-assign="${u.assigned ? "0" : "1"}">${u.assigned ? "On" : "Add"}</button>
        </div>`).join("") : `<div class="workshop-note">No citizens available.</div>`;
      return `<div class="workshop-note">${unrestricted ? "This workshop is free for anybody to use." : `${Number(profile.permittedCount) || 0} worker(s) assigned to this workshop.`}</div>
        ${unrestricted ? "" : `<button class="bld-btn" data-ws-workers-clear>Let anybody use this workshop</button>`}
        <div class="workshop-list compact">${rows}</div>`;
    })();

    const ordersBody = (() => {
      const orderRows = orders.length ? orders.map(o => {
        const total = Number(o.amountTotal) || 0;
        const left = Number(o.amountLeft) || 0;
        const amount = total > 0 ? `${left}/${total} left` : "repeating";
        return `<div class="workshop-order-row">
          <div>
            <div class="workshop-name">${escapeHtml(o.job || "Work order")}</div>
            <div class="workshop-meta">${escapeHtml(o.frequency === "OneTime" ? "One time" : (o.frequency || "One time"))} &middot; ${escapeHtml(amount)} &middot; ${o.active ? "Active" : "Inactive"}${o.validated ? "" : " &middot; Pending"}</div>
          </div>
          <button class="workshop-icon-btn danger" data-ws-order-cancel="${Number(o.id)}" title="Cancel order">X</button>
        </div>`;
      }).join("") : `<div class="workshop-note">No work orders are assigned to this workshop.</div>`;
      const orderTasks = tasks.filter(t => t.orderKey).slice(0, 180);
      const freqOptions = (typeof WO_FREQS !== "undefined" ? WO_FREQS : ["OneTime", "Daily", "Monthly", "Seasonally", "Yearly"])
        .map(f => `<option value="${escapeHtml(f)}">${escapeHtml(typeof woFreqLabel === "function" ? woFreqLabel(f) : f)}</option>`).join("");
      const picker = workshopOrderAddMode ? `
        <div class="workshop-section-title">New shop work order</div>
        <div class="zone-btn-row">
          <input class="wo-input" id="wsOrderAmount" type="number" min="1" max="9999" value="1" style="width:84px">
          <select class="wo-select" id="wsOrderFreq">${freqOptions}</select>
        </div>
        <div class="workshop-task-grid">
          ${orderTasks.length ? orderTasks.map(t => `<button class="workshop-task-option" data-ws-add-order="${escapeHtml(t.orderKey)}"><span>${escapeHtml(t.name || "Work order")}</span><span class="workshop-meta">${escapeHtml(t.reaction || t.job || "")}</span></button>`).join("") : `<div class="workshop-note">No orderable tasks reported for this station.</div>`}
        </div>` : "";
      return `<div class="workshop-note">Work orders created here are assigned to this exact workshop.</div>
        <div class="workshop-list">${orderRows}</div>
        <button class="bld-btn" data-ws-toggle-order>${workshopOrderAddMode ? "Hide order list" : "Add shop work order"}</button>
        <button class="bld-btn" data-ws-open-orders>Open full work orders</button>
        ${picker}`;
    })();

    const body = tab === "workers" ? workersBody : (tab === "orders" ? ordersBody : tasksBody);
    const itemRows = items.length ? items.map(item => {
      const ic = workshopItemIconName(item);
      const st = ic ? bldIconStyle(ic, 26) : "";
      return `<div class="workshop-item-row">
        <span class="workshop-item-ico"${st ? ` style="${st}"` : ""}></span>
        <div class="workshop-name">${escapeHtml(item.name || `Item ${item.id}`)}</div>
        <div class="workshop-meta">${escapeHtml(item.role || "")}</div>
      </div>`;
    }).join("") : `<div class="workshop-note">No visible contents.</div>`;

    selection.className = "visible building-panel workshop-panel";
    selection.innerHTML = `
      <div class="bld-head">
        <div class="bld-name workshop-title"><span class="workshop-ico"${wsStyle ? ` style="${wsStyle}"` : ""}></span><span>${escapeHtml(info.name || "Workshop")}</span></div>
        <button class="bld-x" data-bld-close title="Close">X</button>
      </div>
      <div class="workshop-tabs">${tabs.map(([key, label]) =>
        `<button class="workshop-tab${tab === key ? " active" : ""}" data-ws-tab="${key}">${escapeHtml(label)}</button>`).join("")}</div>
      <div class="workshop-body">
        ${statusHtml}
        ${body}
      </div>
      <div class="workshop-footer">
        <div class="workshop-section-title">Contents (${items.length})</div>
        <div class="workshop-list compact">${itemRows}</div>
      </div>
    `;

    selection.querySelector("[data-bld-close]")?.addEventListener("click", event => {
      event.stopPropagation(); closeSelection(); focusPage();
    });
    selection.querySelectorAll("[data-ws-tab]").forEach(btn => btn.addEventListener("click", event => {
      event.preventDefault(); event.stopPropagation();
      activeWorkshopTab = btn.dataset.wsTab || "tasks";
      workshopAddMode = false;
      workshopOrderAddMode = false;
      workshopStatusMsg = "";
      renderWorkshopPanel(info);
      focusPage();
    }));
    selection.querySelector("[data-ws-toggle-add]")?.addEventListener("click", event => {
      event.preventDefault(); event.stopPropagation();
      workshopAddMode = !workshopAddMode;
      workshopStatusMsg = "";
      renderWorkshopPanel(info);
      focusPage();
    });
    selection.querySelectorAll("[data-ws-add-task]").forEach(btn => btn.addEventListener("click", async event => {
      event.preventDefault(); event.stopPropagation();
      try {
        await workshopPost("/workshop-add-job", { id: info.id, task: btn.dataset.wsAddTask });
        workshopAddMode = false;
        workshopStatusMsg = "Shop task queued.";
        workshopStatusIsError = false;
      } catch (err) {
        workshopStatusMsg = err.message || "Could not queue shop task.";
        workshopStatusIsError = true;
      }
      await openWorkshopPanel(info.id, "tasks");
      focusPage();
    }));
    selection.querySelectorAll("[data-ws-job]").forEach(btn => btn.addEventListener("click", async event => {
      event.preventDefault(); event.stopPropagation();
      try {
        await workshopPost("/workshop-job-action", { id: info.id, job: btn.dataset.wsJob, action: btn.dataset.wsJobAction });
        workshopStatusMsg = "Task updated.";
        workshopStatusIsError = false;
      } catch (err) {
        workshopStatusMsg = err.message || "Could not update task.";
        workshopStatusIsError = true;
      }
      await openWorkshopPanel(info.id, "tasks");
      focusPage();
    }));
    selection.querySelectorAll("[data-ws-worker]").forEach(btn => btn.addEventListener("click", async event => {
      event.preventDefault(); event.stopPropagation();
      try {
        await workshopPost("/workshop-worker-action", { id: info.id, unit: btn.dataset.wsWorker, assign: btn.dataset.wsAssign });
        workshopStatusMsg = "Worker assignment updated.";
        workshopStatusIsError = false;
      } catch (err) {
        workshopStatusMsg = err.message || "Could not update workers.";
        workshopStatusIsError = true;
      }
      await openWorkshopPanel(info.id, "workers");
      focusPage();
    }));
    selection.querySelector("[data-ws-workers-clear]")?.addEventListener("click", async event => {
      event.preventDefault(); event.stopPropagation();
      try {
        await workshopPost("/workshop-workers-clear", { id: info.id });
        workshopStatusMsg = "Workshop is unrestricted.";
        workshopStatusIsError = false;
      } catch (err) {
        workshopStatusMsg = err.message || "Could not clear workers.";
        workshopStatusIsError = true;
      }
      await openWorkshopPanel(info.id, "workers");
      focusPage();
    });
    selection.querySelector("[data-ws-toggle-order]")?.addEventListener("click", event => {
      event.preventDefault(); event.stopPropagation();
      workshopOrderAddMode = !workshopOrderAddMode;
      workshopStatusMsg = "";
      renderWorkshopPanel(info);
      focusPage();
    });
    selection.querySelectorAll("[data-ws-add-order]").forEach(btn => btn.addEventListener("click", async event => {
      event.preventDefault(); event.stopPropagation();
      const amount = Math.max(1, Math.min(9999, Number(document.getElementById("wsOrderAmount")?.value) || 1));
      const frequency = document.getElementById("wsOrderFreq")?.value || "OneTime";
      try {
        await workshopPost("/order-create", { key: btn.dataset.wsAddOrder, amount, frequency, workshop: info.id });
        workshopOrderAddMode = false;
        workshopStatusMsg = "Shop work order queued.";
        workshopStatusIsError = false;
      } catch (err) {
        workshopStatusMsg = err.message || "Could not queue work order.";
        workshopStatusIsError = true;
      }
      await openWorkshopPanel(info.id, "orders");
      focusPage();
    }));
    selection.querySelectorAll("[data-ws-order-cancel]").forEach(btn => btn.addEventListener("click", async event => {
      event.preventDefault(); event.stopPropagation();
      try {
        await workshopPost("/order-cancel", { id: btn.dataset.wsOrderCancel });
        workshopStatusMsg = "Work order cancelled.";
        workshopStatusIsError = false;
      } catch (err) {
        workshopStatusMsg = err.message || "Could not cancel work order.";
        workshopStatusIsError = true;
      }
      await openWorkshopPanel(info.id, "orders");
      focusPage();
    }));
    selection.querySelector("[data-ws-open-orders]")?.addEventListener("click", event => {
      event.preventDefault(); event.stopPropagation();
      woCreateWorkshop = info.id;
      closeSelection();
      openWorkOrdersPanel();
      focusPage();
    });
  }

  // --- Activity zone panel: enable/disable (active shaded), remove, + per-type specials ---
  const ZONE_TYPE_LABEL = {
    Pond: "Pit / Pond", Pen: "Pen / Pasture", WaterSource: "Water Source",
    MeetingHall: "Meeting Area", FishingArea: "Fishing", SandCollection: "Sand Collection",
    ClayCollection: "Clay Collection", Dump: "Garbage Dump", PlantGathering: "Gather Fruit",
    AnimalTraining: "Animal Training", Dungeon: "Dungeon", Bedroom: "Bedroom",
    DiningHall: "Dining Hall", Office: "Office", Dormitory: "Dormitory",
    Barracks: "Barracks", ArcheryRange: "Archery Range", Tomb: "Tomb"
  };
  async function openZonePanel(id) {
    let info = null;
    try {
      const r = await fetch(`/zone-info?id=${id}&t=${Date.now()}`, { cache: "no-store" });
      if (r.ok) info = await r.json();
    } catch (_) {}
    if (!info || info.error || Number(info.id) < 0) { closeSelection(); return; }
    const typeLabel = ZONE_TYPE_LABEL[info.type] || info.type || "Zone";
    const owner = info.owner || {};
    const location = info.location || {};
    const gather = info.gather || {};
    const tomb = info.tomb || {};
    const archery = info.archery || {};
    const specialParts = [];
    if (info.canOwner) {
      specialParts.push(`
        <div class="zone-section-label">Assignment</div>
        <button class="bld-btn" data-zone-owner>${Number(owner.id) >= 0 ? `Assigned to ${escapeHtml(owner.name || `Unit ${owner.id}`)}` : "Assign citizen"}</button>`);
    }
    if (info.canLocation) {
      specialParts.push(`
        <div class="zone-section-label">Location</div>
        <button class="bld-btn" data-zone-locations>${Number(location.id) >= 0 ? `${escapeHtml(location.name || location.type || "Location")} (${escapeHtml(location.type || "Location")})` : "Add or choose location"}</button>`);
    }
    if (info.isPitPond) {
      specialParts.push(`
      <div class="zone-section-label">Pit / Pond</div>
      <div class="zone-btn-row">
        <button class="zone-tgl${info.fillingPond ? " zone-on" : ""}" data-zone-act="pond">Pond (fill water)</button>
        <button class="zone-tgl${info.fillingPond ? "" : " zone-on"}" data-zone-act="pit">Pit (drop)</button>
      </div>
      <button class="bld-btn" data-zone-units>${info.fillingPond ? "Assign animals to pond" : "Assign animals to drop"}</button>`);
    }
    if (info.isPen) {
      specialParts.push(`<button class="bld-btn" data-zone-units>Assign animals to pasture</button>`);
    }
    if (info.isGather) {
      specialParts.push(`
        <div class="zone-section-label">Gather Fruit</div>
        <div class="zone-btn-row">
          <button class="zone-tgl${gather.shrubs ? " zone-on" : ""}" data-zone-act="${gather.shrubs ? "gather-shrubs-off" : "gather-shrubs-on"}">Shrubs</button>
          <button class="zone-tgl${gather.trees ? " zone-on" : ""}" data-zone-act="${gather.trees ? "gather-trees-off" : "gather-trees-on"}">Trees</button>
          <button class="zone-tgl${gather.fallen ? " zone-on" : ""}" data-zone-act="${gather.fallen ? "gather-fallen-off" : "gather-fallen-on"}">Fallen</button>
        </div>`);
    }
    if (info.isTomb) {
      specialParts.push(`
        <div class="zone-section-label">Automatic Burial</div>
        <div class="zone-btn-row">
          <button class="zone-tgl${tomb.citizens ? " zone-on" : ""}" data-zone-act="${tomb.citizens ? "tomb-citizens-off" : "tomb-citizens-on"}">Citizens</button>
          <button class="zone-tgl${tomb.pets ? " zone-on" : ""}" data-zone-act="${tomb.pets ? "tomb-pets-off" : "tomb-pets-on"}">Pets</button>
        </div>`);
    }
    if (info.isArchery) {
      const dir = archery.direction || "west";
      specialParts.push(`
        <div class="zone-section-label">Shoot From</div>
        <div class="zone-btn-row">
          <button class="zone-tgl${dir === "west" ? " zone-on" : ""}" data-zone-act="archery-west">West</button>
          <button class="zone-tgl${dir === "east" ? " zone-on" : ""}" data-zone-act="archery-east">East</button>
        </div>
        <div class="zone-btn-row">
          <button class="zone-tgl${dir === "north" ? " zone-on" : ""}" data-zone-act="archery-north">North</button>
          <button class="zone-tgl${dir === "south" ? " zone-on" : ""}" data-zone-act="archery-south">South</button>
        </div>`);
    }
    const specials = specialParts.join("");
    selection.className = "visible building-panel zone-panel";
    selection.innerHTML = `
      <div class="bld-head"><div class="bld-name">${escapeHtml(info.name || typeLabel)}</div>
        <button class="bld-x" data-bld-close title="Close">&#10005;</button></div>
      <div class="bld-status">${escapeHtml(typeLabel)}${info.assignedUnits ? ` &middot; ${info.assignedUnits} assigned` : ""}</div>
      <div class="zone-section-label">Status</div>
      <div class="zone-btn-row">
        <button class="zone-tgl${info.active ? " zone-on" : ""}" data-zone-act="enable" title="Zone active">&#9654; Active</button>
        <button class="zone-tgl${info.active ? "" : " zone-on"}" data-zone-act="disable" title="Zone suspended">&#10074;&#10074; Suspended</button>
      </div>
      ${specials}
      <button class="bld-btn danger" data-zone-act="remove">Remove zone</button>
    `;
    selection.querySelectorAll("[data-zone-act]").forEach(btn => btn.addEventListener("click", async event => {
      event.stopPropagation();
      const action = btn.dataset.zoneAct;
      try { await fetch(`/zone-action?id=${info.id}&action=${encodeURIComponent(action)}`, { method: "POST", cache: "no-store" }); } catch (_) {}
      if (action === "remove") closeSelection();
      else openZonePanel(info.id);   // re-render with the new state (active/pit-pond shading)
      focusPage();
    }));
    selection.querySelector("[data-zone-units]")?.addEventListener("click", event => {
      event.stopPropagation(); openZoneUnitsPanel(info.id); focusPage();
    });
    selection.querySelector("[data-zone-owner]")?.addEventListener("click", event => {
      event.stopPropagation(); openZoneOwnersPanel(info.id); focusPage();
    });
    selection.querySelector("[data-zone-locations]")?.addEventListener("click", event => {
      event.stopPropagation(); openZoneLocationsPanel(info.id); focusPage();
    });
    selection.querySelector("[data-bld-close]").addEventListener("click", event => {
      event.stopPropagation(); closeSelection(); focusPage();
    });
  }

  async function openZoneUnitsPanel(id) {
    let data = null;
    try {
      const r = await fetch(`/zone-units?id=${id}&t=${Date.now()}`, { cache: "no-store" });
      if (r.ok) data = await r.json();
    } catch (_) {}
    if (!data || Number(data.id) < 0) { openZonePanel(id); return; }
    const typeLabel = ZONE_TYPE_LABEL[data.type] || data.type || "Zone";
    const rows = Array.isArray(data.units) ? data.units : [];
    selection.className = "visible building-panel zone-panel";
    selection.innerHTML = `
      <div class="bld-head"><div class="bld-name">${escapeHtml(data.name || typeLabel)}</div>
        <button class="bld-x" data-bld-close title="Close">&#10005;</button></div>
      <div class="bld-status">${escapeHtml(typeLabel)} &middot; animal assignment</div>
      <button class="bld-btn" data-zone-back>Back to zone</button>
      ${rows.length ? `<div class="zone-unit-list">
        ${rows.map(u => {
          const flags = Array.isArray(u.flags) ? u.flags.join(" | ") : "";
          const label = u.assigned ? "Unassign" : (u.assignedElsewhere ? "Move here" : "Assign");
          const kind = u.kind || "unit";
          return `<div class="zone-unit-row">
            <div>
              <div class="zone-unit-name">${escapeHtml(u.name || u.race || `Unit ${u.id}`)}</div>
              <div class="zone-unit-meta">${escapeHtml(flags || u.race || "")}</div>
            </div>
            <button class="zone-unit-act${u.assigned ? " assigned" : ""}" data-zone-unit="${Number(u.id)}" data-zone-kind="${escapeHtml(kind)}" data-zone-assign="${u.assigned ? "0" : "1"}">${label}</button>
          </div>`;
        }).join("")}
      </div>` : `<div class="zone-note">No assignable animals found.</div>`}
    `;
    selection.querySelector("[data-zone-back]").addEventListener("click", event => {
      event.stopPropagation(); openZonePanel(data.id); focusPage();
    });
    selection.querySelectorAll("[data-zone-unit]").forEach(btn => btn.addEventListener("click", async event => {
      event.stopPropagation();
      const unit = Number(btn.dataset.zoneUnit);
      const kind = btn.dataset.zoneKind || "unit";
      const assign = Number(btn.dataset.zoneAssign) ? 1 : 0;
      if (Number.isInteger(unit) && unit >= 0) {
        try {
          await fetch(`/zone-unit-action?id=${data.id}&unit=${unit}&assign=${assign}&kind=${encodeURIComponent(kind)}`, { method: "POST", cache: "no-store" });
        } catch (_) {}
      }
      openZoneUnitsPanel(data.id);
      loadZones();
      focusPage();
    }));
    selection.querySelector("[data-bld-close]").addEventListener("click", event => {
      event.stopPropagation(); closeSelection(); focusPage();
    });
  }

  async function openZoneOwnersPanel(id) {
    let data = null;
    try {
      const r = await fetch(`/zone-owners?id=${id}&t=${Date.now()}`, { cache: "no-store" });
      if (r.ok) data = await r.json();
    } catch (_) {}
    if (!data || Number(data.id) < 0) { openZonePanel(id); return; }
    const typeLabel = ZONE_TYPE_LABEL[data.type] || data.type || "Zone";
    const rows = Array.isArray(data.owners) ? data.owners : [];
    selection.className = "visible building-panel zone-panel zone-wide";
    selection.innerHTML = `
      <div class="bld-head"><div class="bld-name">${escapeHtml(data.name || typeLabel)}</div>
        <button class="bld-x" data-bld-close title="Close">&#10005;</button></div>
      <div class="bld-status">${escapeHtml(typeLabel)} &middot; citizen assignment</div>
      <button class="bld-btn" data-zone-back>Back to zone</button>
      ${Number(data.ownerId) >= 0 ? `<button class="bld-btn danger" data-zone-owner-clear>Remove assignment</button>` : ""}
      ${rows.length ? `<div class="zone-unit-list">
        ${rows.map(u => {
          const flags = [];
          if (u.profession) flags.push(u.profession);
          if (u.dead) flags.push("deceased");
          if (Number(u.sameTypeRooms) > 0) flags.push(`${u.sameTypeRooms} other ${typeLabel}`);
          const label = u.assigned ? "Assigned" : "Assign";
          return `<div class="zone-unit-row">
            <div>
              <div class="zone-unit-name">${escapeHtml(u.name || `Unit ${u.id}`)}</div>
              <div class="zone-unit-meta">${escapeHtml(flags.join(" | "))}</div>
            </div>
            <button class="zone-unit-act${u.assigned ? " assigned" : ""}" data-zone-owner-unit="${Number(u.id)}">${label}</button>
          </div>`;
        }).join("")}
      </div>` : `<div class="zone-note">No assignable citizens found.</div>`}
    `;
    selection.querySelector("[data-zone-back]").addEventListener("click", event => {
      event.stopPropagation(); openZonePanel(data.id); focusPage();
    });
    selection.querySelector("[data-zone-owner-clear]")?.addEventListener("click", async event => {
      event.stopPropagation();
      try { await fetch(`/zone-owner-action?id=${data.id}&unit=-1`, { method: "POST", cache: "no-store" }); } catch (_) {}
      openZoneOwnersPanel(data.id);
      focusPage();
    });
    selection.querySelectorAll("[data-zone-owner-unit]").forEach(btn => btn.addEventListener("click", async event => {
      event.stopPropagation();
      const unit = Number(btn.dataset.zoneOwnerUnit);
      if (Number.isInteger(unit) && unit >= 0) {
        const nextUnit = btn.classList.contains("assigned") ? -1 : unit;
        try {
          await fetch(`/zone-owner-action?id=${data.id}&unit=${nextUnit}`, { method: "POST", cache: "no-store" });
        } catch (_) {}
      }
      openZoneOwnersPanel(data.id);
      focusPage();
    }));
    selection.querySelector("[data-bld-close]").addEventListener("click", event => {
      event.stopPropagation(); closeSelection(); focusPage();
    });
  }

  async function openZoneLocationsPanel(id) {
    let data = null;
    try {
      const r = await fetch(`/zone-locations?id=${id}&t=${Date.now()}`, { cache: "no-store" });
      if (r.ok) data = await r.json();
    } catch (_) {}
    if (!data || Number(data.id) < 0) { openZonePanel(id); return; }
    const typeLabel = ZONE_TYPE_LABEL[data.type] || data.type || "Zone";
    const locations = Array.isArray(data.locations) ? data.locations : [];
    const createTypes = Array.isArray(data.createTypes) ? data.createTypes : [];
    selection.className = "visible building-panel zone-panel zone-wide";
    selection.innerHTML = `
      <div class="bld-head"><div class="bld-name">${escapeHtml(data.name || typeLabel)}</div>
        <button class="bld-x" data-bld-close title="Close">&#10005;</button></div>
      <div class="bld-status">${escapeHtml(typeLabel)} &middot; locations</div>
      <button class="bld-btn" data-zone-back>Back to zone</button>
      ${Number(data.locationId) >= 0 ? `<button class="bld-btn danger" data-zone-location-clear>Remove current location assignment</button>` : ""}
      ${createTypes.length ? `<div class="zone-section-label">Create New Location</div>
        <div class="zone-location-create-grid">
          ${createTypes.map(t => `<button class="zone-mini-btn" data-zone-location-create="${escapeHtml(t.kind)}">New ${escapeHtml(t.label)}</button>`).join("")}
        </div>` : ""}
      <div class="zone-section-label">Existing Locations</div>
      ${locations.length ? `<div class="zone-unit-list">
        ${locations.map(loc => {
          const flags = [];
          if (loc.label) flags.push(loc.label);
          flags.push(`${Number(loc.zoneCount) || 0} zone${Number(loc.zoneCount) === 1 ? "" : "s"}`);
          const label = loc.current ? "Current" : "Assign";
          return `<div class="zone-unit-row">
            <div>
              <div class="zone-unit-name">${escapeHtml(loc.name || loc.label || `Location ${loc.id}`)}</div>
              <div class="zone-unit-meta">${escapeHtml(flags.join(" | "))}</div>
            </div>
            <button class="zone-unit-act${loc.current ? " assigned" : ""}" data-zone-location="${Number(loc.id)}">${label}</button>
          </div>`;
        }).join("")}
      </div>` : `<div class="zone-note">No existing locations found.</div>`}
    `;
    selection.querySelector("[data-zone-back]").addEventListener("click", event => {
      event.stopPropagation(); openZonePanel(data.id); focusPage();
    });
    selection.querySelector("[data-zone-location-clear]")?.addEventListener("click", async event => {
      event.stopPropagation();
      try { await fetch(`/zone-location-action?id=${data.id}&action=clear`, { method: "POST", cache: "no-store" }); } catch (_) {}
      openZoneLocationsPanel(data.id);
      focusPage();
    });
    selection.querySelectorAll("[data-zone-location-create]").forEach(btn => btn.addEventListener("click", async event => {
      event.stopPropagation();
      const kind = btn.dataset.zoneLocationCreate || "";
      try {
        await fetch(`/zone-location-action?id=${data.id}&action=create&kind=${encodeURIComponent(kind)}`, { method: "POST", cache: "no-store" });
      } catch (_) {}
      openZoneLocationsPanel(data.id);
      focusPage();
    }));
    selection.querySelectorAll("[data-zone-location]").forEach(btn => btn.addEventListener("click", async event => {
      event.stopPropagation();
      const loc = Number(btn.dataset.zoneLocation);
      if (!btn.classList.contains("assigned") && Number.isInteger(loc) && loc >= 0) {
        try {
          await fetch(`/zone-location-action?id=${data.id}&action=assign&location=${loc}`, { method: "POST", cache: "no-store" });
        } catch (_) {}
      }
      openZoneLocationsPanel(data.id);
      focusPage();
    }));
    selection.querySelector("[data-bld-close]").addEventListener("click", event => {
      event.stopPropagation(); closeSelection(); focusPage();
    });
  }

  // --- Stockpile management panel ---
  const STOCK_CATS = [
    ["All", "all"], ["Food", "food"], ["Stone", "stone"], ["Wood", "wood"],
    ["Furniture", "furniture"], ["Finished goods", "finished"], ["Bars & blocks", "bars"],
    ["Gems", "gems"], ["Cloth", "cloth"], ["Leather", "leather"], ["Sheets", "sheets"],
    ["Ammo", "ammo"], ["Armor", "armor"], ["Weapons", "weapons"], ["Animals", "animals"],
    ["Refuse", "refuse"], ["Corpses", "corpses"], ["Coins", "coins"], ["None", "none"]
  ];
  function activePresetFromGroups(g) {
    g = g || {};
    const on = Object.keys(g).filter(k => g[k] === true);
    if (on.length === 0) return "none";
    if (on.length >= 17) return "all";
    if (on.length === 1) {
      return ({ food: "food", stone: "stone", wood: "wood", furniture: "furniture",
        finished_goods: "finished", bars_blocks: "bars", gems: "gems", cloth: "cloth",
        leather: "leather", sheet: "sheets", ammo: "ammo", armor: "armor",
        weapons: "weapons", animals: "animals", refuse: "refuse", corpses: "corpses",
        coins: "coins" })[on[0]] || "";
    }
    return "";
  }
  function stockGroupForPreset(key) {
    return ({ food: "food", stone: "stone", wood: "wood", furniture: "furniture",
      finished: "finished_goods", bars: "bars_blocks", gems: "gems", cloth: "cloth",
      leather: "leather", sheets: "sheet", ammo: "ammo", armor: "armor",
      weapons: "weapons", animals: "animals", refuse: "refuse", corpses: "corpses",
      coins: "coins" })[key] || "";
  }
  function stockCatIsActive(groups, key) {
    const preset = activePresetFromGroups(groups);
    if (key === "all") return preset === "all";
    if (key === "none") return preset === "none";
    const group = stockGroupForPreset(key);
    return !!(group && groups && groups[group]);
  }
  async function openStockpilePanel(id) {
    try {
      const r = await fetch(`/stockpile-info?id=${id}&t=${Date.now()}`, { cache: "no-store" });
    if (!r.ok) throw new Error("info failed");
      renderStockpilePanel(await r.json());
    } catch (_) {
      selection.className = "visible";
      selection.innerHTML = `<h1>Stockpile unavailable</h1>`;
    }
  }
  function linkListHtml(items) {
    items = Array.isArray(items) ? items : [];
    if (!items.length) return `<span class="sp-pill">None</span>`;
    return items.map(item => `<span class="sp-pill" title="${escapeHtml(item.name || "")}">${escapeHtml(item.name || `#${item.id}`)}</span>`).join("");
  }
  function flatStockpileLinks(info, key) {
    const links = info.links || {};
    if (key === "give")
      return [...(Array.isArray(links.give) ? links.give : []), ...(Array.isArray(links.giveWorkshops) ? links.giveWorkshops : [])];
    return [...(Array.isArray(links.take) ? links.take : []), ...(Array.isArray(links.takeWorkshops) ? links.takeWorkshops : [])];
  }
  async function postStockpile(url) {
    try {
      const r = await fetch(url, { method: "POST", cache: "no-store" });
      return r.ok ? r : null;
    } catch (_) {
      return null;
    }
  }
  function renderStockpilePanel(info) {
    const id = info.id;
    const groups = info.groups || {};
    const display = info.displayName || `Stockpile #${info.number || 0}`;
    const sz = info.size || { w: 1, h: 1 };
    const pos = info.pos || { x: 0, y: 0, z: 0 };
    const storage = info.storage || { barrels: 0, bins: 0, wheelbarrows: 0 };
    const giveLinks = flatStockpileLinks(info, "give");
    const takeLinks = flatStockpileLinks(info, "take");
    const giveIds = new Set(giveLinks.map(x => Number(x.id)));
    const takeIds = new Set(takeLinks.map(x => Number(x.id)));
    const targets = Array.isArray(info.targets) ? info.targets : [];
    selection.className = "visible stockpile-panel";
    selection.innerHTML = `
      <div class="sp-panel">
        <button class="unit-close-button" data-sp-close title="Close">X</button>
        <div class="sp-header">
          <input class="sp-name" type="text" value="${escapeHtml(info.name || "")}" placeholder="${escapeHtml(display)}" maxlength="64">
          <button class="sp-rename" data-sp-rename>Rename</button>
        </div>
        <div class="sp-sub">${escapeHtml(display)} - ${sz.w}x${sz.h} at ${pos.x},${pos.y},${pos.z}</div>
        <div class="sp-section-title">Stores</div>
        <div class="sp-cat-grid">
          ${STOCK_CATS.map(([label, key]) => `<button class="sp-cat${stockCatIsActive(groups, key) ? " active" : ""}" data-sp-cat="${key}">${escapeHtml(label)}</button>`).join("")}
        </div>
        <div class="sp-section-title">Customize contents</div>
        <button class="sp-small-button sp-open-editor" data-sp-open-editor>Edit which items are stored (custom)&hellip;</button>
        <div class="sp-section-title">Containers</div>
        <div class="sp-storage-grid">
          <label class="sp-num-label">Barrels<input class="sp-num" data-sp-storage="barrels" type="number" min="0" max="3000" value="${Number(storage.barrels) || 0}"></label>
          <label class="sp-num-label">Bins<input class="sp-num" data-sp-storage="bins" type="number" min="0" max="3000" value="${Number(storage.bins) || 0}"></label>
          <label class="sp-num-label">Wheelbarrows<input class="sp-num" data-sp-storage="wheelbarrows" type="number" min="0" max="3000" value="${Number(storage.wheelbarrows) || 0}"></label>
          <button class="sp-small-button" data-sp-storage-save>Save</button>
        </div>
        <div class="sp-section-title">Links</div>
        <div class="sp-mode-row">
          <button class="sp-mode-button${info.linksOnly ? " active" : ""}" data-sp-links-only="${info.linksOnly ? 0 : 1}">Links only</button>
          <button class="sp-mode-button" data-sp-refresh>Refresh</button>
        </div>
        <div class="sp-link-summary">
          <div class="sp-link-bucket"><strong>Gives to</strong><div class="sp-pill-row">${linkListHtml(giveLinks)}</div></div>
          <div class="sp-link-bucket"><strong>Takes from</strong><div class="sp-pill-row">${linkListHtml(takeLinks)}</div></div>
        </div>
        <div class="sp-targets">
          ${targets.length ? targets.map(target => {
            const tid = Number(target.id);
            const gives = giveIds.has(tid);
            const takes = takeIds.has(tid);
            const meta = `${target.kind || "building"} ${target.pos ? `${target.pos.x},${target.pos.y},${target.pos.z}` : ""}`;
            return `<div class="sp-target-row">
              <div>
                <div class="sp-target-name" title="${escapeHtml(target.name || "")}">${escapeHtml(target.name || `Building ${tid}`)}</div>
                <div class="sp-target-meta">${escapeHtml(meta)}</div>
              </div>
              <button class="sp-link-button${gives ? " active" : ""}" data-sp-link-mode="give" data-sp-link-target="${tid}" data-on="${gives ? 0 : 1}">Give</button>
              <button class="sp-link-button${takes ? " active" : ""}" data-sp-link-mode="take" data-sp-link-target="${tid}" data-on="${takes ? 0 : 1}">Take</button>
            </div>`;
          }).join("") : `<div class="sp-target-row"><div class="sp-target-name">No linkable buildings</div><span></span><span></span></div>`}
        </div>
        <div class="sp-actions">
          <button class="sp-repaint${stockRepaintId === id ? " active" : ""}" data-sp-repaint>Repaint</button>
          <button class="sp-remove" data-sp-remove>Remove stockpile</button>
        </div>
      </div>
    `;
    selection.querySelector("[data-sp-close]").addEventListener("click", event => {
      event.stopPropagation(); closeSelection(); focusPage();
    });
    const doRename = async () => {
      const nm = selection.querySelector(".sp-name").value;
      await postStockpile(`/stockpile-rename?id=${id}&name=${encodeURIComponent(nm)}`);
      openStockpilePanel(id);
    };
    selection.querySelector("[data-sp-rename]").addEventListener("click", event => { event.stopPropagation(); doRename(); });
    selection.querySelector(".sp-name").addEventListener("keydown", event => {
      if (event.key === "Enter") { event.preventDefault(); doRename(); }
    });
    selection.querySelectorAll("[data-sp-cat]").forEach(b => b.addEventListener("click", async event => {
      event.stopPropagation();
      const key = b.dataset.spCat || "all";
      const mode = (key === "all" || key === "none") ? "set" : (stockCatIsActive(groups, key) ? "disable" : "enable");
      await postStockpile(`/stockpile-set?id=${id}&preset=${encodeURIComponent(key)}&mode=${encodeURIComponent(mode)}`);
      openStockpilePanel(id);
    }));
    selection.querySelector("[data-sp-storage-save]").addEventListener("click", async event => {
      event.stopPropagation();
      const valueFor = key => Math.max(0, Math.min(3000, Number(selection.querySelector(`[data-sp-storage="${key}"]`)?.value || 0) || 0));
      await postStockpile(`/stockpile-storage?id=${id}&barrels=${valueFor("barrels")}&bins=${valueFor("bins")}&wheelbarrows=${valueFor("wheelbarrows")}`);
      openStockpilePanel(id);
    });
    selection.querySelector("[data-sp-links-only]").addEventListener("click", async event => {
      event.stopPropagation();
      await postStockpile(`/stockpile-links-only?id=${id}&on=${event.currentTarget.dataset.spLinksOnly}`);
      openStockpilePanel(id);
    });
    selection.querySelector("[data-sp-refresh]").addEventListener("click", event => {
      event.stopPropagation();
      openStockpilePanel(id);
    });
    selection.querySelectorAll("[data-sp-link-target]").forEach(button => button.addEventListener("click", async event => {
      event.stopPropagation();
      const target = Number(button.dataset.spLinkTarget);
      const mode = button.dataset.spLinkMode || "give";
      const on = Number(button.dataset.on || 0);
      await postStockpile(`/stockpile-link?id=${id}&target=${target}&mode=${encodeURIComponent(mode)}&on=${on}`);
      openStockpilePanel(id);
    }));
    selection.querySelector("[data-sp-repaint]").addEventListener("click", event => {
      event.stopPropagation();
      setStockRepaint(id);
      renderStockpilePanel(info);
      focusPage();
    });
    selection.querySelector("[data-sp-remove]").addEventListener("click", async event => {
      event.stopPropagation();
      await postStockpile(`/stockpile-remove?id=${id}`);
      closeSelection(); focusPage();
    });
    // Custom item editor opens in its own window (DF-style 3-column layout).
    const openEd = selection.querySelector("[data-sp-open-editor]");
    if (openEd) openEd.addEventListener("click", event => { event.stopPropagation(); openSpEditor(id); focusPage(); });
  }

  // ---- Custom stockpile editor: its own window, DF-style 3 columns (category | sub-group | items) ----
  const SP_EDIT_CATS = [
    ["Ammo", "ammo", 1],
    ["Animals", "animals", 2],
    ["Armor", "armor", 3],
    ["Bars/blocks", "bars", 4],
    ["Cloth", "cloth", 5],
    ["Coins", "coins", 6],
    ["Finished goods", "finished", 7],
    ["Food", "food", 8],
    ["Furniture/siege ammo", "furniture", 9],
    ["Gems", "gems", 10],
    ["Leather", "leather", 11],
    ["Corpses", "corpses", 12],
    ["Refuse", "refuse", 13],
    ["Sheets", "sheets", 14],
    ["Stone", "stone", 15],
    ["Weapons/trap comps", "weapons", 16],
    ["Wood", "wood", 17]
  ];  // [label, key, icon row in stockpile_icons.png]
  let spEditId = null, spEditCat = null, spEditGroup = null;
  let spGroupsCache = [], spItemsCache = [], spItemSearch = "";

  function spIconStyle(row, px) {
    px = px || 18;
    return `display:inline-block;width:${px}px;height:${px}px;vertical-align:middle;margin-right:6px;` +
           `background-image:url(/asset/stockpile_icons.png);background-size:${px}px ${20 * px}px;` +
           `background-position:0 -${row * px}px;image-rendering:pixelated`;
  }

  function closeSpEditor() { const m = document.getElementById("spEditorModal"); if (m) m.remove(); }

  function openSpEditor(id) {
    spEditId = id; spEditCat = SP_EDIT_CATS[0][1]; spEditGroup = null; spItemSearch = "";
    spGroupsCache = []; spItemsCache = [];
    let m = document.getElementById("spEditorModal");
    if (!m) { m = document.createElement("div"); m.id = "spEditorModal"; document.body.appendChild(m); }
    renderSpEditor();
    loadSpGroups(spEditCat);
  }

  async function loadSpGroups(cat) {
    spEditCat = cat; spGroupsCache = []; spEditGroup = null;
    try {
      const r = await fetch(`/stockpile-cat-groups?cat=${encodeURIComponent(cat)}&t=${Date.now()}`, { cache: "no-store" });
      const d = await r.json();
      spGroupsCache = (d.ok && Array.isArray(d.groups)) ? d.groups : [];
    } catch (_) { spGroupsCache = []; }
    spEditGroup = spGroupsCache.length ? spGroupsCache[0].key : null;
    renderSpEditor();
    if (spEditGroup) loadSpEditorItems();
  }

  async function loadSpEditorItems() {
    const el = document.getElementById("speItems");
    if (el) el.innerHTML = `<div class="sp-note">Loading...</div>`;
    try {
      const r = await fetch(`/stockpile-items?id=${spEditId}&cat=${encodeURIComponent(spEditCat)}&group=${encodeURIComponent(spEditGroup || "")}&t=${Date.now()}`, { cache: "no-store" });
      const d = await r.json();
      spItemsCache = (d.ok && Array.isArray(d.items)) ? d.items : [];
    } catch (_) { spItemsCache = []; }
    renderSpEditorItems();
  }

  function renderSpEditor() {
    const m = document.getElementById("spEditorModal");
    if (!m) return;
    const cats = SP_EDIT_CATS.map(([label, key, row]) =>
      `<button class="spe-cat${spEditCat === key ? " active" : ""}" data-spe-cat="${key}"><span class="sp-cat-icon" style="${spIconStyle(row, 18)}"></span>${escapeHtml(label)}</button>`).join("");
    const groups = spGroupsCache.length
      ? spGroupsCache.map(g => `<button class="spe-group${spEditGroup === g.key ? " active" : ""}" data-spe-group="${escapeHtml(g.key)}">${escapeHtml(g.label)}</button>`).join("")
      : `<div class="sp-note">(single list)</div>`;
    m.innerHTML = `<div class="spe-backdrop" data-spe-close></div>
      <div class="spe-window">
        <div class="spe-head"><div class="spe-title">Stockpile contents</div><button class="spe-close" data-spe-close>X</button></div>
        <div class="spe-cols">
          <div class="spe-col spe-cats">${cats}</div>
          <div class="spe-col spe-groups">${groups}</div>
          <div class="spe-col spe-itemcol">
            <div class="sp-items-head">
              <button class="sp-small-button" data-spe-all="1">All</button>
              <button class="sp-small-button" data-spe-all="0">None</button>
              <input class="sp-item-search" id="speSearch" type="text" placeholder="Find..." value="${escapeHtml(spItemSearch || "")}">
            </div>
            <div class="spe-items" id="speItems"></div>
          </div>
        </div>
      </div>`;
    m.querySelectorAll("[data-spe-close]").forEach(b => b.addEventListener("click", e => { e.stopPropagation(); closeSpEditor(); }));
    m.querySelectorAll("[data-spe-cat]").forEach(b => b.addEventListener("click", e => { e.stopPropagation(); spItemSearch = ""; loadSpGroups(b.dataset.speCat); }));
    m.querySelectorAll("[data-spe-group]").forEach(b => b.addEventListener("click", e => { e.stopPropagation(); spEditGroup = b.dataset.speGroup; spItemSearch = ""; renderSpEditor(); loadSpEditorItems(); }));
    m.querySelectorAll("[data-spe-all]").forEach(b => b.addEventListener("click", async e => {
      e.stopPropagation();
      await postStockpile(`/stockpile-toggle-all?id=${spEditId}&cat=${encodeURIComponent(spEditCat)}&group=${encodeURIComponent(spEditGroup || "")}&on=${b.dataset.speAll}`);
      await loadSpEditorItems();
    }));
    const s = document.getElementById("speSearch");
    if (s) s.addEventListener("input", () => {
      spItemSearch = s.value || "";
      renderSpEditorItems();
      const n = document.getElementById("speSearch");
      if (n) { n.focus(); try { n.setSelectionRange(n.value.length, n.value.length); } catch (_) {} }
    });
    renderSpEditorItems();
  }

  function renderSpEditorItems() {
    const el = document.getElementById("speItems");
    if (!el) return;
    const q = (spItemSearch || "").toLowerCase();
    const items = q ? spItemsCache.filter(it => (it.name || "").toLowerCase().includes(q)) : spItemsCache;
    const onCount = spItemsCache.filter(it => it.on).length;
    el.innerHTML = `<div class="spe-count">${onCount}/${spItemsCache.length} enabled</div>
      <div class="spe-itemlist">${items.length ? items.map(it =>
        `<button class="sp-item${it.on ? " on" : ""}" data-spe-item="${it.idx}" data-on="${it.on ? 0 : 1}">${escapeHtml(it.name)}</button>`).join("")
        : `<div class="sp-note">No matches.</div>`}</div>`;
    el.querySelectorAll("[data-spe-item]").forEach(b => b.addEventListener("click", async e => {
      e.stopPropagation();
      const idx = b.dataset.speItem, on = b.dataset.on;
      await postStockpile(`/stockpile-toggle-item?id=${spEditId}&cat=${encodeURIComponent(spEditCat)}&group=${encodeURIComponent(spEditGroup || "")}&idx=${idx}&on=${on}`);
      const it = spItemsCache.find(x => String(x.idx) === String(idx));
      if (it) it.on = (on === "1");
      renderSpEditorItems();
    }));
  }
