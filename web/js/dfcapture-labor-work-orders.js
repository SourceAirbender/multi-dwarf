  // ---- Interactive Labor tab (Work details), backed by /labor* endpoints ----
  let laborSelected = 0; // index of the selected work detail
  let laborEditingTasks = false;

  async function laborPost(url) {
    const r = await fetch(url, { method: "POST", cache: "no-store" });
    let data = null;
    try { data = await r.json(); } catch (_) {}
    if (!r.ok || (data && data.ok === false)) {
      throw new Error((data && data.error) || "Labor update failed");
    }
    return data || {};
  }

  // Native DF work-detail icons from graphics_interface.txt:
  // INTERFACE_BITS_LABOR uses 8x12 cells; these coords are tile coords.
  const LABOR_ICON_CELL = {
    MINERS:[20,0], WOODCUTTERS:[24,0], HUNTERS:[28,0], PLANTERS:[20,6],
    FISHERMEN:[24,6], STONECUTTERS:[32,0], ENGRAVERS:[36,0],
    PLANT_GATHERERS:[40,0], HAULERS:[44,0], ORDERLIES:[48,0],
    SIEGE_OPERATORS:[12,6], CUSTOM_1:[20,3], CUSTOM_2:[24,3],
    CUSTOM_3:[28,3], CUSTOM_4:[32,3], CUSTOM_5:[36,3],
    CUSTOM_6:[40,3], CUSTOM_7:[44,3], CUSTOM_8:[48,3]
  };
  const LABOR_CATEGORY_COLOR = {
    Woodworking:"#ffd45c",
    Stoneworking:"#e6e3dc",
    Hunting:"#a6ff4d",
    Healthcare:"#d66cff",
    Farming:"#67b7ff",
    Fishing:"#4be7ff",
    Metalsmithing:"#b7b7ff",
    Jewelry:"#55ff8a",
    Crafts:"#ffae54",
    Engineering:"#ffc266",
    Hauling:"#c8b6ff",
    Other:"#b9b1a1",
    None:"#b9b1a1"
  };
  function laborIconStyle(iconKey, width = 32) {
    const key = String(iconKey || "").toUpperCase();
    const c = LABOR_ICON_CELL[key];
    if (!c) return "";
    const scale = width / 32;
    const height = 36 * scale;
    return `width:${width}px;height:${height}px;background-size:${416*scale}px ${144*scale}px;` +
      `background-position:-${c[0]*8*scale}px -${c[1]*12*scale}px`;
  }
  function laborIconMarkup(iconKey, cls, width = 32) {
    const st = laborIconStyle(iconKey, width);
    return st ? `<span class="${cls}" style="${st}"></span>` : "";
  }
  function laborCategoryColor(key) {
    return LABOR_CATEGORY_COLOR[String(key || "Other")] || LABOR_CATEGORY_COLOR.Other;
  }

  async function openLaborPanel() {
    setActiveToolbar("labor");
    activeInfoPanel = "labor";
    clientPanel.className = "visible info-panel";
    if (!clientPanel.querySelector(".labor-grid, .labor-task-panel")) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading labor...</div></div></div>`;
    }
    try {
      const r = await fetch(`/labor?detail=${laborSelected}&t=${Date.now()}`, { cache: "no-store" });
      if (!r.ok) throw new Error("labor failed");
      renderLaborPanel(await r.json());
    } catch (_) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Labor data unavailable.</div></div></div>`;
    }
  }

  function renderLaborPanel(data) {
    const details = Array.isArray(data.details) ? data.details : [];
    const selected = data.selected;
    const sel = details.find(d => d.index === selected) || null;
    if (!sel) laborEditingTasks = false;
    if (sel && sel.noModify) laborEditingTasks = false;
    const mode = sel ? sel.mode : 0;     // 1=everybody, 2=nobody, 3=only selected
    const onlySel = mode === 3;
    const rows = Array.isArray(data.rows) ? data.rows : [];
    const tasks = Array.isArray(data.tasks) ? data.tasks : [];

    const MAIN_TABS = [
      ["Creatures", "citizens"], ["Tasks", "orders"], ["Places", "locations"],
      ["Labor", "labor"], ["Work orders", "workorders"], ["Nobles", "nobles"],
      ["Objects", "objects"], ["Justice", "justice"]
    ];
    const topTabs = MAIN_TABS.map(([label, panel]) =>
      `<button class="info-tab${panel === "labor" ? " active" : ""}" data-labor-goto="${panel}">${escapeHtml(label)}</button>`).join("");
    const sectionTabs = ["Work Details", "Standing orders", "Kitchen", "Stone use"].map((s, i) =>
      `<button class="info-tab${i === 0 ? " active" : ""}" data-labor-section="${escapeHtml(s)}">${escapeHtml(s)}</button>`).join("");

    const sideList = `
      <div class="info-side-list">
        <div class="info-side-item labor-add" data-labor-add><span>+</span><strong>Add new work detail</strong></div>
        ${details.map(d => `
          <div class="info-side-item labor-wd${d.index === selected ? " selected" : ""}" data-labor-detail="${d.index}">
            ${laborIconMarkup(d.iconKey, "labor-wd-icon", 32) || `<span></span>`}
            <strong class="labor-wd-name">${escapeHtml(d.name)}</strong>
          </div>`).join("")}
      </div>`;

    const modeBtn = (label, m) =>
      `<button class="labor-mode-btn${mode === m ? " active" : ""}" data-labor-mode="${m}">${escapeHtml(label)}</button>`;
    const modeRow = sel ? `
      <div class="labor-mode-row">
        ${modeBtn("Everybody does this", 1)}
        ${modeBtn("Only selected do this", 3)}
        ${modeBtn("Nobody does this", 2)}
      </div>` : "";

    const grid = rows.map(r => `
      <div class="labor-row">
        ${unitPortraitMarkup({id: r.id, name: r.name, portraitTexpos: r.portraitTexpos}, "info-portrait-small")}
        <div class="labor-namecell">
          <div class="labor-name" data-unit-id="${r.id}">${escapeHtml(r.name)}</div>
          ${r.assignedTo ? `<div class="labor-assigned">${escapeHtml(r.assignedTo)}</div>` : ""}
        </div>
        <div class="labor-skill">${escapeHtml(r.skillLabel || "")}</div>
        <div class="labor-spec${r.specialist ? " on" : ""}" data-labor-spec="${r.id}" data-spec="${r.specialist ? 1 : 0}" title="${r.specialist ? "Locked: only does its assigned work details (click to allow any free task)" : "Unlocked: does any free task (click to lock to assigned work)"}">${r.specialist ? "&#128274;" : "&#128275;"}</div>
        <div class="labor-check${r.assigned ? " on" : ""}${onlySel ? "" : " disabled"}" data-labor-toggle="${r.id}" data-on="${r.assigned ? 1 : 0}" title="${onlySel ? "Toggle assignment" : "Set mode to 'Only selected' to assign individuals"}">${r.assigned ? "&#10003;" : ""}</div>
      </div>`).join("");

    let lastTaskCat = "";
    const taskRows = tasks.map(t => {
      const cat = t.category || "Other";
      const catColor = laborCategoryColor(t.categoryKey || cat);
      const catStyle = ` style="--labor-cat:${catColor}"`;
      const heading = cat !== lastTaskCat ? `<div class="labor-task-cat"${catStyle}>${escapeHtml(cat)}</div>` : "";
      lastTaskCat = cat;
      return `${heading}
        <div class="labor-task-row"${catStyle}>
          <div class="labor-task-name">${escapeHtml(t.name || t.key || `Labor ${t.id}`)}</div>
          <div class="labor-task-meta">${escapeHtml(t.skillName || t.key || "")}</div>
          <div class="labor-task-native">${laborIconMarkup(t.iconKey, "labor-task-icon", 24)}</div>
          <div class="labor-task-check${t.allowed ? " on" : ""}" data-labor-task="${Number(t.id)}" data-on="${t.allowed ? 1 : 0}">${t.allowed ? "&#10003;" : ""}</div>
        </div>`;
    }).join("") || `<div class="labor-empty">No tasks available.</div>`;

    const header = sel ? `
      <div class="labor-detail-head">
        <div class="labor-name-wrap">
          <input class="labor-name-input" id="laborNameInput" type="text" value="${escapeHtml(sel.name)}" maxlength="64"${sel.noModify ? " disabled" : ""}>
          ${sel.skillName ? `<span class="labor-detail-skill">${escapeHtml(sel.skillName)}</span>` : ""}
        </div>
        <div class="labor-head-actions">
          <button class="labor-icon-btn" data-labor-save-name title="Rename work detail"${sel.noModify ? " disabled" : ""}>&#10003;</button>
          <button class="labor-icon-btn${laborEditingTasks ? " active" : ""}" data-labor-edit-tasks title="Select tasks"${sel.noModify ? " disabled" : ""}>&#9881;</button>
          ${sel.noModify ? "" : `<button class="labor-icon-btn danger" data-labor-delete title="Delete work detail">&times;</button>`}
        </div>
      </div>` : `<div class="info-message">Select a work detail.</div>`;

    const taskPanel = sel ? `
      <div class="labor-task-panel">
        <div class="labor-task-toolbar">
          <div class="labor-task-title">Tasks</div>
          <button class="labor-icon-btn" data-labor-tasks-done title="Done">&#10003;</button>
        </div>
        <div class="labor-task-list">${taskRows}</div>
      </div>` : "";

    const assignmentPanel = `
      ${modeRow}
      ${sel ? `<div class="labor-grid-head"><span></span><span>Name</span><span>Skill</span><span class="labor-col-spec">Lock</span><span class="labor-col-do">${onlySel ? "Do this" : ""}</span></div>` : ""}
      <div class="labor-grid">${grid}</div>`;

    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs">${topTabs}</div>
        <div class="info-section-tabs">${sectionTabs}</div>
        <div class="info-body with-side">
          ${sideList}
          <div class="info-main">
            ${header}
            ${laborEditingTasks ? taskPanel : assignmentPanel}
          </div>
        </div>
        <div class="info-footer"><div>Changes apply to the host fort immediately.</div></div>
      </div>`;

    clientPanel.querySelectorAll("[data-labor-goto]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      if (b.dataset.laborGoto !== "labor") openPanel(b.dataset.laborGoto);
    }));
    clientPanel.querySelectorAll("[data-labor-section]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      const section = b.dataset.laborSection || "";
      if (section === "Work Details") {
        laborEditingTasks = false;
        openLaborPanel();
        return;
      }
      const main = clientPanel.querySelector(".info-main");
      if (main) main.innerHTML = `<div class="info-message">Ask host to set these up</div>`;
      clientPanel.querySelectorAll("[data-labor-section]").forEach(x => x.classList.toggle("active", x === b));
    }));
    clientPanel.querySelector("[data-labor-add]")?.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try {
        const created = await laborPost("/labor-create");
        if (Number.isInteger(Number(created.index))) laborSelected = Number(created.index);
        laborEditingTasks = true;
      } catch (err) {
        window.alert(err.message || "Could not create work detail");
      }
      openLaborPanel();
    });
    clientPanel.querySelectorAll("[data-labor-detail]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      laborSelected = Number(b.dataset.laborDetail);
      laborEditingTasks = false;
      openLaborPanel();
    }));
    const nameInput = clientPanel.querySelector("#laborNameInput");
    const saveLaborName = async () => {
      if (!sel || !nameInput || sel.noModify) return;
      const name = String(nameInput.value || "").trim();
      if (!name) {
        nameInput.value = sel.name || "";
        return;
      }
      try {
        await laborPost(`/labor-rename?detail=${selected}&name=${encodeURIComponent(name)}`);
      } catch (err) {
        window.alert(err.message || "Could not rename work detail");
      }
      openLaborPanel();
    };
    clientPanel.querySelector("[data-labor-save-name]")?.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      saveLaborName();
    });
    nameInput?.addEventListener("keydown", e => {
      if (e.key === "Enter") {
        e.preventDefault(); e.stopPropagation();
        saveLaborName();
      }
    });
    clientPanel.querySelector("[data-labor-edit-tasks]")?.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      if (!sel || sel.noModify) return;
      laborEditingTasks = !laborEditingTasks;
      openLaborPanel();
    });
    clientPanel.querySelector("[data-labor-tasks-done]")?.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      laborEditingTasks = false;
      openLaborPanel();
    });
    clientPanel.querySelector("[data-labor-delete]")?.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      if (!sel || sel.noModify) return;
      if (!window.confirm(`Delete ${sel.name}?`)) return;
      try {
        await laborPost(`/labor-delete?detail=${selected}`);
        laborSelected = details.length > 1 ? Math.max(0, selected - 1) : 0;
        laborEditingTasks = false;
      } catch (err) {
        window.alert(err.message || "Could not delete work detail");
      }
      openLaborPanel();
    });
    clientPanel.querySelectorAll("[data-labor-task]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const id = Number(b.dataset.laborTask);
      const newOn = b.dataset.on === "1" ? 0 : 1;
      try {
        await laborPost(`/labor-task-toggle?detail=${selected}&labor=${id}&on=${newOn}`);
      } catch (err) {
        window.alert(err.message || "Could not update task");
      }
      laborEditingTasks = true;
      openLaborPanel();
    }));
    clientPanel.querySelectorAll("[data-labor-mode]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await fetch(`/labor-mode?detail=${selected}&mode=${Number(b.dataset.laborMode)}`, { method: "POST", cache: "no-store" }); } catch (_) {}
      openLaborPanel();
    }));
    clientPanel.querySelectorAll("[data-labor-toggle]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      if (b.classList.contains("disabled")) return;
      const id = Number(b.dataset.laborToggle);
      const newOn = b.dataset.on === "1" ? 0 : 1;
      try { await fetch(`/labor-toggle?detail=${selected}&unit=${id}&on=${newOn}`, { method: "POST", cache: "no-store" }); } catch (_) {}
      openLaborPanel();
    }));
    clientPanel.querySelectorAll("[data-labor-spec]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const id = Number(b.dataset.laborSpec);
      const newOn = b.dataset.spec === "1" ? 0 : 1;
      try { await fetch(`/labor-specialist?unit=${id}&on=${newOn}`, { method: "POST", cache: "no-store" }); } catch (_) {}
      openLaborPanel();
    }));
    clientPanel.querySelectorAll(".labor-name[data-unit-id]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      const id = Number(b.dataset.unitId);
      if (Number.isInteger(id) && id >= 0) openUnitById(id);
    }));
  }

  // ---- Work orders (Manager), backed by /orders + /order-* endpoints ----
  let woCatalog = null;    // [{cat, items:[{key,label}]}] -- cached
  let woShopCatalog = null;// [{shop, icon, items:[{key,label}]}] -- DF-style by-workshop picker
  let woSelShop = -1;      // -1 = "All tasks", else index into woShopCatalog
  let woPresets = null;    // [preset names] -- cached
  let woTargets = null;    // item condition targets
  let woWorkshops = null;  // workshop/furnace choices
  let woLastOrders = [];   // last fetched order list (for in-place re-render)
  let woHasManager = true; // /orders.hasManager -- gate the tab like native DF
  let woMode = "list";     // "list" (base orders screen) | "new" (task picker) | "conditions"
  let woSelCat = 0;
  let woSelKey = null;
  let woSelOrderId = null;
  let woAmount = 1;
  let woFreq = "OneTime";
  let woSearch = "";
  let woCreateWorkshop = "-1";
  const WORK_ORDERS_ENABLED = true;

  const WO_MAIN_TABS = [
    ["Creatures", "citizens"], ["Tasks", "orders"], ["Places", "locations"],
    ["Labor", "labor"], ["Work orders", "workorders"], ["Nobles", "nobles"],
    ["Objects", "objects"], ["Justice", "justice"]
  ];
  const WO_FREQS = ["OneTime", "Daily", "Monthly", "Seasonally", "Yearly"];
  const WO_COMPARE = [
    ["AtMost", "<="], ["AtLeast", ">="], ["LessThan", "<"],
    ["GreaterThan", ">"], ["Exactly", "="], ["Not", "!="]
  ];
  // DF "Adj" property filters for a condition (keys must match dfcapture.lua CONDITION_ADJECTIVES).
  const WO_ADJECTIVES = [
    ["", "any"], ["metal", "metal"], ["wood", "wooden"], ["stone", "stone"],
    ["hard", "hard"], ["edged", "edged"], ["fire_safe", "fire-safe"], ["magma_safe", "magma-safe"],
    ["non_economic", "non-economic"], ["sharpenable", "sharpenable"], ["cookable", "cookable"],
    ["millable", "millable"], ["dyeable", "dyeable"],
  ];
  let woCondMaterials = [];   // materials for the current condition item type (from /condition-materials)
  let woCondMatItem = "";     // item type woCondMaterials was loaded for
  let woCondSuggestions = []; // suggested conditions for the selected order
  let woCondSuggestFor = null;

  function woFreqLabel(f) { return f === "OneTime" ? "One time" : (f || "One time"); }

  function woSelectedOrder() {
    return woLastOrders.find(o => Number(o.id) === Number(woSelOrderId)) || null;
  }

  function woOrderTitle(o) {
    if (!o) return "";
    const n = Number(o.pos);
    return `#${Number.isFinite(n) ? n + 1 : "?"} ${o.job || "Work order"}`;
  }

  function woWorkshopOptions(selected, includeAnyLabel) {
    const sel = String(selected == null ? -1 : selected);
    const rows = [`<option value="-1"${sel === "-1" ? " selected" : ""}>${escapeHtml(includeAnyLabel || "General manager order")}</option>`];
    (woWorkshops || []).forEach(w => {
      const label = `${w.label || w.kind || "Workshop"} (${w.x},${w.y},${w.z})`;
      rows.push(`<option value="${w.id}"${sel === String(w.id) ? " selected" : ""}>${escapeHtml(label)}</option>`);
    });
    return rows.join("");
  }

  function woCurrentCatalogItems() {
    const cats = Array.isArray(woCatalog) ? woCatalog : [];
    if (woSelCat >= cats.length) woSelCat = 0;
    const group = cats[woSelCat] || { items: [] };
    const q = woSearch.trim().toLowerCase();
    let items = Array.isArray(group.items) ? group.items : [];
    if (q) items = items.filter(it => String(it.label || "").toLowerCase().includes(q));
    return { items, total: Array.isArray(group.items) ? group.items.length : 0 };
  }

  function woCatalogGridHtml() {
    const { items, total } = woCurrentCatalogItems();
    const shown = items.slice(0, 220);
    const html = shown.length ? shown.map(it =>
      `<div class="wo-item${it.key === woSelKey ? " selected" : ""}" data-wo-key="${escapeHtml(it.key)}">${escapeHtml(it.label)}</div>`).join("")
      : `<div class="wo-empty">No matching orders.</div>`;
    const note = items.length > shown.length
      ? `Showing ${shown.length} of ${items.length} matches`
      : `${items.length} of ${total} shown`;
    return { html, note };
  }

  function woWireCatalogItems() {
    clientPanel.querySelectorAll("[data-wo-key]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      woSelKey = (woSelKey === b.dataset.woKey) ? null : b.dataset.woKey;
      woRefreshCatalogGrid();
    }));
  }

  function woRefreshCatalogGrid() {
    const grid = document.getElementById("woGrid");
    const note = document.getElementById("woGridNote");
    if (!grid) return;
    const payload = woCatalogGridHtml();
    grid.innerHTML = payload.html;
    if (note) note.textContent = payload.note;
    woWireCatalogItems();
  }

  async function woApi(path, params = {}) {
    const qs = new URLSearchParams();
    Object.entries(params).forEach(([k, v]) => qs.set(k, v == null ? "" : String(v)));
    qs.set("t", Date.now());
    const r = await fetch(`${path}?${qs.toString()}`, { method: "POST", cache: "no-store" });
    const text = await r.text();
    let data = {};
    try { data = text ? JSON.parse(text) : {}; } catch (_) {}
    if (!r.ok || data.ok === false) throw new Error(data.msg || text.trim() || "request failed");
    return data;
  }

  async function woFetchJson(path) {
    const sep = path.includes("?") ? "&" : "?";
    const r = await fetch(`${path}${sep}t=${Date.now()}`, { cache: "no-store" });
    const text = await r.text();
    let data = {};
    try { data = text ? JSON.parse(text) : {}; } catch (_) {}
    if (!r.ok || data.ok === false) throw new Error(data.error || data.msg || text.trim() || `${path} failed`);
    return data;
  }

  // ---- Condition "Mat" picker + DF-style suggested conditions ----
  function woCondMaterialOpts() {
    const opts = [`<option value="">any material</option>`];
    for (const m of woCondMaterials) {
      const val = `${Number(m.matType)}:${Number(m.matIndex)}`;
      opts.push(`<option value="${val}">${escapeHtml(m.name || val)} (${Number(m.count) || 0})</option>`);
    }
    return opts.join("");
  }
  function renderWoCondMaterialSelect() {
    const sel = document.getElementById("woCondMaterial");
    if (sel) sel.innerHTML = woCondMaterialOpts();
  }
  async function loadWoCondMaterials(item) {
    woCondMatItem = item;
    woCondMaterials = [];
    renderWoCondMaterialSelect();
    if (!item) return;
    try {
      const d = await woFetchJson(`/condition-materials?item=${encodeURIComponent(item)}`);
      if (woCondMatItem === item) {
        woCondMaterials = Array.isArray(d.materials) ? d.materials : [];
        renderWoCondMaterialSelect();
      }
    } catch (_) {}
  }
  function renderWoSuggestions() {
    const el = document.getElementById("woSuggestedConds");
    if (!el) return;
    if (!woCondSuggestions.length) { el.innerHTML = ""; return; }
    el.innerHTML = `<span class="wo-suggest-title">Suggested:</span>` + woCondSuggestions.map((s, i) =>
      `<button class="wo-chip" data-wo-suggest="${i}">+ ${escapeHtml(s.label || "")}</button>`).join("");
    el.querySelectorAll("[data-wo-suggest]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const s = woCondSuggestions[Number(b.dataset.woSuggest)];
      if (!s) return;
      try {
        await woApi("/order-condition-item-add", { id: woCondSuggestFor, item: s.item, compare: s.compare, value: s.value });
        await refreshWorkOrders();
        woSetStatus("Suggested condition added.", false);
      } catch (err) { woSetStatus(err.message || "Could not add suggestion.", true); }
    }));
  }
  async function loadWoSuggestions(orderId) {
    woCondSuggestFor = orderId;
    woCondSuggestions = [];
    renderWoSuggestions();
    try {
      const d = await woFetchJson(`/order-suggested-conditions?id=${encodeURIComponent(orderId)}`);
      if (woCondSuggestFor !== orderId) return;
      woCondSuggestions = Array.isArray(d.suggestions) ? d.suggestions : [];
      renderWoSuggestions();
    } catch (_) {}
  }

  async function loadWorkOrderAuxData() {
    const jobs = [];
    if (!woCatalog) jobs.push(["catalog", async () => { const d = await woFetchJson("/order-catalog"); woCatalog = d.catalog || []; }]);
    if (!woShopCatalog) jobs.push(["shop catalog", async () => { const d = await woFetchJson("/order-catalog-shops"); woShopCatalog = d.shops || []; }]);
    if (!woPresets) jobs.push(["presets", async () => { const d = await woFetchJson("/order-presets"); woPresets = d.presets || []; }]);
    if (!woTargets) jobs.push(["condition targets", async () => { const d = await woFetchJson("/condition-targets"); woTargets = d.targets || []; }]);
    if (!woWorkshops) jobs.push(["workshops", async () => { const d = await woFetchJson("/order-workshops"); woWorkshops = d.workshops || []; }]);
    for (const [label, fn] of jobs) {
      try {
        await fn();
        renderWorkOrders();
      } catch (err) {
        woSetStatus(`Could not load ${label}: ${err.message || err}`, true);
      }
    }
  }

  async function openWorkOrdersPanel() {
    setActiveToolbar("workorders");
    clearBuildPlacement(false);
    activeInfoPanel = "workorders";
    woMode = "list";   // always open on the base orders screen
    clientPanel.className = "visible info-panel";
    if (!WORK_ORDERS_ENABLED) {
      clientPanel.innerHTML = `
        <div class="info-window">
          <div class="info-body">
            <div class="info-message">Work Orders are temporarily disabled while the hang is isolated.</div>
          </div>
        </div>`;
      return;
    }
    if (!clientPanel.querySelector(".wo-cols")) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading work orders...</div></div></div>`;
    }
    try {
      await refreshWorkOrders();
      loadWorkOrderAuxData();
    } catch (_) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Work order data unavailable.</div></div></div>`;
    }
  }

  async function refreshWorkOrders() {
    const data = await woFetchJson("/orders");
    woHasManager = data.hasManager !== false;
    woLastOrders = Array.isArray(data.orders) ? data.orders : [];
    if (!woSelectedOrder()) woSelOrderId = woLastOrders.length ? woLastOrders[0].id : null;
    renderWorkOrders();
  }

  function woSetStatus(msg, isErr) {
    const el = document.getElementById("woStatus");
    if (el) { el.textContent = msg || ""; el.className = "wo-status" + (isErr ? " err" : ""); }
  }

  // ---- Base "list" screen: every order with inline quantity + reorder + conditions button ----
  function woRenderListScreen(orders) {
    const rows = orders.length ? orders.map(o => {
      const condN = (Array.isArray(o.itemConditions) ? o.itemConditions.length : 0) +
                    (Array.isArray(o.orderConditions) ? o.orderConditions.length : 0);
      const meta = [];
      if (o.material) meta.push(escapeHtml(o.material));
      meta.push(`<span class="wo-freq">${escapeHtml(o.frequency === "OneTime" ? "One time" : o.frequency)}</span>`);
      meta.push(escapeHtml(o.workshopName || "Any workshop"));
      const amtTxt = Number(o.amountTotal) > 0 ? `${o.amountLeft}/${o.amountTotal}` : "repeating";
      return `<div class="wo-order" data-wo-row="${o.id}">
        <div class="wo-pos">${Number(o.pos) + 1}</div>
        <div class="wo-order-main">
          <div class="wo-job">${escapeHtml(o.job)} <span class="wo-amt-tag">${escapeHtml(amtTxt)}</span></div>
          <div class="wo-meta">${meta.join(" &middot; ")}</div>
        </div>
        <div class="wo-actions">
          <input class="wo-input narrow wo-row-amt" data-wo-amt="${o.id}" type="number" min="0" max="9999" value="${Number(o.amountTotal) || 0}" title="Quantity (Enter to apply)">
          <button class="wo-icon" data-wo-move="${o.id}" data-dir="-1" title="Move up">&uarr;</button>
          <button class="wo-icon" data-wo-move="${o.id}" data-dir="1" title="Move down">&darr;</button>
          <button class="wo-icon${condN ? " has" : ""}" data-wo-conditions="${o.id}" title="Conditions">&#9881;${condN ? `<span class="wo-cond-badge">${condN}</span>` : ""}</button>
          <button class="wo-icon danger" data-wo-cancel="${o.id}" title="Remove order">&times;</button>
        </div>
      </div>`;
    }).join("") : `<div class="wo-empty">No work orders yet. Click "New work order" to add one.</div>`;
    return `
      <div class="wo-screen">
        <div class="wo-screen-head">
          <div class="wo-section-title">Work orders (${orders.length})</div>
          <button class="wo-btn" data-wo-newscreen>&plus; New work order</button>
        </div>
        <div class="wo-list">${rows}</div>
        <div class="wo-status" id="woStatus"></div>
      </div>`;
  }

  // The tasks shown in the right pane: filtered by search across all shops, else the selected shop.
  function woNewTaskList() {
    const shops = Array.isArray(woShopCatalog) ? woShopCatalog : [];
    const q = (woSearch || "").trim().toLowerCase();
    const out = [], seen = new Set();
    const push = it => { if (!seen.has(it.key)) { seen.add(it.key); out.push(it); } };
    if (q) shops.forEach(s => (s.items || []).forEach(it => { if ((it.label || "").toLowerCase().includes(q)) push(it); }));
    else if (woSelShop < 0) shops.forEach(s => (s.items || []).forEach(push));
    else if (shops[woSelShop]) (shops[woSelShop].items || []).forEach(push);
    return out;
  }
  function woNewTasksHtml() {
    const tasks = woNewTaskList();
    return tasks.length
      ? tasks.map(it => `<button class="wo-task${it.key === woSelKey ? " selected" : ""}" data-wo-task="${escapeHtml(it.key)}">${escapeHtml(it.label)}</button>`).join("")
      : `<div class="wo-empty">No tasks here.</div>`;
  }

  // ---- "new" screen: DF-style picker -- workshops (with icons) on the left, their tasks on the right ----
  function woRenderNewScreen() {
    const shops = Array.isArray(woShopCatalog) ? woShopCatalog : [];
    const freqOpts = WO_FREQS.map(f => `<option value="${f}"${f === woFreq ? " selected" : ""}>${f === "OneTime" ? "One time" : f}</option>`).join("");
    const shopBtns = [`<button class="wo-shop${woSelShop < 0 ? " selected" : ""}" data-wo-shop="-1"><span class="wo-shop-icon" style="${bldIconStyle("workshops", 18)}"></span><span>All tasks</span></button>`]
      .concat(shops.map((s, i) => `<button class="wo-shop${i === woSelShop ? " selected" : ""}" data-wo-shop="${i}"><span class="wo-shop-icon" style="${bldIconStyle(s.icon, 18)}"></span><span>${escapeHtml(s.shop)}</span></button>`)).join("");
    return `
      <div class="wo-screen">
        <div class="wo-screen-head">
          <button class="wo-btn secondary" data-wo-backlist>&larr; Back</button>
          <div class="wo-section-title">New work order</div>
        </div>
        <div class="wo-newpick">
          <div class="wo-shoplist">${shopBtns}</div>
          <div class="wo-taskpane">
            <input class="wo-input wide" id="woSearch" type="text" placeholder="Find a task..." value="${escapeHtml(woSearch)}">
            <div class="wo-tasks" id="woTasks">${woNewTasksHtml()}</div>
          </div>
        </div>
        <div class="wo-form-row">
          <label>Amount</label>
          <input class="wo-input" id="woAmount" type="number" min="1" max="9999" value="${woAmount}">
          <label>Repeat</label>
          <select class="wo-select" id="woFreq">${freqOpts}</select>
          <select class="wo-select wide" id="woCreateWorkshop">${woWorkshopOptions(woCreateWorkshop, "General manager order")}</select>
          <button class="wo-btn" id="woQueue">Queue order</button>
        </div>
        <div class="wo-status" id="woStatus"></div>
      </div>`;
  }
  // In-place refresh of just the task pane (so the search box keeps focus while typing).
  function woRefreshTaskPane() {
    const el = document.getElementById("woTasks");
    if (!el) return;
    el.innerHTML = woNewTasksHtml();
    el.querySelectorAll("[data-wo-task]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      woSelKey = b.dataset.woTask;
      el.querySelectorAll("[data-wo-task]").forEach(x => x.classList.toggle("selected", x.dataset.woTask === woSelKey));
    }));
  }

  // ---- "conditions" screen: order settings + the conditions editor for one order ----
  function woRenderConditionsScreen(selected, orders) {
    const editFreqOpts = WO_FREQS.map(f => `<option value="${f}"${f === selected.frequency ? " selected" : ""}>${escapeHtml(woFreqLabel(f))}</option>`).join("");
    const compareOpts = WO_COMPARE.map(([value, label]) => `<option value="${value}">${escapeHtml(label)}</option>`).join("");
    const targetOpts = (woTargets || []).map(t => `<option value="${escapeHtml(t.item)}">${escapeHtml(t.label)}</option>`).join("");
    const otherOpts = orders.filter(o => Number(o.id) !== Number(selected.id)).map(o => `<option value="${o.id}">${escapeHtml(woOrderTitle(o))}</option>`).join("");
    const condRows = (rows, kind) => rows && rows.length ? rows.map(c => `
      <div class="wo-cond">
        <span>${escapeHtml(c.label || "")}</span>
        <button class="wo-icon danger" data-wo-remove-cond="${selected.id}" data-kind="${kind}" data-idx="${c.idx}" title="Remove condition">&times;</button>
      </div>`).join("") : `<div class="wo-empty">None</div>`;
    return `
      <div class="wo-screen">
        <div class="wo-screen-head">
          <button class="wo-btn secondary" data-wo-backlist>&larr; Back</button>
          <div class="wo-detail-title">${escapeHtml(woOrderTitle(selected))}</div>
        </div>
        <div class="wo-subtle">${escapeHtml(selected.workshopName || "General manager order")} &middot; ${selected.frequency && selected.frequency !== "OneTime" ? "Restarts if completed, conditions checked daily" : "Runs once"}</div>
        <div class="wo-detail-grid">
          <div class="wo-field">
            <div class="wo-field-title">Amount and repeat</div>
            <div class="wo-form-row">
              <input class="wo-input" id="woEditAmount" type="number" min="0" max="9999" value="${Number(selected.amountTotal) || 0}">
              <select class="wo-select" id="woEditFreq">${editFreqOpts}</select>
              <button class="wo-btn" id="woApplyOrder">Apply</button>
            </div>
          </div>
          <div class="wo-field">
            <div class="wo-field-title">Workshop control</div>
            <div class="wo-form-row">
              <select class="wo-select wide" id="woEditWorkshop">${woWorkshopOptions(selected.workshopId, "Any matching workshop")}</select>
              <button class="wo-btn" id="woApplyWorkshop">Set</button>
            </div>
          </div>
        </div>
        <div class="wo-field">
          <div class="wo-field-title">Item conditions</div>
          <div class="wo-cond-list">${condRows(selected.itemConditions || [], "item")}</div>
          <div class="wo-suggested" id="woSuggestedConds"></div>
          <div class="wo-cond-builder">
            <span class="wo-cond-word">Amount of</span>
            <select class="wo-select" id="woCondAdjective">${WO_ADJECTIVES.map(([v, l]) => `<option value="${v}">${escapeHtml(l)}</option>`).join("")}</select>
            <select class="wo-select" id="woCondMaterial">${woCondMaterialOpts()}</select>
            <select class="wo-select" id="woCondItem"><option value="">any item</option>${targetOpts}</select>
            <span class="wo-cond-word">is</span>
            <select class="wo-select" id="woCondCompare">${compareOpts}</select>
            <input class="wo-input narrow" id="woCondValue" type="number" min="0" max="999999" value="10">
            <button class="wo-btn" id="woAddItemCond">Add</button>
          </div>
        </div>
        <div class="wo-field">
          <div class="wo-field-title">Order conditions (run after another order)</div>
          <div class="wo-cond-list">${condRows(selected.orderConditions || [], "order")}</div>
          <div class="wo-form-row">
            <select class="wo-select wide" id="woCondOther">${otherOpts || `<option value="">No other orders</option>`}</select>
            <select class="wo-select" id="woCondType"><option value="Completed">Completed</option><option value="Activated">Activated</option></select>
            <button class="wo-btn" id="woAddOrderCond"${otherOpts ? "" : " disabled"}>Add</button>
          </div>
        </div>
        <div class="wo-status" id="woStatus"></div>`;
  }

  function renderWorkOrders() {
    const orders = woLastOrders;
    let selected = woSelectedOrder();
    const topTabs = WO_MAIN_TABS.map(([label, panel]) =>
      `<button class="info-tab${panel === "workorders" ? " active" : ""}" data-wo-goto="${panel}">${escapeHtml(label)}</button>`).join("");

    // If we're on the conditions screen but the order vanished (cancelled), fall back to the list.
    if (woMode === "conditions" && !selected) woMode = "list";

    let body;
    if (!woHasManager) {
      body = `<div class="wo-manager-required">A manager is required to coordinate work orders.<br>
        <span class="wo-subtle">Assign a dwarf to the Manager noble position (Nobles screen) to queue work orders.</span></div>`;
    } else if (woMode === "new") {
      body = woRenderNewScreen();
    } else if (woMode === "conditions") {
      body = woRenderConditionsScreen(selected, orders);
    } else {
      woMode = "list";
      body = woRenderListScreen(orders);
    }

    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs">${topTabs}</div>
        <div class="info-body" style="grid-template-columns:1fr;">${body}</div>
        <div class="info-footer"><div>Orders are filled by your fort's manager. Changes apply to the host fort immediately.</div></div>
      </div>`;

    // ---- handlers (guarded by element/mode existence; absent ones simply no-op) ----
    clientPanel.querySelectorAll("[data-wo-goto]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      if (b.dataset.woGoto !== "workorders") openPanel(b.dataset.woGoto);
    }));
    const newBtn = clientPanel.querySelector("[data-wo-newscreen]");
    if (newBtn) newBtn.addEventListener("click", e => { e.preventDefault(); e.stopPropagation(); woMode = "new"; woSelKey = null; renderWorkOrders(); });
    clientPanel.querySelectorAll("[data-wo-backlist]").forEach(b => b.addEventListener("click", e => { e.preventDefault(); e.stopPropagation(); woMode = "list"; renderWorkOrders(); }));
    clientPanel.querySelectorAll("[data-wo-conditions]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      woSelOrderId = Number(b.dataset.woConditions); woMode = "conditions"; renderWorkOrders();
    }));
    clientPanel.querySelectorAll("[data-wo-move]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await woApi("/order-reorder", { id: b.dataset.woMove, dir: b.dataset.dir }); await refreshWorkOrders(); woSetStatus("Priority updated.", false); }
      catch (err) { woSetStatus(err.message || "Could not reorder.", true); }
    }));
    clientPanel.querySelectorAll("[data-wo-amt]").forEach(inp => {
      const apply = async () => {
        const o = orders.find(x => Number(x.id) === Number(inp.dataset.woAmt));
        if (!o) return;
        const amt = Math.max(0, Math.min(9999, Number(inp.value) || 0));
        if (amt === Number(o.amountTotal)) return;
        try { await woApi("/order-adjust", { id: inp.dataset.woAmt, amount: amt, frequency: o.frequency }); await refreshWorkOrders(); woSetStatus("Quantity updated.", false); }
        catch (err) { woSetStatus(err.message || "Could not update quantity.", true); }
      };
      inp.addEventListener("change", apply);
      inp.addEventListener("keydown", e => { if (e.key === "Enter") { e.preventDefault(); inp.blur(); } });
      inp.addEventListener("click", e => e.stopPropagation());
    });
    clientPanel.querySelectorAll("[data-wo-cancel]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await woApi("/order-cancel", { id: Number(b.dataset.woCancel) }); await refreshWorkOrders(); woSetStatus("Order removed.", false); }
      catch (err) { woSetStatus(err.message || "Could not remove order.", true); }
    }));

    // new-order screen wiring (DF-style workshop picker)
    clientPanel.querySelectorAll("[data-wo-shop]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      woSelShop = Number(b.dataset.woShop);
      clientPanel.querySelectorAll("[data-wo-shop]").forEach(x => x.classList.toggle("selected", Number(x.dataset.woShop) === woSelShop));
      woRefreshTaskPane();
    }));
    const searchIn = document.getElementById("woSearch");
    if (searchIn) searchIn.addEventListener("input", () => { woSearch = searchIn.value || ""; woRefreshTaskPane(); });
    const amtIn = document.getElementById("woAmount");
    if (amtIn) amtIn.addEventListener("input", () => { woAmount = Math.max(1, Math.min(9999, Number(amtIn.value) || 1)); });
    const freqSel = document.getElementById("woFreq");
    if (freqSel) freqSel.addEventListener("change", () => { woFreq = freqSel.value; });
    const createWorkshopSel = document.getElementById("woCreateWorkshop");
    if (createWorkshopSel) createWorkshopSel.addEventListener("change", () => { woCreateWorkshop = createWorkshopSel.value; });
    if (woMode === "new") woRefreshTaskPane();
    const queueBtn = document.getElementById("woQueue");
    if (queueBtn) queueBtn.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      if (!woSelKey) { woSetStatus("Pick an item to make first.", true); return; }
      woSetStatus("Queuing...", false);
      try {
        const data = await woApi("/order-create", { key: woSelKey, amount: woAmount, frequency: woFreq, workshop: woCreateWorkshop });
        woMode = "list";
        await refreshWorkOrders();
        woSetStatus(data.msg || "Order queued.", false);
      } catch (err) { woSetStatus("Could not queue order: " + (err.message || err), true); }
    });

    // conditions screen wiring
    const applyOrder = document.getElementById("woApplyOrder");
    if (applyOrder && selected) applyOrder.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try {
        await woApi("/order-adjust", { id: selected.id, amount: Math.max(0, Math.min(9999, Number(document.getElementById("woEditAmount")?.value) || 0)), frequency: document.getElementById("woEditFreq")?.value || selected.frequency });
        await refreshWorkOrders(); woSetStatus("Order updated.", false);
      } catch (err) { woSetStatus(err.message || "Could not update order.", true); }
    });
    const applyWorkshop = document.getElementById("woApplyWorkshop");
    if (applyWorkshop && selected) applyWorkshop.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await woApi("/order-workshop", { id: selected.id, workshop: document.getElementById("woEditWorkshop")?.value || -1 }); await refreshWorkOrders(); woSetStatus("Workshop updated.", false); }
      catch (err) { woSetStatus(err.message || "Could not update workshop.", true); }
    });
    const condItemSel = document.getElementById("woCondItem");
    if (condItemSel && selected) condItemSel.addEventListener("change", () => { loadWoCondMaterials(condItemSel.value || ""); });
    const addItemCond = document.getElementById("woAddItemCond");
    if (addItemCond && selected) addItemCond.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try {
        await woApi("/order-condition-item-add", {
          id: selected.id,
          item: document.getElementById("woCondItem")?.value || "",
          material: document.getElementById("woCondMaterial")?.value || "",
          adjective: document.getElementById("woCondAdjective")?.value || "",
          compare: document.getElementById("woCondCompare")?.value || "AtMost",
          value: Math.max(0, Math.min(999999, Number(document.getElementById("woCondValue")?.value) || 0))
        });
        await refreshWorkOrders(); woSetStatus("Item condition added.", false);
      } catch (err) { woSetStatus(err.message || "Could not add condition.", true); }
    });
    if (woMode === "conditions" && selected) {
      loadWoSuggestions(selected.id);
      const firstItem = document.getElementById("woCondItem")?.value || "";
      if (firstItem !== woCondMatItem) loadWoCondMaterials(firstItem); else renderWoCondMaterialSelect();
    }
    const addOrderCond = document.getElementById("woAddOrderCond");
    if (addOrderCond && selected) addOrderCond.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const other = document.getElementById("woCondOther")?.value || "";
      if (!other) return;
      try { await woApi("/order-condition-order-add", { id: selected.id, other, type: document.getElementById("woCondType")?.value || "Completed" }); await refreshWorkOrders(); woSetStatus("Order condition added.", false); }
      catch (err) { woSetStatus(err.message || "Could not add dependency.", true); }
    });
    clientPanel.querySelectorAll("[data-wo-remove-cond]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try { await woApi("/order-condition-remove", { id: b.dataset.woRemoveCond, kind: b.dataset.kind, idx: b.dataset.idx }); await refreshWorkOrders(); woSetStatus("Condition removed.", false); }
      catch (err) { woSetStatus(err.message || "Could not remove condition.", true); }
    }));
  }
