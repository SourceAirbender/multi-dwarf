// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

  // ---- Trade Depot: hauling/broker management plus atomic DF 53.15 browser barter ------------
  let tdDepots = [];
  let tdSelId = null;
  let tdInfo = null;
  let tdGoods = null;
  let tdFilter = "";
  let tdCategoryFilter = "";
  const tdCollapsedCategories = new Set();
  const tdBarterCollapsedCategories = new Set();
  let tdShowAll = false;
  let tdMode = "manage";
  let tdBarter = null;
  let tdBarterError = "";
  let tdTradeCaravan = null;
  let tdMerchantSel = new Map();
  let tdFortSel = new Map();
  let tdPending = null;
  let tdBusy = false;

  async function openTradeDepotPanel(preferredId = null) {
    setActiveToolbar("tradedepot");
    clearBuildPlacement(false);
    activeInfoPanel = "tradedepot";
    const requestedId = Number(preferredId);
    if (Number.isInteger(requestedId) && requestedId >= 0)
      tdMode = "manage";
    clientPanel.className = "visible info-panel";
    if (!clientPanel.querySelector(".td-body"))
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading trade depot...</div></div></div>`;
    try {
      const r = await fetch(`/depots?player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
      const d = await r.json();
      tdDepots = Array.isArray(d.depots) ? d.depots : [];
      if (Number.isInteger(requestedId) && tdDepots.some(x => Number(x.id) === requestedId))
        tdSelId = requestedId;
      else if (tdSelId == null || !tdDepots.some(x => Number(x.id) === Number(tdSelId)))
        tdSelId = tdDepots.length ? Number(tdDepots[0].id) : null;
      await refreshTradeDepot();
    } catch (_) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Trade depot data unavailable.</div></div></div>`;
    }
  }

  async function refreshTradeDepot() {
    tdInfo = null;
    tdGoods = null;
    if (tdSelId != null) {
      try {
        const [ri, rg] = await Promise.all([
          fetch(`/depot-info?player=${encodeURIComponent(player)}&id=${tdSelId}&t=${Date.now()}`, { cache: "no-store" }),
          fetch(`/depot-goods?player=${encodeURIComponent(player)}&id=${tdSelId}${tdShowAll ? "&all=1" : ""}&t=${Date.now()}`, { cache: "no-store" }),
        ]);
        tdInfo = await ri.json().catch(() => null);
        tdGoods = await rg.json().catch(() => null);
      } catch (_) {}
    }
    if (tdMode === "barter")
      await loadTradeBarter(false);
    else
      renderTradeDepot();
  }

  async function loadTradeBarter(resetSelection) {
    tdBarter = null;
    tdBarterError = "";
    if (resetSelection) {
      tdMerchantSel.clear();
      tdFortSel.clear();
      tdPending = null;
    }
    const active = tdInfo && Array.isArray(tdInfo.caravans)
      ? tdInfo.caravans.filter(c => c.active && c.atDepot) : [];
    if (tdTradeCaravan == null || !active.some(c => Number(c.index) === Number(tdTradeCaravan)))
      tdTradeCaravan = active.length ? Number(active[0].index) : null;
    if (tdSelId == null || tdTradeCaravan == null) {
      tdBarterError = "A caravan and both traders must be ready at this depot.";
      renderTradeDepot();
      return;
    }
    try {
      const r = await fetch(`/depot-trade?player=${encodeURIComponent(player)}&id=${tdSelId}&caravan=${tdTradeCaravan}&t=${Date.now()}`, { cache: "no-store" });
      const d = await r.json().catch(() => ({}));
      if (!r.ok || d.ok === false) throw new Error(d.error || "barter is unavailable");
      tdBarter = d;
      const merchantIds = new Set((d.merchant || []).map(x => Number(x.id)));
      const fortIds = new Set((d.fortress || []).map(x => Number(x.id)));
      for (const id of [...tdMerchantSel.keys()]) if (!merchantIds.has(id)) tdMerchantSel.delete(id);
      for (const id of [...tdFortSel.keys()]) if (!fortIds.has(id)) tdFortSel.delete(id);
    } catch (err) {
      tdBarterError = err.message || "Barter is unavailable.";
    }
    renderTradeDepot();
  }

  function tdStatus(msg, isErr) {
    const el = document.getElementById("tdStatus");
    if (el) {
      el.textContent = msg || "";
      el.className = "sq-status" + (isErr ? " err" : "");
    }
  }

  function tdSelectionText(selection) {
    return [...selection.entries()].map(([id, amount]) => `${id}:${Math.max(0, Number(amount) || 0)}`).join(",");
  }

  function tdSelectionValue(items, selection) {
    return items.reduce((sum, item) => {
      if (!selection.has(Number(item.id))) return sum;
      const stack = Math.max(1, Number(item.stack) || 1);
      const amount = Number(selection.get(Number(item.id))) || stack;
      return sum + Math.max(0, Math.round((Number(item.value) || 0) * Math.min(stack, amount) / stack));
    }, 0);
  }

  const TD_CATEGORY_GROUPS = {
    WEAPON: ["weapons", "Weapons"], AMMO: ["weapons", "Weapons"],
    SIEGEAMMO: ["weapons", "Weapons"], TRAPCOMP: ["weapons", "Weapons"],
    TRAPPARTS: ["weapons", "Weapons"],
    ARMOR: ["armor", "Armor & clothing"], SHOES: ["armor", "Armor & clothing"],
    SHIELD: ["armor", "Armor & clothing"], HELM: ["armor", "Armor & clothing"],
    GLOVES: ["armor", "Armor & clothing"], PANTS: ["armor", "Armor & clothing"],
    DOOR: ["furniture", "Furniture"], FLOODGATE: ["furniture", "Furniture"],
    BED: ["furniture", "Furniture"], CHAIR: ["furniture", "Furniture"],
    CHAIN: ["furniture", "Furniture"], WINDOW: ["furniture", "Furniture"],
    CAGE: ["furniture", "Furniture"], TABLE: ["furniture", "Furniture"],
    COFFIN: ["furniture", "Furniture"], STATUE: ["furniture", "Furniture"],
    ARMORSTAND: ["furniture", "Furniture"], WEAPONRACK: ["furniture", "Furniture"],
    CABINET: ["furniture", "Furniture"], HATCH_COVER: ["furniture", "Furniture"],
    GRATE: ["furniture", "Furniture"], QUERN: ["furniture", "Furniture"],
    MILLSTONE: ["furniture", "Furniture"], SLAB: ["furniture", "Furniture"],
    TRACTION_BENCH: ["furniture", "Furniture"],
    FLASK: ["containers", "Containers"], BOX: ["containers", "Containers"],
    BIN: ["containers", "Containers"], BARREL: ["containers", "Containers"],
    BUCKET: ["containers", "Containers"], BACKPACK: ["containers", "Containers"],
    QUIVER: ["containers", "Containers"],
    GOBLET: ["finished", "Finished goods"], FIGURINE: ["finished", "Finished goods"],
    AMULET: ["finished", "Finished goods"], SCEPTER: ["finished", "Finished goods"],
    CROWN: ["finished", "Finished goods"], RING: ["finished", "Finished goods"],
    EARRING: ["finished", "Finished goods"], BRACELET: ["finished", "Finished goods"],
    TOTEM: ["finished", "Finished goods"], TOY: ["finished", "Finished goods"],
    INSTRUMENT: ["finished", "Finished goods"],
    SMALLGEM: ["gems", "Gems"], ROUGH: ["gems", "Gems"], GEM: ["gems", "Gems"],
    BAR: ["materials", "Stone, metal & wood"], BLOCKS: ["materials", "Stone, metal & wood"],
    BOULDER: ["materials", "Stone, metal & wood"], WOOD: ["materials", "Stone, metal & wood"],
    ANVIL: ["tools", "Tools & equipment"], TOOL: ["tools", "Tools & equipment"],
    SPLINT: ["tools", "Tools & equipment"], CRUTCH: ["tools", "Tools & equipment"],
    PIPE_SECTION: ["tools", "Tools & equipment"], CATAPULTPARTS: ["tools", "Tools & equipment"],
    BALLISTAPARTS: ["tools", "Tools & equipment"],
    BALLISTAARROWHEAD: ["tools", "Tools & equipment"],
    MEAT: ["food", "Food & drink"], FISH: ["food", "Food & drink"],
    FISH_RAW: ["food", "Food & drink"], SEEDS: ["food", "Food & drink"],
    PLANT: ["food", "Food & drink"], PLANT_GROWTH: ["food", "Food & drink"],
    DRINK: ["food", "Food & drink"], POWDER_MISC: ["food", "Food & drink"],
    CHEESE: ["food", "Food & drink"], FOOD: ["food", "Food & drink"],
    LIQUID_MISC: ["food", "Food & drink"], EGG: ["food", "Food & drink"],
    SKIN_TANNED: ["cloth", "Cloth & leather"], THREAD: ["cloth", "Cloth & leather"],
    CLOTH: ["cloth", "Cloth & leather"],
    CORPSE: ["remains", "Animals & remains"], CORPSEPIECE: ["remains", "Animals & remains"],
    REMAINS: ["remains", "Animals & remains"], VERMIN: ["remains", "Animals & remains"],
    PET: ["remains", "Animals & remains"],
    BOOK: ["books", "Books & sheets"], SHEET: ["books", "Books & sheets"]
  };

  function tdCategoryInfo(item) {
    const raw = String(item?.category || "OTHER").toUpperCase();
    const known = TD_CATEGORY_GROUPS[raw];
    if (known) return { key: known[0], label: known[1] };
    const label = raw.toLowerCase().replace(/_/g, " ").replace(/\b\w/g, c => c.toUpperCase());
    return { key: `other-${raw.toLowerCase()}`, label: label || "Other" };
  }

  function tdGoodsCategories(goods) {
    const byKey = new Map();
    (goods || []).forEach(item => {
      const category = tdCategoryInfo(item);
      if (!byKey.has(category.key)) byKey.set(category.key, category);
    });
    return [...byKey.values()].sort((a, b) => a.label.localeCompare(b.label));
  }

  const TD_BARTER_CATEGORY_LABELS = {
    BAR: "Bars", SMALLGEM: "Cut gems", BLOCKS: "Blocks", ROUGH: "Rough gems",
    BOULDER: "Stone", WOOD: "Logs", WEAPON: "Weapons", AMMO: "Ammunition",
    ARMOR: "Armor", SHOES: "Footwear", SHIELD: "Shields", HELM: "Helms",
    GLOVES: "Gloves", PANTS: "Legwear", DOOR: "Doors", FLOODGATE: "Floodgates",
    BED: "Beds", CHAIR: "Chairs", CHAIN: "Chains", FLASK: "Flasks",
    GOBLET: "Goblets", INSTRUMENT: "Instruments", TOY: "Toys", WINDOW: "Windows",
    CAGE: "Cages", BARREL: "Barrels", BUCKET: "Buckets", ANIMALTRAP: "Animal traps",
    TABLE: "Tables", COFFIN: "Coffins", STATUE: "Statues", CORPSE: "Corpses",
    CORPSEPIECE: "Body parts", REMAINS: "Remains", MEAT: "Meat", FISH: "Fish",
    FISH_RAW: "Raw fish", SEEDS: "Seeds", PLANT: "Plants",
    PLANT_GROWTH: "Plant products", DRINK: "Drinks", POWDER_MISC: "Powders",
    CHEESE: "Cheese", FOOD: "Prepared food", LIQUID_MISC: "Liquids", EGG: "Eggs",
    SKIN_TANNED: "Leather", THREAD: "Thread", CLOTH: "Cloth", BOX: "Boxes and bags",
    BIN: "Bins", BACKPACK: "Backpacks", QUIVER: "Quivers", TOOL: "Tools",
    ANVIL: "Anvils", SPLINT: "Splints", CRUTCH: "Crutches",
    TRAPCOMP: "Trap components", FIGURINE: "Figurines", AMULET: "Amulets",
    SCEPTER: "Scepters", CROWN: "Crowns", RING: "Rings", EARRING: "Earrings",
    BRACELET: "Bracelets", TOTEM: "Totems", BOOK: "Books", SHEET: "Sheets"
  };

  function tdBarterCategoryInfo(item) {
    const raw = String(item?.category || "OTHER").toUpperCase();
    const fallback = raw.toLowerCase().replace(/_/g, " ")
      .replace(/\b\w/g, char => char.toUpperCase());
    return { key: raw.toLowerCase(), label: TD_BARTER_CATEGORY_LABELS[raw] || fallback || "Other" };
  }

  function tdBarterVisibleItems(items) {
    const filter = tdFilter.trim().toLowerCase();
    return (items || []).filter(item => !filter ||
      String(item.desc || "").toLowerCase().includes(filter));
  }

  function tdBarterMassText(item) {
    const whole = Math.max(0, Number(item?.massWhole) || 0);
    const fraction = Math.max(0, Number(item?.massFraction) || 0);
    if (whole > 0) return `${whole}${fraction ? "+" : ""}`;
    return fraction > 0 ? "<1" : "0";
  }

  function tdBarterRows(items, selection, side) {
    const visible = tdBarterVisibleItems(items);
    if (!visible.length)
      return `<div class="sq-empty">No goods${tdFilter.trim() ? " matching the filter" : ""}.</div>`;
    const groups = new Map();
    visible.forEach(item => {
      const category = tdBarterCategoryInfo(item);
      if (!groups.has(category.key)) groups.set(category.key, { ...category, items: [] });
      groups.get(category.key).items.push(item);
    });
    return [...groups.values()].sort((a, b) => a.label.localeCompare(b.label)).map(group => {
      const collapseKey = `${side}:${group.key}`;
      const collapsed = tdBarterCollapsedCategories.has(collapseKey);
      const selectedCount = group.items.filter(item => selection.has(Number(item.id))).length;
      const allSelected = selectedCount === group.items.length;
      const rows = group.items.map(item => {
        const id = Number(item.id);
        const checked = selection.has(id);
        const stack = Math.max(1, Number(item.stack) || 1);
        const amount = checked ? Math.max(1, Number(selection.get(id)) || stack) : stack;
        return `<label class="td-barter-row${checked ? " selected" : ""}">
          <input type="checkbox" data-td-select="${side}" data-item="${id}"${checked ? " checked" : ""}>
          <span class="td-barter-name">
            <span>${escapeHtml(item.desc || "")}${item.artifact ? ` <b class="td-artifact">artifact</b>` : ""}</span>
            <small>Weight: ${tdBarterMassText(item)}</small>
          </span>
          ${stack > 1 ? `<input class="td-amount" type="number" min="1" max="${stack}" value="${Math.min(stack, amount)}" data-td-amount="${side}" data-item="${id}"${checked ? "" : " disabled"}>` : `<span class="td-stack"></span>`}
          <span class="td-barter-value">Value: ~${Number(item.value) || 0}&#9788;</span>
        </label>`;
      }).join("");
      return `<section class="td-barter-category${collapsed ? " collapsed" : ""}">
        <div class="td-barter-category-head">
          <button type="button" data-td-barter-toggle="${escapeHtml(collapseKey)}"
            aria-expanded="${collapsed ? "false" : "true"}">
            <span aria-hidden="true">${collapsed ? "&#9656;" : "&#9662;"}</span>
            <span>${escapeHtml(group.label)}</span>
            <small>${selectedCount}/${group.items.length}</small>
          </button>
          <button type="button" class="td-category-check${allSelected ? " selected" : ""}"
            data-td-barter-group="${side}" data-td-barter-category="${escapeHtml(group.key)}"
            data-td-barter-check="${allSelected ? "0" : "1"}"
            title="${allSelected ? "Unmark" : "Mark"} this category">&#10003;</button>
        </div>
        ${collapsed ? "" : `<div>${rows}</div>`}
      </section>`;
    }).join("");
  }

  function tdRenderBarterPreservingScroll() {
    const scroll = {};
    clientPanel.querySelectorAll("[data-td-barter-list]").forEach(list => {
      scroll[list.dataset.tdBarterList] = list.scrollTop;
    });
    renderTradeDepot();
    clientPanel.querySelectorAll("[data-td-barter-list]").forEach(list => {
      list.scrollTop = scroll[list.dataset.tdBarterList] || 0;
    });
  }

  function tdBarterSide(side) {
    const merchant = side === "merchant";
    return {
      items: tdBarter ? (merchant ? tdBarter.merchant : tdBarter.fortress) : [],
      selection: merchant ? tdMerchantSel : tdFortSel
    };
  }

  function renderTradeManagement() {
    if (!tdDepots.length)
      return `<div class="sq-hint">No trade depot yet. Build one near your fort entrance so caravans can reach it.</div>`;
    if (!tdInfo || tdInfo.ok === false)
      return `<div class="info-message">Depot unavailable.</div>`;
    const caravans = Array.isArray(tdInfo.caravans) ? tdInfo.caravans : [];
    const activeCars = caravans.filter(c => c.active);
    const carRows = activeCars.length ? activeCars.map(c => `
      <div class="td-caravan">
        <span class="wm-civ-name">${escapeHtml(c.origin || "caravan")}</span>
        <span class="wm-dim">${escapeHtml(c.state || "")}${c.atDepot ? " · at depot" : ""}${Number(c.daysRemaining) > 0 ? ` · ~${c.daysRemaining}d left` : ""} · ${Number(c.goodsCount) || 0} goods</span>
      </div>`).join("") : `<div class="td-empty-state">There are no merchants trading right now.</div>`;
    const b = tdInfo.broker || {};
    const brokerLine = b.found
      ? `Broker: <b>${escapeHtml(b.name || "")}</b> <span class="wm-dim">(${escapeHtml(b.position || "broker")})</span>`
      : `<span class="wm-dim">No broker appointed (Nobles screen).</span>`;
    const traderMode = !tdInfo.traderRequested ? "none"
      : (tdInfo.anyoneCanTrade ? "anyone" : "broker");
    const traderChoices = [
      ["broker", "Broker requested at depot"],
      ["anyone", "Anyone requested at depot"],
      ["none", "No trader needed at depot"]
    ].map(([mode, label]) =>
      `<button class="td-trader-choice${traderMode === mode ? " active" : ""}"
        data-td-trader-mode="${mode}" aria-pressed="${traderMode === mode}">${label}</button>`).join("");
    const materials = Array.isArray(tdInfo.constructionMaterials)
      ? tdInfo.constructionMaterials : [];
    const materialRows = materials.length ? materials.map(item => `
      <div class="td-material">
        <span class="td-material-icon" aria-hidden="true">&#9638;</span>
        <span class="kit-name">${escapeHtml(item.desc || "building material")}</span>
        ${item.forbidden ? `<span class="td-state">forbidden</span>` : ""}
        ${item.dump ? `<span class="td-state">dump</span>` : ""}
        <button class="sq-btn tiny" data-td-material="${Number(item.id)}">View</button>
      </div>`).join("") : `<div class="sq-subtle">No construction materials recorded.</div>`;
    const goods = (tdGoods && Array.isArray(tdGoods.goods)) ? tdGoods.goods : [];
    const filter = tdFilter.trim().toLowerCase();
    const list = goods.filter(g => {
      const category = tdCategoryInfo(g);
      return (!tdCategoryFilter || category.key === tdCategoryFilter) &&
        (!filter || (g.desc || "").toLowerCase().includes(filter));
    });
    const groupedGoods = new Map();
    list.forEach(g => {
      const category = tdCategoryInfo(g);
      if (!groupedGoods.has(category.key))
        groupedGoods.set(category.key, { ...category, goods: [] });
      groupedGoods.get(category.key).goods.push(g);
    });
    const goodRows = list.length ? [...groupedGoods.values()]
      .sort((a, b) => a.label.localeCompare(b.label))
      .map(group => {
        const collapsed = tdCollapsedCategories.has(group.key);
        const marked = group.goods.filter(g => g.pending || g.atDepot).length;
        const rows = group.goods.map(g => `
      <div class="td-good${g.pending || g.atDepot ? " marked" : ""}">
        <span class="kit-name">${escapeHtml(g.desc || "")}</span>
        <span class="td-val">${Number(g.value) || 0}&#9788;</span>
        <span class="td-state">${g.atDepot ? "at depot" : (g.pending ? "hauling" : "")}</span>
        <button class="sq-btn tiny" data-td-mark="${g.id}" data-td-on="${g.pending || g.atDepot ? 0 : 1}">${g.pending || g.atDepot ? "Unmark" : "Trade"}</button>
      </div>`).join("");
        return `<section class="td-goods-category${collapsed ? " collapsed" : ""}">
          <button class="td-category-head" type="button" data-td-category-toggle="${escapeHtml(group.key)}"
            aria-expanded="${collapsed ? "false" : "true"}">
            <span class="td-category-caret" aria-hidden="true">${collapsed ? "&#9656;" : "&#9662;"}</span>
            <span>${escapeHtml(group.label)}</span>
            <span class="td-category-count">${group.goods.length} item${group.goods.length === 1 ? "" : "s"}${marked ? ` &middot; ${marked} marked` : ""}</span>
          </button>
          ${collapsed ? "" : `<div class="td-category-rows">${rows}</div>`}
        </section>`;
      }).join("") : `<div class="sq-empty">No tradeable goods${filter || tdCategoryFilter ? " matching the filters" : ""}.</div>`;
    const truncNote = tdGoods && tdGoods.truncated && !tdShowAll
      ? `<div class="sq-form-row"><button class="sq-btn" data-td-all>Show all (capped at ${Number(tdGoods.cap) || 400})</button></div>` : "";
    return `<div class="sq-section-title">Caravans</div>${carRows}
      <div class="sq-section-title">Depot</div>
      <div class="td-flags">
        <span>${brokerLine}</span>
        <span class="td-access ${tdInfo.accessible ? "ok" : "blocked"}">${tdInfo.accessible ? "Wagon access available" : "No wagon access to this depot"}</span>
        <span class="wm-dim">${Number(tdInfo.goodsAtDepot) || 0} at depot · ${Number(tdInfo.pendingCount) || 0} hauling</span>
      </div>
      <div class="td-trader-choices">${traderChoices}</div>
      <div class="sq-section-title">Goods to haul</div>
      <div class="td-goods">${goodRows}</div>${truncNote}
      <div class="sq-section-title">Construction materials</div>
      <div class="td-materials">${materialRows}</div>`;
  }

  function renderTradeBarter() {
    if (tdBarterError)
      return `<div class="td-barter-unavailable"><b>Barter is not ready</b><span>${escapeHtml(tdBarterError)}</span><button class="sq-btn" data-td-retry>Try again</button></div>`;
    if (!tdBarter)
      return `<div class="info-message">Loading live trade inventories...</div>`;
    const merchant = Array.isArray(tdBarter.merchant) ? tdBarter.merchant : [];
    const fortress = Array.isArray(tdBarter.fortress) ? tdBarter.fortress : [];
    const merchantValue = tdSelectionValue(merchant, tdMerchantSel);
    const fortValue = tdSelectionValue(fortress, tdFortSel);
    const balance = fortValue - merchantValue;
    const merchantName = tdBarter.talker || tdBarter.merchantCiv || "Merchant";
    const merchantCiv = tdBarter.merchantCiv || "the visiting caravan";
    const fortressTrader = tdBarter.fortressTrader || "Fortress trader";
    const pending = tdPending ? `<div class="td-counteroffer">
      <b>Merchant counteroffer</b>
      <span>${escapeHtml(tdPending.message || "The merchants request more goods.")}</span>
      <ul>${(tdPending.counterItems || []).map(x => `<li>${escapeHtml(x.desc || "")}</li>`).join("")}</ul>
      <div class="sq-form-row"><button class="sq-btn" data-td-accept>Accept counteroffer</button><button class="sq-btn danger" data-td-decline>Decline</button></div>
    </div>` : "";
    return `${pending}
      <div class="td-merchant-banner">
        <div><b>${escapeHtml(merchantName)}, Merchant</b></div>
        <div>&ldquo;Greetings from ${escapeHtml(merchantCiv)}. Let us trade!&rdquo;</div>
      </div>
      <div class="td-barter-grid">
        <section>
          <div class="td-barter-title"><b>Merchants from ${escapeHtml(merchantCiv)}</b><span>${merchantValue}&#9788;</span></div>
          <div class="td-barter-list" data-td-barter-list="merchant">${tdBarterRows(merchant, tdMerchantSel, "merchant")}</div>
          <div class="td-barter-pane-foot">
            <span>Selected value: <b>${merchantValue}&#9788;</b></span>
            <button class="sq-btn" data-td-side-all="merchant">Mark all</button>
            <button class="sq-btn" data-td-side-clear="merchant">Unmark all</button>
          </div>
        </section>
        <section>
          <div class="td-barter-title"><b>Your fortress</b><span>${fortValue}&#9788;</span></div>
          <div class="td-barter-list" data-td-barter-list="fort">${tdBarterRows(fortress, tdFortSel, "fort")}</div>
          <div class="td-barter-pane-foot">
            <span>${escapeHtml(fortressTrader)} &middot; <b>${fortValue}&#9788;</b></span>
            <button class="sq-btn" data-td-side-all="fort">Mark all</button>
            <button class="sq-btn" data-td-side-clear="fort">Unmark all</button>
          </div>
        </section>
      </div>
      <div class="td-trade-actions">
        <div class="td-trade-balance">
          <span>Merchant goods: <b>${merchantValue}&#9788;</b></span>
          <span>Fortress goods: <b>${fortValue}&#9788;</b></span>
          <span class="${balance >= 0 ? "positive" : "negative"}">Balance: <b>${balance >= 0 ? "+" : ""}${balance}&#9788;</b></span>
        </div>
        <button class="sq-btn primary td-trade-submit" data-td-submit${tdBusy || !tdMerchantSel.size || !tdFortSel.size || tdPending ? " disabled" : ""}>${tdBusy ? "Trading..." : "Trade"}</button>
      </div>`;
  }

  function renderTradeDepot() {
    const picker = tdDepots.length > 1
      ? `<select id="tdPick" class="sq-select">${tdDepots.map(x => `<option value="${x.id}"${Number(x.id) === Number(tdSelId) ? " selected" : ""}>${escapeHtml(x.name)}</option>`).join("")}</select>`
      : (tdDepots.length === 1 ? `<span class="hosp-title">${escapeHtml(tdDepots[0].name)}</span>` : "");
    const activeCars = tdInfo && Array.isArray(tdInfo.caravans)
      ? tdInfo.caravans.filter(c => c.active && c.atDepot) : [];
    const caravanPicker = tdMode === "barter" && activeCars.length > 1
      ? `<select id="tdCaravanPick" class="sq-select">${activeCars.map(c => `<option value="${c.index}"${Number(c.index) === Number(tdTradeCaravan) ? " selected" : ""}>${escapeHtml(c.origin || "caravan")}</option>`).join("")}</select>` : "";
    const goods = (tdGoods && Array.isArray(tdGoods.goods)) ? tdGoods.goods : [];
    const categories = tdGoodsCategories(goods);
    if (tdMode === "manage" && tdCategoryFilter &&
        !categories.some(category => category.key === tdCategoryFilter))
      tdCategoryFilter = "";
    const categoryPicker = tdMode === "manage" && categories.length
      ? `<select id="tdCategoryFilter" class="sq-select td-category-filter" title="Filter goods by category">
          <option value="">All categories</option>
          ${categories.map(category => `<option value="${escapeHtml(category.key)}"${category.key === tdCategoryFilter ? " selected" : ""}>${escapeHtml(category.label)}</option>`).join("")}
        </select>` : "";
    const body = tdMode === "barter" ? renderTradeBarter() : renderTradeManagement();
    clientPanel.className = "visible info-panel td-panel";
    clientPanel.innerHTML = `<div class="info-window">
      <div class="info-top-tabs">
        <button class="info-tab${tdMode === "manage" ? " active" : ""}" data-td-mode="manage">Depot</button>
        <button class="info-tab${tdMode === "barter" ? " active" : ""}" data-td-mode="barter">Barter</button>
        ${picker}${caravanPicker}${categoryPicker}
        <input id="tdSearch" class="sq-rename kit-search" type="text" placeholder="${tdMode === "barter" ? "filter both inventories..." : "filter goods..."}" value="${escapeHtml(tdFilter)}" spellcheck="false">
        <span id="tdStatus" class="sq-status"></span>
      </div>
      <div class="info-body" style="grid-template-columns:1fr;"><div class="td-body${tdMode === "barter" ? " td-barter-body" : ""}">${body}</div></div>
      ${tdMode === "barter" ? "" : `<div class="info-footer"><div>Mark goods here and dwarves will haul them to the depot for barter.</div></div>`}
    </div>`;
    wireTradeDepot();
  }

  async function postTrade(action) {
    if (tdBusy) return;
    tdBusy = true;
    renderTradeDepot();
    try {
      const p = new URLSearchParams({ player, id: String(tdSelId), caravan: String(tdTradeCaravan), action });
      if (action === "trade") {
        p.set("merchant", tdSelectionText(tdMerchantSel));
        p.set("fort", tdSelectionText(tdFortSel));
      } else if (tdPending && tdPending.token) {
        p.set("token", tdPending.token);
      }
      const r = await fetch(`/depot-trade?${p.toString()}&t=${Date.now()}`, { method: "POST", cache: "no-store" });
      const d = await r.json().catch(() => ({}));
      if (!r.ok || d.ok === false) throw new Error(d.error || "trade failed");
      if (action === "decline" && d.declined) {
        tdPending = null;
        tdStatus(d.message || "Counteroffer declined.", false);
      } else if (d.counterOffer) {
        tdPending = d;
        tdStatus(d.message || "Counteroffer received.", false);
      } else if (d.committed) {
        tdPending = null;
        tdMerchantSel.clear();
        tdFortSel.clear();
        tdStatus(d.message || "Trade completed.", false);
        await refreshTradeDepot();
      } else {
        tdStatus(d.message || "The merchants declined.", true);
      }
    } catch (err) {
      tdStatus(err.message || "Trade could not be applied.", true);
    } finally {
      tdBusy = false;
      renderTradeDepot();
    }
  }

  function wireTradeDepot() {
    clientPanel.querySelectorAll("[data-td-mode]").forEach(btn => btn.addEventListener("click", async () => {
      const next = btn.dataset.tdMode;
      if (next === tdMode) return;
      tdMode = next;
      tdFilter = "";
      tdCategoryFilter = "";
      if (tdMode === "barter") await loadTradeBarter(true); else renderTradeDepot();
    }));
    const pick = document.getElementById("tdPick");
    if (pick) pick.addEventListener("change", async () => {
      tdSelId = Number(pick.value);
      tdBarter = null;
      tdPending = null;
      tdMerchantSel.clear();
      tdFortSel.clear();
      await refreshTradeDepot();
    });
    const carPick = document.getElementById("tdCaravanPick");
    if (carPick) carPick.addEventListener("change", async () => {
      tdTradeCaravan = Number(carPick.value);
      await loadTradeBarter(true);
    });
    const search = document.getElementById("tdSearch");
    if (search) {
      search.addEventListener("click", ev => ev.stopPropagation());
      search.addEventListener("keydown", ev => ev.stopPropagation());
      search.addEventListener("input", () => {
        tdFilter = search.value || "";
        renderTradeDepot();
        const next = document.getElementById("tdSearch");
        if (next) { next.focus(); next.setSelectionRange(next.value.length, next.value.length); }
      });
    }
    const categoryFilter = document.getElementById("tdCategoryFilter");
    if (categoryFilter) categoryFilter.addEventListener("change", () => {
      tdCategoryFilter = categoryFilter.value || "";
      renderTradeDepot();
    });
    clientPanel.querySelectorAll("[data-td-category-toggle]").forEach(button =>
      button.addEventListener("click", () => {
        const category = button.dataset.tdCategoryToggle;
        if (tdCollapsedCategories.has(category)) tdCollapsedCategories.delete(category);
        else tdCollapsedCategories.add(category);
        renderTradeDepot();
      }));
    const retry = clientPanel.querySelector("[data-td-retry]");
    if (retry) retry.addEventListener("click", () => loadTradeBarter(false));
    const allBtn = clientPanel.querySelector("[data-td-all]");
    if (allBtn) allBtn.addEventListener("click", () => { tdShowAll = true; refreshTradeDepot(); });
    clientPanel.querySelectorAll("[data-td-trader-mode]").forEach(button =>
      button.addEventListener("click", async () => {
        const mode = button.dataset.tdTraderMode;
        const request = mode === "none" ? 0 : 1;
        const anyone = mode === "anyone" ? 1 : 0;
        clientPanel.querySelectorAll("[data-td-trader-mode]").forEach(x => x.disabled = true);
        try {
          const response = await fetch(
            `/depot-broker?player=${encodeURIComponent(player)}&id=${tdSelId}&request=${request}&anyone=${anyone}&t=${Date.now()}`,
            { method: "POST", cache: "no-store" });
          const result = await response.json().catch(() => ({}));
          if (!response.ok || result.ok === false)
            throw new Error(result.error || "trader request update failed");
          await refreshTradeDepot();
          tdStatus("Trader request updated.", false);
        } catch (error) {
          tdStatus(error.message || "Could not update trader request.", true);
          clientPanel.querySelectorAll("[data-td-trader-mode]").forEach(x => x.disabled = false);
        }
      }));
    clientPanel.querySelectorAll("[data-td-material]").forEach(button =>
      button.addEventListener("click", () => {
        const id = Number(button.dataset.tdMaterial);
        if (Number.isInteger(id) && id >= 0)
          openItemPanel(id);
      }));
    clientPanel.querySelectorAll("[data-td-mark]").forEach(btn => btn.addEventListener("click", async e => {
      e.preventDefault();
      try {
        const r = await fetch(`/depot-mark?player=${encodeURIComponent(player)}&id=${tdSelId}&item=${btn.dataset.tdMark}&on=${btn.dataset.tdOn}&t=${Date.now()}`, { method: "POST", cache: "no-store" });
        const d = await r.json().catch(() => ({}));
        if (!r.ok || d.ok === false) throw new Error(d.error || "mark failed");
        await refreshTradeDepot();
        tdStatus(btn.dataset.tdOn === "1" ? "Marked for trade." : "Unmarked.", false);
      } catch (err) { tdStatus(err.message || "Could not update.", true); }
    }));
    clientPanel.querySelectorAll("[data-td-barter-toggle]").forEach(button =>
      button.addEventListener("click", () => {
        const key = button.dataset.tdBarterToggle;
        if (tdBarterCollapsedCategories.has(key)) tdBarterCollapsedCategories.delete(key);
        else tdBarterCollapsedCategories.add(key);
        tdRenderBarterPreservingScroll();
      }));
    clientPanel.querySelectorAll("[data-td-barter-group]").forEach(button =>
      button.addEventListener("click", () => {
        const side = button.dataset.tdBarterGroup;
        const category = button.dataset.tdBarterCategory;
        const checked = button.dataset.tdBarterCheck === "1";
        const target = tdBarterSide(side);
        tdBarterVisibleItems(target.items).forEach(item => {
          if (tdBarterCategoryInfo(item).key !== category) return;
          const id = Number(item.id);
          if (checked) target.selection.set(id, Math.max(1, Number(item.stack) || 1));
          else target.selection.delete(id);
        });
        tdRenderBarterPreservingScroll();
      }));
    clientPanel.querySelectorAll("[data-td-side-all]").forEach(button =>
      button.addEventListener("click", () => {
        const target = tdBarterSide(button.dataset.tdSideAll);
        tdBarterVisibleItems(target.items).forEach(item =>
          target.selection.set(Number(item.id), Math.max(1, Number(item.stack) || 1)));
        tdRenderBarterPreservingScroll();
      }));
    clientPanel.querySelectorAll("[data-td-side-clear]").forEach(button =>
      button.addEventListener("click", () => {
        const target = tdBarterSide(button.dataset.tdSideClear);
        tdBarterVisibleItems(target.items).forEach(item => target.selection.delete(Number(item.id)));
        tdRenderBarterPreservingScroll();
      }));
    clientPanel.querySelectorAll("[data-td-select]").forEach(box => box.addEventListener("change", () => {
      const selection = box.dataset.tdSelect === "merchant" ? tdMerchantSel : tdFortSel;
      const id = Number(box.dataset.item);
      const items = box.dataset.tdSelect === "merchant" ? tdBarter.merchant : tdBarter.fortress;
      const item = (items || []).find(x => Number(x.id) === id);
      if (box.checked) selection.set(id, Math.max(1, Number(item && item.stack) || 1));
      else selection.delete(id);
      tdRenderBarterPreservingScroll();
    }));
    clientPanel.querySelectorAll("[data-td-amount]").forEach(input => {
      input.addEventListener("click", ev => ev.stopPropagation());
      input.addEventListener("keydown", ev => ev.stopPropagation());
      input.addEventListener("change", () => {
        const selection = input.dataset.tdAmount === "merchant" ? tdMerchantSel : tdFortSel;
        const id = Number(input.dataset.item);
        const value = Math.max(Number(input.min) || 1, Math.min(Number(input.max) || 1, Number(input.value) || 1));
        if (selection.has(id)) selection.set(id, value);
        tdRenderBarterPreservingScroll();
      });
    });
    const clear = clientPanel.querySelector("[data-td-clear]");
    if (clear) clear.addEventListener("click", () => {
      tdMerchantSel.clear();
      tdFortSel.clear();
      tdPending = null;
      renderTradeDepot();
    });
    const submit = clientPanel.querySelector("[data-td-submit]");
    if (submit) submit.addEventListener("click", () => postTrade("trade"));
    const accept = clientPanel.querySelector("[data-td-accept]");
    if (accept) accept.addEventListener("click", () => postTrade("accept"));
    const decline = clientPanel.querySelector("[data-td-decline]");
    if (decline) decline.addEventListener("click", () => postTrade("decline"));
  }
