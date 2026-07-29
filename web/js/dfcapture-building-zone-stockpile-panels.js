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

  function decorateObjectAttribution(kind, id) {
    const row = selection.querySelector(
      ".bld-head,.sp-header,.farm-header,.cage-header,.coffin-header,.lever-header"
    ) || selection;
    row.dataset.attribKind = kind;
    row.dataset.attribId = String(id);
    if (!row.querySelector(":scope > [data-attrib-slot]")) {
      const slot = document.createElement("span");
      slot.dataset.attribSlot = "";
      row.prepend(slot);
    }
    window.dfAttribution?.decorate(selection);
  }

  async function buildingPanelPost(path, params) {
    const query = new URLSearchParams();
    Object.entries(params || {}).forEach(([key, value]) => query.set(key, String(value)));
    query.set("t", Date.now());
    const response = await fetch(`${path}?${query}`, { method: "POST", cache: "no-store" });
    const text = await response.text();
    let data = {};
    try { data = text ? JSON.parse(text) : {}; } catch (_) {}
    if (!response.ok || data.ok === false)
      throw new Error(data.error || text.trim() || "request failed");
    return data;
  }

  async function fetchFarmPlotInfo(id) {
    try {
      const response = await fetch(`/farm-plot?id=${id}&t=${Date.now()}`, { cache: "no-store" });
      if (!response.ok) return null;
      const data = await response.json();
      return data && data.isFarmPlot ? data : null;
    } catch (_) {
      return null;
    }
  }

  function renderFarmPlotPanel(info, farm) {
    const seasons = Array.isArray(farm.seasons) ? farm.seasons : [];
    farmSelectedSeason = Math.max(0, Math.min(3, Number(farmSelectedSeason) || 0));
    const active = seasons.find(season => Number(season.season) === farmSelectedSeason) ||
      seasons[0] || { season: 0, name: "Spring", plantId: -1, crops: [] };
    const crops = Array.isArray(active.crops) ? active.crops : [];
    const cropRows = [
      { id: -1, name: "Leave fallow", seedCount: null },
      ...crops
    ].map(crop => {
      const selected = Number(crop.id) === Number(active.plantId);
      const stock = crop.seedCount == null ? "" :
        `<span class="farm-crop-stock${Number(crop.seedCount) ? "" : " empty"}">${Number(crop.seedCount) || 0} seed${Number(crop.seedCount) === 1 ? "" : "s"}</span>`;
      return `<button class="farm-crop-row${selected ? " active" : ""}" data-farm-crop="${Number(crop.id)}">
        <span>${escapeHtml(crop.name || "Crop")}</span>${stock}
      </button>`;
    }).join("");
    const renameHeader = farmRenameMode
      ? `<div class="farm-rename-row">
          <input class="farm-rename-input" maxlength="128" value="${escapeHtml(info.name || "")}">
          <button class="bld-btn" data-farm-rename-save>Save</button>
          <button class="bld-btn" data-farm-rename-cancel>Cancel</button>
        </div>`
      : `<div class="bld-name">${escapeHtml(info.name || "Farm Plot")}</div>
         <button class="workshop-icon-btn" data-farm-rename title="Rename farm plot">Rename</button>`;
    selection.className = "visible building-panel farm-panel";
    selection.innerHTML = `
      <div class="bld-head">${renameHeader}<button class="bld-x" data-bld-close title="Close">X</button></div>
      <div class="farm-location">${farm.underground ? "Underground" : "Surface"} farm plot
        <span>${escapeHtml(farm.biome || "")}</span></div>
      <div class="farm-season-tabs">${seasons.map(season =>
        `<button class="farm-season-tab${Number(season.season) === Number(active.season) ? " active" : ""}" data-farm-season="${Number(season.season)}">
          ${escapeHtml(season.name || "Season")}${Number(season.season) === Number(farm.currentSeason) ? " (now)" : ""}
        </button>`).join("")}</div>
      <div class="farm-section-title">${escapeHtml(active.name || "Season")} crop</div>
      <div class="farm-crop-list">${cropRows}</div>
      <label class="farm-fertilize-row">
        <input type="checkbox" data-farm-fertilize${farm.fertilize?.seasonal ? " checked" : ""}>
        <span>Fertilize every season</span>
        <small>${Number(farm.fertilize?.current) || 0}/${Number(farm.fertilize?.max) || 0}</small>
      </label>
      <button class="bld-btn danger" data-bld-act="cancel">Remove farm plot</button>
    `;
    selection.querySelectorAll("[data-farm-season]").forEach(button =>
      button.addEventListener("click", event => {
        event.stopPropagation();
        farmSelectedSeason = Number(button.dataset.farmSeason) || 0;
        renderFarmPlotPanel(info, farm);
      }));
    selection.querySelectorAll("[data-farm-crop]").forEach(button =>
      button.addEventListener("click", async event => {
        event.stopPropagation();
        try {
          await buildingPanelPost("/farm-plot-action", {
            id: info.id, season: active.season, plant: button.dataset.farmCrop
          });
          const refreshed = await fetchFarmPlotInfo(info.id);
          if (refreshed) renderFarmPlotPanel(info, refreshed);
        } catch (error) {
          console.warn("farm crop update failed", error);
        }
        focusPage();
      }));
    selection.querySelector("[data-farm-fertilize]")?.addEventListener("change", async event => {
      event.stopPropagation();
      try {
        await buildingPanelPost("/farm-plot-fertilize-action", {
          id: info.id, seasonal: event.currentTarget.checked ? 1 : 0
        });
        const refreshed = await fetchFarmPlotInfo(info.id);
        if (refreshed) renderFarmPlotPanel(info, refreshed);
      } catch (error) {
        console.warn("farm fertilizer update failed", error);
      }
      focusPage();
    });
    selection.querySelector("[data-farm-rename]")?.addEventListener("click", event => {
      event.stopPropagation();
      farmRenameMode = true;
      renderFarmPlotPanel(info, farm);
      selection.querySelector(".farm-rename-input")?.focus();
    });
    selection.querySelector("[data-farm-rename-cancel]")?.addEventListener("click", event => {
      event.stopPropagation();
      farmRenameMode = false;
      renderFarmPlotPanel(info, farm);
    });
    const saveFarmName = async () => {
      const name = selection.querySelector(".farm-rename-input")?.value.trim() || "";
      try {
        await buildingPanelPost("/farm-plot-rename", { id: info.id, name });
        info.name = name || "Farm Plot";
        farmRenameMode = false;
        renderFarmPlotPanel(info, farm);
      } catch (error) {
        console.warn("farm rename failed", error);
      }
      focusPage();
    };
    selection.querySelector("[data-farm-rename-save]")?.addEventListener("click", event => {
      event.stopPropagation();
      saveFarmName();
    });
    selection.querySelector(".farm-rename-input")?.addEventListener("keydown", event => {
      if (event.key === "Enter") { event.preventDefault(); saveFarmName(); }
      if (event.key === "Escape") {
        event.preventDefault();
        farmRenameMode = false;
        renderFarmPlotPanel(info, farm);
      }
    });
    selection.querySelector("[data-bld-act]")?.addEventListener("click", async event => {
      event.stopPropagation();
      try {
        await fetch(`/building-action?id=${info.id}&action=cancel`, {
          method: "POST", cache: "no-store"
        });
      } catch (_) {}
      closeSelection();
      focusPage();
    });
    selection.querySelector("[data-bld-close]")?.addEventListener("click", event => {
      event.stopPropagation();
      closeSelection();
      focusPage();
    });
  }

  async function fetchOptionalBuildingJson(path, id) {
    try {
      const response = await fetch(`${path}?id=${id}&t=${Date.now()}`, { cache: "no-store" });
      if (!response.ok) return null;
      return await response.json();
    } catch (_) {
      return null;
    }
  }

  function wireSpecialBuildingClose() {
    selection.querySelector("[data-bld-close]")?.addEventListener("click", event => {
      event.stopPropagation();
      closeSelection();
      focusPage();
    });
  }

  async function openBuildingCagePanel(info) {
    const cage = await fetchOptionalBuildingJson("/building-cage", info.id);
    if (!cage || cage.ok === false) return false;
    const rows = Array.isArray(cage.units) ? cage.units : [];
    selection.className = "visible building-panel building-control-panel cage-panel";
    selection.innerHTML = `
      <div class="bld-head"><div class="bld-name">${escapeHtml(cage.name || info.name || "Cage")}</div>
        <button class="bld-x" data-bld-close title="Close">X</button></div>
      <div class="bld-note">Assign creatures, pets, or captured vermin to this cage.</div>
      <div class="building-control-list">${rows.length ? rows.map(row => `
        <div class="building-control-row${row.assigned ? " active" : ""}">
          <div><strong>${escapeHtml(row.name || `Unit ${row.id}`)}</strong>
            <small>${escapeHtml(row.race || row.kind || "")}${row.assignedElsewhere ? " - assigned elsewhere" : ""}</small>
            ${(Array.isArray(row.flags) ? row.flags : []).map(flag => `<span>${escapeHtml(flag)}</span>`).join("")}
          </div>
          <button class="bld-btn" data-cage-target="${Number(row.id)}" data-cage-kind="${escapeHtml(row.kind || "unit")}" data-cage-action="${row.assigned ? "release" : "assign"}">
            ${row.assigned ? "Release" : "Assign"}
          </button>
        </div>`).join("") : `<div class="bld-note">No assignable occupants were found.</div>`}</div>`;
    selection.querySelectorAll("[data-cage-target]").forEach(button =>
      button.addEventListener("click", async event => {
        event.stopPropagation();
        try {
          await buildingPanelPost("/building-cage-action", {
            id: info.id,
            target: button.dataset.cageTarget,
            kind: button.dataset.cageKind,
            action: button.dataset.cageAction
          });
          await openBuildingCagePanel(info);
        } catch (error) {
          console.warn("cage assignment failed", error);
        }
        focusPage();
      }));
    wireSpecialBuildingClose();
    return true;
  }

  function renderCoffinPanel(info, coffin) {
    const hasTomb = Number(coffin.tombId) >= 0;
    const owner = coffin.owner || {};
    const tomb = coffin.tomb || {};
    selection.className = "visible building-panel building-control-panel coffin-panel";
    selection.innerHTML = `
      <div class="bld-head"><div class="bld-name">${escapeHtml(coffin.name || info.name || "Coffin")}</div>
        <button class="bld-x" data-bld-close title="Close">X</button></div>
      <div class="bld-status">${hasTomb ? `Linked tomb: ${escapeHtml(coffin.tombName || `Zone ${coffin.tombId}`)}` : "No tomb zone is linked."}</div>
      <div class="bld-note">Owner: ${Number(owner.id) >= 0 ? escapeHtml(owner.name || `Unit ${owner.id}`) : "Any eligible citizen"}</div>
      ${hasTomb ? "" : `<button class="bld-btn" data-coffin-action="ensure-tomb">Create and link tomb zone</button>`}
      <button class="bld-btn" data-coffin-action="any-citizen">Use for any citizen</button>
      <label class="building-control-toggle"><input type="checkbox" data-coffin-toggle="citizens"${tomb.citizens ? " checked" : ""}> Permit citizens</label>
      <label class="building-control-toggle"><input type="checkbox" data-coffin-toggle="pets"${tomb.pets ? " checked" : ""}> Permit pets</label>`;
    selection.querySelectorAll("[data-coffin-action]").forEach(button =>
      button.addEventListener("click", async event => {
        event.stopPropagation();
        try {
          await buildingPanelPost("/burial-coffin-action", {
            id: info.id, action: button.dataset.coffinAction
          });
          const updated = await fetchOptionalBuildingJson("/burial-coffin", info.id);
          if (updated?.ok) renderCoffinPanel(info, updated);
        } catch (error) {
          console.warn("coffin action failed", error);
        }
        focusPage();
      }));
    selection.querySelectorAll("[data-coffin-toggle]").forEach(input =>
      input.addEventListener("change", async event => {
        event.stopPropagation();
        const key = input.dataset.coffinToggle;
        try {
          await buildingPanelPost("/burial-coffin-action", {
            id: info.id, action: `${key}-${input.checked ? "on" : "off"}`
          });
          const updated = await fetchOptionalBuildingJson("/burial-coffin", info.id);
          if (updated?.ok) renderCoffinPanel(info, updated);
        } catch (error) {
          console.warn("coffin permission update failed", error);
        }
        focusPage();
      }));
    wireSpecialBuildingClose();
  }

  function renderLeverLinkPanel(info, lever) {
    const targets = Array.isArray(lever.targets) ? lever.targets : [];
    const currentLinks = Array.isArray(lever.currentLinks) ? lever.currentLinks : [];
    const currentRows = currentLinks.length ? currentLinks.map(link => `
      <div class="lever-current-row ${link.status === "pending" ? "pending" : "linked"}">
        <div><strong>${escapeHtml(link.name || link.type || `Building ${link.id}`)}</strong>
          <small>${escapeHtml(link.type || "Building")} at ${Number(link.x)}, ${Number(link.y)}, ${Number(link.z)}</small></div>
        <span>${link.status === "pending" ? "Link queued" : "Linked"}</span>
      </div>`).join("") : `<div class="bld-note">This lever is not connected to any buildings.</div>`;
    selection.className = "visible building-panel building-control-panel lever-panel";
    selection.innerHTML = `
      <div class="bld-head"><div class="bld-name">${escapeHtml(lever.name || info.name || "Lever")}</div>
        <button class="bld-x" data-bld-close title="Close">X</button></div>
      <div class="bld-status">${Number(lever.mechanismCount) || 0} available mechanism(s)</div>
      ${lever.needsMechanisms ? `<div class="bld-note warning">Two available mechanisms are required to link a target.</div>` : ""}
      <div class="lever-section-title">Current connections (${currentLinks.length})</div>
      <div class="lever-current-list">${currentRows}</div>
      <div class="lever-section-title">Link another building (${targets.length})</div>
      <input class="lever-target-search" type="search"
        placeholder="Search floodgate, bridge, door..." data-lever-search>
      <div class="building-control-list lever-target-list">${targets.length ? targets.map(target => {
        const state = target.linked ? "linked" : target.pending ? "pending" : "available";
        const searchAliases = target.type === "Floodgate" ? " watergate water gate sluice" : "";
        return `
        <div class="building-control-row ${state}" data-lever-target-row
          data-lever-search-text="${escapeHtml(`${target.name || ""} ${target.type || ""}${searchAliases}`.toLowerCase())}">
          <div><strong>${escapeHtml(target.name || target.type || `Building ${target.id}`)}</strong>
            <small>${escapeHtml(target.type || "")} at ${Number(target.x)}, ${Number(target.y)}, ${Number(target.z)}</small></div>
          <button class="bld-btn" data-lever-target="${Number(target.id)}"
            ${lever.needsMechanisms || target.linked || target.pending ? "disabled" : ""}>
            ${target.linked ? "Linked" : target.pending ? "Queued" : "Link"}
          </button>
        </div>`;
      }).join("") : `<div class="bld-note">No linkable targets were found.</div>`}</div>`;
    selection.querySelector("[data-lever-search]")?.addEventListener("input", event => {
      const needle = event.currentTarget.value.trim().toLowerCase();
      selection.querySelectorAll("[data-lever-target-row]").forEach(row => {
        row.hidden = !!needle && !String(row.dataset.leverSearchText || "").includes(needle);
      });
    });
    selection.querySelectorAll("[data-lever-target]").forEach(button =>
      button.addEventListener("click", async event => {
        event.stopPropagation();
        try {
          await buildingPanelPost("/lever-link", {
            id: info.id, target: button.dataset.leverTarget
          });
          await openBuildingPanel(info.id);
        } catch (error) {
          console.warn("lever link failed", error);
        }
        focusPage();
      }));
    wireSpecialBuildingClose();
  }

  async function openBuildingPanel(id) {
    let info = null;
    try {
      const r = await fetch(`/building-info?id=${id}&t=${Date.now()}`, { cache: "no-store" });
      if (r.ok) info = await r.json();
    } catch (_) {}
    if (!info || info.error || info.id < 0) { closeSelection(); return; }
    if (info.isTradeDepot && info.built) {
      closeSelection();
      await openTradeDepotPanel(info.id);
      return;
    }
    if (info.isFarmPlot && info.built) {
      const farm = await fetchFarmPlotInfo(info.id);
      if (farm) {
        renderFarmPlotPanel(info, farm);
        decorateObjectAttribution("building", info.id);
        return;
      }
    }
    if (info.isCage && info.built && await openBuildingCagePanel(info)) {
      decorateObjectAttribution("building", info.id);
      return;
    }
    if (info.built) {
      const [coffin, lever] = await Promise.all([
        fetchOptionalBuildingJson("/burial-coffin", info.id),
        fetchOptionalBuildingJson("/lever-link", info.id)
      ]);
      if (coffin?.ok && coffin.isCoffin) {
        renderCoffinPanel(info, coffin);
        decorateObjectAttribution("building", info.id);
        return;
      }
      if (lever?.ok && lever.isLever) {
        renderLeverLinkPanel(info, lever);
        decorateObjectAttribution("building", info.id);
        return;
      }
    }
    const underConstruction = !info.built;
    const statusLine = info.built ? "Constructed."
      : (info.suspended ? "Construction suspended." : "Waiting for construction...");
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
        <button class="bld-x" data-bld-close title="Close">X</button></div>
      <div class="bld-status${info.suspended ? " suspended" : ""}">${escapeHtml(statusLine)}</div>
      ${suspendBtn}
      ${passageBtn}
      <button class="bld-btn danger" data-bld-act="cancel">${escapeHtml(cancelLabel)}</button>
    `;
    decorateObjectAttribution("building", info.id);
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
      const skillNames = ["Dabbling", "Novice", "Adequate", "Competent", "Skilled",
        "Proficient", "Talented", "Adept", "Expert", "Professional", "Accomplished",
        "Great", "Master", "High Master", "Grand Master", "Legendary", "Legendary+1",
        "Legendary+2", "Legendary+3", "Legendary+4", "Legendary+5"];
      const skillOptions = (current, includeUnlimited) => {
        const value = Number(current);
        let html = skillNames.map((name, level) =>
          `<option value="${level}"${level === value ? " selected" : ""}>${escapeHtml(name)}</option>`).join("");
        if (includeUnlimited)
          html += `<option value="3000"${value >= 3000 ? " selected" : ""}>No maximum</option>`;
        return html;
      };
      const blocked = new Set((Array.isArray(profile.blockedLabors) ?
        profile.blockedLabors : []).map(labor => Number(labor.id)));
      const laborRows = (Array.isArray(profile.allLabors) ? profile.allLabors : [])
        .map(labor => `<label class="workshop-labor-toggle">
          <input type="checkbox" data-ws-labor="${Number(labor.id)}"${blocked.has(Number(labor.id)) ? " checked" : ""}>
          <span>${escapeHtml(String(labor.name || "").replaceAll("_", " "))}</span>
        </label>`).join("");
      const profileControls = `
        <div class="workshop-section-title">Workshop profile</div>
        <div class="workshop-profile-grid">
          <label>Minimum skill<select class="wo-select" data-ws-min-level>${skillOptions(profile.minLevel, false)}</select></label>
          <label>Maximum skill<select class="wo-select" data-ws-max-level>${skillOptions(profile.maxLevel, true)}</select></label>
          <label>General orders<select class="wo-select" data-ws-max-orders>
            ${Array.from({ length: 11 }, (_, value) => `<option value="${value}"${value === Number(profile.maxGeneralOrders) ? " selected" : ""}>${value}</option>`).join("")}
          </select></label>
          <label class="workshop-profile-check"><input type="checkbox" data-ws-ban-orders${profile.generalOrdersBanned ? " checked" : ""}> Ban general work orders</label>
        </div>
        <details class="workshop-blocked-labors">
          <summary>Blocked labors (${blocked.size})</summary>
          <div class="workshop-labor-grid">${laborRows || `<div class="workshop-note">No labor list available.</div>`}</div>
        </details>`;
      const rows = workers.length ? workers.map(u => `
        <div class="workshop-worker-row">
          <div>
            <div class="workshop-name">${escapeHtml(u.name || `Unit ${u.id}`)}</div>
            <div class="workshop-meta">${escapeHtml(u.profession || "")}</div>
          </div>
          <button class="workshop-icon-btn${u.assigned ? " active" : ""}" data-ws-worker="${Number(u.id)}" data-ws-assign="${u.assigned ? "0" : "1"}">${u.assigned ? "On" : "Add"}</button>
        </div>`).join("") : `<div class="workshop-note">No citizens available.</div>`;
      return `${profileControls}
        <div class="workshop-note">${unrestricted ? "This workshop is free for anybody to use." : `${Number(profile.permittedCount) || 0} worker(s) assigned to this workshop.`}</div>
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
    const titleMarkup = workshopRenameMode
      ? `<div class="ws-rename-row">
          <input class="ws-rename-input" maxlength="128" value="${escapeHtml(info.name || "")}">
          <button class="workshop-icon-btn" data-ws-rename-save>Save</button>
          <button class="workshop-icon-btn" data-ws-rename-cancel>Cancel</button>
        </div>`
      : `<div class="bld-name workshop-title"><span class="workshop-ico"${wsStyle ? ` style="${wsStyle}"` : ""}></span><span>${escapeHtml(info.name || "Workshop")}</span></div>
         <button class="workshop-icon-btn" data-ws-rename title="Rename workshop">Rename</button>`;
    selection.innerHTML = `
      <div class="bld-head">
        ${titleMarkup}
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
    const postProfile = async (field, value, message) => {
      try {
        await workshopPost("/workshop-profile", { id: info.id, field, value });
        workshopStatusMsg = message;
        workshopStatusIsError = false;
      } catch (error) {
        workshopStatusMsg = error.message || "Could not update workshop profile.";
        workshopStatusIsError = true;
      }
      await openWorkshopPanel(info.id, "workers");
      focusPage();
    };
    selection.querySelector("[data-ws-min-level]")?.addEventListener("change", event =>
      postProfile("minLevel", Number(event.currentTarget.value), "Minimum skill updated."));
    selection.querySelector("[data-ws-max-level]")?.addEventListener("change", event =>
      postProfile("maxLevel", Number(event.currentTarget.value), "Maximum skill updated."));
    selection.querySelector("[data-ws-max-orders]")?.addEventListener("change", event =>
      postProfile("maxGeneralOrders", Number(event.currentTarget.value), "Order limit updated."));
    selection.querySelector("[data-ws-ban-orders]")?.addEventListener("change", event =>
      postProfile("banGeneralOrders", event.currentTarget.checked ? 1 : 0,
        "General-order policy updated."));
    selection.querySelectorAll("[data-ws-labor]").forEach(control =>
      control.addEventListener("change", event => postProfile(
        event.currentTarget.checked ? "blockLabor" : "unblockLabor",
        Number(event.currentTarget.dataset.wsLabor), "Blocked labors updated.")));
    selection.querySelector("[data-ws-rename]")?.addEventListener("click", event => {
      event.stopPropagation();
      workshopRenameMode = true;
      renderWorkshopPanel(info);
      selection.querySelector(".ws-rename-input")?.focus();
    });
    selection.querySelector("[data-ws-rename-cancel]")?.addEventListener("click", event => {
      event.stopPropagation();
      workshopRenameMode = false;
      renderWorkshopPanel(info);
      focusPage();
    });
    const saveWorkshopName = async () => {
      const name = selection.querySelector(".ws-rename-input")?.value.trim() || "";
      try {
        await workshopPost("/workshop-rename", { id: info.id, name });
        workshopRenameMode = false;
        workshopStatusMsg = name ? "Workshop renamed." : "Name cleared.";
        workshopStatusIsError = false;
      } catch (error) {
        workshopStatusMsg = error.message || "Could not rename workshop.";
        workshopStatusIsError = true;
      }
      await openWorkshopPanel(info.id, activeWorkshopTab);
      focusPage();
    };
    selection.querySelector("[data-ws-rename-save]")?.addEventListener("click", event => {
      event.stopPropagation();
      saveWorkshopName();
    });
    selection.querySelector(".ws-rename-input")?.addEventListener("keydown", event => {
      if (event.key === "Enter") {
        event.preventDefault();
        saveWorkshopName();
      } else if (event.key === "Escape") {
        event.preventDefault();
        workshopRenameMode = false;
        renderWorkshopPanel(info);
        focusPage();
      }
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
    if (info.canSquads) {
      specialParts.push(`<div class="zone-section-label">Squads</div>
        <button class="bld-btn" data-zone-squads>${Number(info.assignedSquads) || 0} squad assignment(s)</button>`);
    }
    const zoneHeader = zoneRenameMode
      ? `<div class="zone-rename-row">
          <input class="zone-rename-input" maxlength="128" value="${escapeHtml(info.name || "")}">
          <button class="zone-mini-btn" data-zone-rename-save>Save</button>
          <button class="zone-mini-btn" data-zone-rename-cancel>Cancel</button>
        </div>`
      : `<div class="bld-name">${escapeHtml(info.name || typeLabel)}</div>
         <button class="zone-mini-btn" data-zone-rename title="Rename zone">Rename</button>`;
    selection.className = "visible building-panel zone-panel";
    selection.innerHTML = `
      <div class="bld-head">${zoneHeader}
        <button class="bld-x" data-bld-close title="Close">&#10005;</button></div>
      <div class="bld-status">${escapeHtml(typeLabel)}${info.assignedUnits ? ` &middot; ${info.assignedUnits} assigned` : ""}</div>
      <div class="zone-section-label">Status</div>
      <div class="zone-btn-row">
        <button class="zone-tgl${info.active ? " zone-on" : ""}" data-zone-act="enable" title="Zone active">&#9654; Active</button>
        <button class="zone-tgl${info.active ? "" : " zone-on"}" data-zone-act="disable" title="Zone suspended">&#10074;&#10074; Suspended</button>
      </div>
      ${specialParts.join("")}
      <div class="zone-section-label">Repaint footprint</div>
      <div class="zone-btn-row">
        <button class="zone-tgl" data-zone-repaint="add">Add tiles</button>
        <button class="zone-tgl" data-zone-repaint="erase">Erase tiles</button>
      </div>
      <button class="bld-btn danger" data-zone-act="remove">Remove zone</button>
    `;
    decorateObjectAttribution("zone", info.id);
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
    selection.querySelector("[data-zone-squads]")?.addEventListener("click", event => {
      event.stopPropagation(); openZoneSquadsPanel(info.id); focusPage();
    });
    selection.querySelectorAll("[data-zone-repaint]").forEach(button => {
      button.addEventListener("click", event => {
        event.stopPropagation();
        const mode = button.dataset.zoneRepaint === "erase" ? "erase" : "add";
        closeSelection();
        setZoneRepaint(info.id, mode);
        focusPage();
      });
    });
    selection.querySelector("[data-zone-rename]")?.addEventListener("click", event => {
      event.stopPropagation();
      zoneRenameMode = true;
      openZonePanel(info.id);
    });
    selection.querySelector("[data-zone-rename-cancel]")?.addEventListener("click", event => {
      event.stopPropagation();
      zoneRenameMode = false;
      openZonePanel(info.id);
    });
    const saveZoneName = async () => {
      const name = selection.querySelector(".zone-rename-input")?.value.trim() || "";
      try {
        await buildingPanelPost("/zone-rename", { id: info.id, name });
        zoneRenameMode = false;
      } catch (_) {}
      openZonePanel(info.id);
    };
    selection.querySelector("[data-zone-rename-save]")?.addEventListener("click", event => {
      event.stopPropagation();
      saveZoneName();
    });
    selection.querySelector(".zone-rename-input")?.addEventListener("keydown", event => {
      if (event.key === "Enter") {
        event.preventDefault();
        saveZoneName();
      } else if (event.key === "Escape") {
        event.preventDefault();
        zoneRenameMode = false;
        openZonePanel(info.id);
      }
    });
    selection.querySelector("[data-bld-close]").addEventListener("click", event => {
      event.stopPropagation(); closeSelection(); focusPage();
    });
  }

  async function openZoneSquadsPanel(id) {
    let data = null;
    try {
      const response = await fetch(`/zone-squads?id=${id}&t=${Date.now()}`, { cache: "no-store" });
      if (response.ok) data = await response.json();
    } catch (_) {}
    if (!data || Number(data.id) < 0) {
      openZonePanel(id);
      return;
    }
    const squads = Array.isArray(data.squads) ? data.squads : [];
    const modeButton = (squad, mode, field, label) =>
      `<button class="zone-squad-mode${squad[field] ? " active" : ""}"
        data-zone-squad="${Number(squad.id)}" data-zone-squad-mode="${mode}"
        data-zone-squad-enabled="${squad[field] ? "0" : "1"}">${label}</button>`;
    selection.className = "visible building-panel zone-panel zone-wide";
    selection.innerHTML = `
      <div class="bld-head"><div class="bld-name">${escapeHtml(data.name || "Zone")} squads</div>
        <button class="bld-x" data-bld-close title="Close">X</button></div>
      <button class="bld-btn" data-zone-back>Back to zone</button>
      <div class="zone-squad-list">
        ${squads.length ? squads.map(squad => `<div class="zone-squad-row${squad.assigned ? " assigned" : ""}">
          <strong>${escapeHtml(squad.name || `Squad ${squad.id}`)}</strong>
          <div class="zone-squad-modes">
            ${modeButton(squad, "sleep", "sleep", "Sleep")}
            ${modeButton(squad, "train", "train", "Train")}
            ${modeButton(squad, "individual-equipment", "individualEquipment", "Individual equip")}
            ${modeButton(squad, "squad-equipment", "squadEquipment", "Squad equip")}
          </div>
        </div>`).join("") : `<div class="zone-note">No fortress squads.</div>`}
      </div>`;
    selection.querySelector("[data-zone-back]")?.addEventListener("click", () => openZonePanel(data.id));
    selection.querySelector("[data-bld-close]")?.addEventListener("click", () => {
      closeSelection();
      focusPage();
    });
    selection.querySelectorAll("[data-zone-squad-mode]").forEach(button => {
      button.addEventListener("click", async event => {
        event.stopPropagation();
        button.disabled = true;
        try {
          await buildingPanelPost("/zone-squad-action", {
            id: data.id,
            squad: button.dataset.zoneSquad,
            mode: button.dataset.zoneSquadMode,
            enabled: button.dataset.zoneSquadEnabled
          });
        } catch (_) {}
        openZoneSquadsPanel(data.id);
      });
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
    decorateObjectAttribution("stockpile", id);
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
