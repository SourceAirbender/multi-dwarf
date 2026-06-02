  // ---- Interactive Build menu, backed by /build-catalog and /build-place ----
  let buildCatalog = null;
  let activeBuildCategory = "";
  let buildSearch = "";
  let selectedBuild = null;
  let buildDirection = 0;
  let buildOptions = null;
  let buildStatus = "";
  let buildStatusError = false;
  // DF-style material selection: available materials per requirement for the selected building,
  // and the player's per-requirement pick ("matType:matIndex", or "" = let DF choose any).
  let buildMaterials = null;       // { requirements: [{index,label,quantity,pinned,materials:[...]}] }
  let buildMatPicks = {};          // reqIndex -> "matType:matIndex" | "closest"
  let buildMaterialsToken = "";    // token buildMaterials was loaded for (guards stale responses)
  let lastBuildPicksByToken = {};  // token -> last per-requirement picks (DF-style "use last material")

  function defaultBuildOptions() {
    return {
      hollow: 0,
      weapon_count: 1,
      plate_units: 1,
      plate_water: 0,
      plate_magma: 0,
      plate_track: 0,
      plate_citizens: 0,
      plate_resets: 1,
      unit_min: 1,
      unit_max: 1000000,
      water_min: 1,
      water_max: 7,
      magma_min: 1,
      magma_max: 7,
      track_min: 1,
      track_max: 1000000,
      track_dump: 0,
      dump_x: 0,
      dump_y: 0,
      friction: 50000,
      speed: 50000
    };
  }

  function allBuildItems() {
    return Array.isArray(buildCatalog?.items) ? buildCatalog.items : [];
  }

  function allBuildCategories() {
    return Array.isArray(buildCatalog?.categories) ? buildCatalog.categories : [];
  }

  function itemRequirements(item) {
    return Array.isArray(item?.requirements) ? item.requirements : [];
  }

  function buildItemMeta(item) {
    const size = item.area
      ? `${Number(item.limit?.w) || 31}x${Number(item.limit?.h) || 31}`
      : `${Number(item.size?.w) || 1}x${Number(item.size?.h) || 1}`;
    const reqs = itemRequirements(item);
    const req = reqs.length
      ? reqs.slice(0, 2).map(r => `${Number(r.quantity) < 0 ? "area" : Number(r.quantity) || 1} ${r.label || "material"}`).join(", ")
      : "no materials";
    return `${size} - ${req}`;
  }

  async function loadBuildMaterials(item) {
    const token = item && item.token;
    if (!token) { buildMaterials = null; buildMaterialsToken = ""; return; }
    try {
      const r = await fetch(`/build-materials?token=${encodeURIComponent(token)}&t=${Date.now()}`, { cache: "no-store" });
      if (!r.ok) throw new Error(await r.text());
      const data = await r.json();
      // Ignore a stale response if the player has since picked a different building.
      if (selectedBuild && selectedBuild.token === token) {
        buildMaterials = data && data.ok ? data : null;
        buildMaterialsToken = token;
        // "Use last material": default each requirement to the player's previous pick for this
        // building, but only if that material is still on hand (or it's the "closest" mode).
        const last = lastBuildPicksByToken[token];
        if (last && buildMaterials && Array.isArray(buildMaterials.requirements)) {
          buildMatPicks = {};
          for (const req of buildMaterials.requirements) {
            const v = last[req.index];
            if (!v || req.pinned) continue;
            const avail = v === "closest" ||
              (Array.isArray(req.materials) && req.materials.some(m => `${Number(m.matType)}:${Number(m.matIndex)}` === v));
            if (avail) buildMatPicks[req.index] = v;
          }
        }
        if (clientPanel.classList.contains("build-panel")) renderBuildPanel();
      }
    } catch (_) {
      if (selectedBuild && selectedBuild.token === token) { buildMaterials = null; buildMaterialsToken = token; }
    }
  }

  function selectBuildItem(item, preserveOptions = false) {
    selectedBuild = item || null;
    buildOptions = preserveOptions && buildOptions ? buildOptions : defaultBuildOptions();
    buildMatPicks = {};
    buildMaterials = null;
    buildMaterialsToken = "";
    if (item) loadBuildMaterials(item);
    const dirs = Array.isArray(item?.directions) ? item.directions : [];
    buildDirection = dirs.length ? Number(dirs[0].value) : 0;
    buildStatus = item ? item.label : "";
    buildStatusError = false;
    currentTool = null;
    selectedDesignation = null;
    digMenuOpen = false;
    plantMenuOpen = false;
    smoothMenuOpen = false;
    stockPreset = null;
    stockRepaintId = null;
    if (typeof stockPalette !== "undefined") stockPalette.style.display = "none";
    zonePreset = null;
    if (typeof zonePalette !== "undefined") zonePalette.style.display = "none";
    zoneOverlayEnabled = false;
    currentZones = [];
    renderZoneOverlay();
    updateDesignationButtons();
    updateToolCursor();
  }

  function chooseFirstBuildInCategory() {
    const items = allBuildItems().filter(item => item.category === activeBuildCategory);
    if (!items.length) {
      selectedBuild = null;
      return;
    }
    if (!selectedBuild || selectedBuild.category !== activeBuildCategory)
      selectBuildItem(items[0]);
  }

  async function openBuildPanel() {
    setActiveToolbar("build");
    clientPanel.className = "visible build-panel";
    clientPanel.innerHTML = `<div class="build-window"><div class="build-head"><div class="build-title">Buildings</div><button class="build-close">X</button></div></div>`;
    try {
      const r = await fetch(`/build-catalog?t=${Date.now()}`, { cache: "no-store" });
      if (!r.ok) throw new Error(await r.text());
      buildCatalog = await r.json();
      const cats = allBuildCategories();
      if (!activeBuildCategory || !cats.some(c => c.id === activeBuildCategory))
        activeBuildCategory = cats[0]?.id || "";
      chooseFirstBuildInCategory();
      renderBuildPanel();
    } catch (err) {
      buildStatus = "Building catalog unavailable";
      buildStatusError = true;
      renderBuildPanel();
    }
  }

  // DF's real building-menu icons: building_icons.png (256x512 = 8 cols x 16 rows of 32px tiles),
  // served at /asset. Cell coords come straight from DF's graphics_building_icons.txt.
  const BLD_ICON_CELL = {
    workshops:[0,0], furniture:[1,0], doors_hatches:[2,0], walls_floors:[3,0], machines_fluids:[4,0],
    cages_restraint:[5,0], traps:[6,0], military:[7,0], trade_depot:[0,1], workshop_carpenter:[1,1],
    workshop_mason:[2,1], workshop_metalsmith:[3,1], workshops_furnaces:[4,1], workshop_crafts:[5,1],
    workshop_jeweler:[6,1], workshops_clothing:[7,1], workshops_farming:[0,2], workshop_bowyer:[1,2],
    workshop_mechanic:[2,2], workshop_siege:[3,2], workshop_ashery:[4,2], furnace_wood:[5,2],
    furnace_smelter:[6,2], furnace_glass:[7,2], furnace_kiln:[0,3], workshop_leather:[1,3],
    workshop_loom:[2,3], workshop_clothes:[3,3], workshop_dyer:[4,3], farm_plot:[5,3], workshop_still:[6,3],
    workshop_butcher:[7,3], workshop_tanner:[0,4], workshop_fishery:[1,4], workshop_kitchen:[2,4],
    workshop_farmer:[3,4], workshop_quern:[4,4], workshop_kennel:[5,4], nest_box:[6,4], hive:[7,4],
    bed:[0,5], chair:[1,5], table:[2,5], box:[3,5], cabinet:[4,5], coffin:[5,5], slab:[6,5], statue:[7,5],
    traction_bench:[0,6], bookcase:[1,6], display_furniture:[2,6], offering_place:[3,6], instrument:[4,6],
    door:[5,6], hatch:[6,6], wall:[7,6], floor:[0,7], ramp:[1,7], stairs:[2,7], bridge:[3,7],
    road_paved:[4,7], road_dirt:[5,7], fortification:[6,7], grate_wall:[7,7], grate_floor:[0,8],
    bars_vertical:[1,8], bars_floors:[2,8], window_glass:[3,8], window_gem:[4,8], support:[5,8],
    track:[6,8], track_stop:[7,8], lever:[0,9], well:[1,9], floodgate:[2,9], screw_pump:[3,9],
    water_wheel:[4,9], windmill:[5,9], gear_assembly:[6,9], axle_horizontal:[7,9], axle_vertical:[0,10],
    workshop_millstone:[1,10], rollers:[2,10], restraint:[3,10], cage:[4,10], animal_trap:[5,10],
    pressure_plate:[6,10], trap_stone:[7,10], trap_weapon:[0,11], trap_cage:[1,11], weapon:[2,11],
    archery_target:[3,11], weapon_rack:[4,11], armor_stand:[5,11], ballista:[6,11], catapult:[7,11],
    wagon:[0,12]
  };
  const CAT_ICON = { furniture:"furniture", workshops:"workshops", furnaces:"workshops_furnaces",
    constructions:"walls_floors", machines:"machines_fluids", traps:"traps", siege:"workshop_siege",
    track:"track", farming:"workshops_farming", trade:"trade_depot", doors:"doors_hatches",
    cages:"cages_restraint", military:"military" };
  // Ordered keyword -> icon name; first substring match on the item label wins. Falls back to the
  // category icon, so every item still gets a sensible DF sprite.
  const ITEM_ICON_KW = [
    ["throne","chair"],["chair","chair"],["bed","bed"],["table","table"],["chest","box"],["coffer","box"],
    ["box","box"],["cabinet","cabinet"],["coffin","coffin"],["casket","coffin"],["slab","slab"],
    ["statue","statue"],["armor stand","armor_stand"],["weapon rack","weapon_rack"],["traction","traction_bench"],
    ["bookcase","bookcase"],["display","display_furniture"],["offering","offering_place"],["instrument","instrument"],
    ["hatch","hatch"],["door","door"],["floodgate","floodgate"],["floor grate","grate_floor"],["wall grate","grate_wall"],
    ["grate","grate_floor"],["vertical bars","bars_vertical"],["floor bars","bars_floors"],["bars","bars_vertical"],
    ["gem window","window_gem"],["glass window","window_glass"],["window","window_glass"],["nest","nest_box"],["hive","hive"],
    ["fortification","fortification"],["paved road","road_paved"],["dirt road","road_dirt"],["road","road_paved"],
    ["bridge","bridge"],["wall","wall"],["floor","floor"],["ramp","ramp"],["stair","stairs"],["support","support"],
    ["track stop","track_stop"],["rollers","rollers"],["track","track"],["lever","lever"],["pressure plate","pressure_plate"],
    ["well","well"],["screw pump","screw_pump"],["water wheel","water_wheel"],["windmill","windmill"],
    ["gear","gear_assembly"],["horizontal axle","axle_horizontal"],["vertical axle","axle_vertical"],["axle","axle_horizontal"],
    ["millstone","workshop_millstone"],["quern","workshop_quern"],
    ["cage trap","trap_cage"],["weapon trap","trap_weapon"],["stone-fall","trap_stone"],["stone fall","trap_stone"],
    ["animal trap","animal_trap"],["cage","cage"],["chain","restraint"],["rope","restraint"],["restraint","restraint"],
    ["archery","archery_target"],["weapon rack","weapon_rack"],["ballista","ballista"],["catapult","catapult"],
    ["carpenter","workshop_carpenter"],["mason","workshop_mason"],["metalsmith","workshop_metalsmith"],
    ["craftsdwarf","workshop_crafts"],["craft","workshop_crafts"],["jeweler","workshop_jeweler"],
    ["clothier","workshop_clothes"],["loom","workshop_loom"],["dyer","workshop_dyer"],["leather","workshop_leather"],
    ["tanner","workshop_tanner"],["still","workshop_still"],["kitchen","workshop_kitchen"],["butcher","workshop_butcher"],
    ["fishery","workshop_fishery"],["farmer","workshop_farmer"],["kennel","workshop_kennel"],["ashery","workshop_ashery"],
    ["bowyer","workshop_bowyer"],["mechanic","workshop_mechanic"],["siege","workshop_siege"],
    ["smelter","furnace_smelter"],["glass furnace","furnace_glass"],["kiln","furnace_kiln"],["wood furnace","furnace_wood"],
    ["farm plot","farm_plot"],["depot","trade_depot"]
  ];
  function bldIconStyle(name, px) {
    const c = BLD_ICON_CELL[name];
    if (!c) return "";
    return `background-image:url(/asset/building_icons.png);background-size:${8*px}px ${16*px}px;` +
           `background-position:-${c[0]*px}px -${c[1]*px}px;image-rendering:auto`;
  }
  function catIconName(cat) { return CAT_ICON[String((cat && cat.id) || "").toLowerCase()] || null; }
  function itemIconName(item) {
    const s = String((item && item.label) || "").toLowerCase();
    for (const [kw, name] of ITEM_ICON_KW) if (s.includes(kw)) return name;
    const c = (item && item.category) ? CAT_ICON[String(item.category).toLowerCase()] : null;
    return c || null;
  }
  function catGlyph(cat) {
    const s = String((cat && (cat.label || cat.id)) || "?").trim();
    return s ? s.charAt(0).toUpperCase() : "?";
  }
  function renderBuildPanel() {
    const cats = allBuildCategories();
    const items = allBuildItems();
    if (!activeBuildCategory && cats.length)
      activeBuildCategory = cats[0].id;
    const needle = buildSearch.trim().toLowerCase();
    const shownItems = items.filter(item => item.category === activeBuildCategory)
      .filter(item => !needle || String(item.label || "").toLowerCase().includes(needle));
    if (selectedBuild && !items.some(item => item.token === selectedBuild.token))
      selectedBuild = null;

    const selectedToken = selectedBuild?.token || "";
    const detail = selectedBuild ? renderBuildDetail(selectedBuild) : "";
    clientPanel.className = "visible build-panel";
    clientPanel.innerHTML = `
      <div class="build-window">
        <div class="build-head">
          <div class="build-title">Build</div>
          <input class="build-search" type="search" value="${escapeHtml(buildSearch)}" placeholder="SearchÃ¢â‚¬Â¦" data-build-search>
          <button class="build-close" data-build-close title="Close">Ã¢Å“â€¢</button>
        </div>
        <div class="build-body">
          <div class="build-cats">
            ${cats.map(cat => { const ic = catIconName(cat), st = ic ? bldIconStyle(ic, 26) : ""; return `<button class="build-cat${cat.id === activeBuildCategory ? " active" : ""}" data-build-cat="${escapeHtml(cat.id)}"><span class="build-cat-ico"${st ? ` style="${st}"` : ""}>${st ? "" : escapeHtml(catGlyph(cat))}</span><span class="build-cat-label">${escapeHtml(cat.label || cat.id)}</span><span class="build-count">${Number(cat.count) || 0}</span></button>`; }).join("")}
          </div>
          <div class="build-items">
            ${shownItems.map(item => { const ic = itemIconName(item), st = ic ? bldIconStyle(ic, 30) : ""; return `<button class="build-item${item.token === selectedToken ? " active" : ""}" data-build-token="${escapeHtml(item.token)}" title="${escapeHtml(item.label || "")}"><span class="build-item-ico"${st ? ` style="${st}"` : ""}></span><span class="build-item-text"><span class="build-item-name">${escapeHtml(item.label || "Building")}</span><span class="build-item-meta">${escapeHtml(buildItemMeta(item))}</span></span></button>`; }).join("")}
          </div>
          <div class="build-detail">${detail}</div>
        </div>
        <div class="build-footer">
          <div class="build-status${buildStatusError ? " error" : ""}">${escapeHtml(buildStatus || "")}</div>
          <button class="build-clear" data-build-clear>Cancel</button>
        </div>
      </div>
    `;
    clientPanel.querySelector("[data-build-close]").addEventListener("click", event => {
      event.stopPropagation();
      clearBuildPlacement(false);
      closeClientPanel();
      setActiveToolbar(null);
      focusPage();
    });
    clientPanel.querySelector("[data-build-clear]").addEventListener("click", event => {
      event.stopPropagation();
      clearBuildPlacement(true);
      focusPage();
    });
    clientPanel.querySelector("[data-build-search]").addEventListener("input", event => {
      const pos = event.currentTarget.selectionStart || 0;
      buildSearch = event.currentTarget.value || "";
      renderBuildPanel();
      const next = clientPanel.querySelector("[data-build-search]");
      if (next) {
        next.focus();
        try { next.setSelectionRange(pos, pos); } catch (_) {}
      }
    });
    clientPanel.querySelectorAll("[data-build-cat]").forEach(button => button.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      activeBuildCategory = button.dataset.buildCat || activeBuildCategory;
      selectedBuild = null;
      chooseFirstBuildInCategory();
      renderBuildPanel();
      focusPage();
    }));
    clientPanel.querySelectorAll("[data-build-token]").forEach(button => button.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      const item = items.find(i => i.token === button.dataset.buildToken);
      selectBuildItem(item);
      renderBuildPanel();
      focusPage();
    }));
    clientPanel.querySelectorAll("[data-build-mat]").forEach(sel => sel.addEventListener("change", event => {
      const idx = Number(sel.dataset.buildMat);
      const v = sel.value || "";
      if (v) buildMatPicks[idx] = v; else delete buildMatPicks[idx];
      focusPage();
    }));
    clientPanel.querySelectorAll("[data-build-dir]").forEach(button => button.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      buildDirection = Number(button.dataset.buildDir);
      renderBuildPanel();
      focusPage();
    }));
    clientPanel.querySelectorAll("[data-build-toggle]").forEach(button => button.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      const key = button.dataset.buildToggle;
      buildOptions[key] = Number(button.dataset.value || 0);
      renderBuildPanel();
      focusPage();
    }));
    clientPanel.querySelectorAll("[data-build-dump]").forEach(button => button.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      buildOptions.dump_x = Number(button.dataset.dumpX || 0);
      buildOptions.dump_y = Number(button.dataset.dumpY || 0);
      renderBuildPanel();
      focusPage();
    }));
    clientPanel.querySelectorAll("[data-build-num]").forEach(input => {
      input.addEventListener("change", event => {
        const key = input.dataset.buildNum;
        const min = Number(input.min || 0);
        const max = Number(input.max || 1000000);
        const val = Math.max(min, Math.min(max, Math.floor(Number(input.value || 0))));
        buildOptions[key] = val;
        input.value = String(val);
      });
    });
  }

  function renderBuildDetail(item) {
    const reqs = itemRequirements(item);
    const dirs = Array.isArray(item.directions) ? item.directions : [];
    const qtyText = q => (Number(q) < 0 ? "area" : (Number(q) || 1));
    // Prefer the live material list (DF-style picker) when it matches this building; else fall back
    // to the catalog's plain "Material" text (e.g. while the list is still loading).
    const matReqs = (buildMaterials && buildMaterialsToken === item.token && Array.isArray(buildMaterials.requirements))
      ? buildMaterials.requirements : null;
    let reqHtml;
    if (matReqs && matReqs.length) {
      reqHtml = matReqs.map(req => {
        const idx = Number(req.index);
        if (req.pinned) {
          return `<div class="build-req-row"><span>${qtyText(req.quantity)}</span><span>${escapeHtml(req.label || "Material")}</span></div>`;
        }
        const mats = Array.isArray(req.materials) ? req.materials : [];
        const total = mats.reduce((a, m) => a + (Number(m.count) || 0), 0);
        const pick = buildMatPicks[idx] || "";
        const opts = [
          `<option value=""${pick === "" ? " selected" : ""}>Any material (${total} on hand)</option>`,
          `<option value="closest"${pick === "closest" ? " selected" : ""}>Closest to placement</option>`,
        ].concat(mats.map(m => {
            const val = `${Number(m.matType)}:${Number(m.matIndex)}`;
            return `<option value="${val}"${pick === val ? " selected" : ""}>${escapeHtml(m.name || val)} (${Number(m.count) || 0})</option>`;
          }));
        return `<div class="build-req-row"><span>${qtyText(req.quantity)}</span><span class="build-req-mat"><select class="build-mat-select" data-build-mat="${idx}">${opts.join("")}</select></span></div>`;
      }).join("");
    } else {
      reqHtml = reqs.length
        ? reqs.map(req => `<div class="build-req-row"><span>${escapeHtml(qtyText(req.quantity))}</span><span>${escapeHtml(req.label || "Material")}</span></div>`).join("")
        : `<div class="build-req-row"><span>0</span><span>No materials</span></div>`;
    }
    const directionHtml = item.direction ? `
      <div class="build-section-title">Direction</div>
      <div class="build-dir-row">
        ${dirs.map(dir => `<button class="build-dir${Number(dir.value) === Number(buildDirection) ? " active" : ""}" data-build-dir="${Number(dir.value)}">${escapeHtml(dir.label || dir.value)}</button>`).join("")}
      </div>` : "";
    const hollowHtml = item.hollow ? `
      <div class="build-section-title">Area</div>
      <div class="build-toggle-row">
        <button class="build-toggle${buildOptions.hollow ? " active" : ""}" data-build-toggle="hollow" data-value="${buildOptions.hollow ? 0 : 1}">Hollow</button>
      </div>` : "";
    const weaponHtml = item.weaponCount ? `
      <div class="build-section-title">Weapons</div>
      <div class="build-num-grid">
        ${numInput("weapon_count", "Count", 1, 10)}
      </div>` : "";
    const pressureHtml = item.pressure ? `
      <div class="build-section-title">Triggers</div>
      <div class="build-toggle-row">
        ${toggleButton("plate_units", "Units")}
        ${toggleButton("plate_water", "Water")}
        ${toggleButton("plate_magma", "Magma")}
        ${toggleButton("plate_track", "Minecart")}
        ${toggleButton("plate_citizens", "Citizens")}
        ${toggleButton("plate_resets", "Resets")}
      </div>
      <div class="build-num-grid">
        ${numInput("unit_min", "Unit min", 0, 1000000)}
        ${numInput("unit_max", "Unit max", 0, 1000000)}
        ${numInput("water_min", "Water min", 0, 7)}
        ${numInput("water_max", "Water max", 0, 7)}
        ${numInput("magma_min", "Magma min", 0, 7)}
        ${numInput("magma_max", "Magma max", 0, 7)}
        ${numInput("track_min", "Cart min", 0, 1000000)}
        ${numInput("track_max", "Cart max", 0, 1000000)}
      </div>` : "";
    const dumpDir = `${Number(buildOptions.dump_x) || 0},${Number(buildOptions.dump_y) || 0}`;
    const dumpButton = (label, dx, dy) =>
      `<button class="build-dir${dumpDir === `${dx},${dy}` ? " active" : ""}" data-build-dump data-dump-x="${dx}" data-dump-y="${dy}">${label}</button>`;
    const trackHtml = item.trackStop ? `
      <div class="build-section-title">Track stop</div>
      <div class="build-toggle-row">
        ${toggleButton("track_dump", "Dump")}
      </div>
      <div class="build-dir-row">
        ${dumpButton("None", 0, 0)}
        ${dumpButton("N", 0, -1)}
        ${dumpButton("E", 1, 0)}
        ${dumpButton("S", 0, 1)}
        ${dumpButton("W", -1, 0)}
      </div>
      <div class="build-num-grid">
        ${numInput("friction", "Friction", 0, 50000)}
      </div>` : "";
    const speedHtml = item.speed ? `
      <div class="build-section-title">Speed</div>
      <div class="build-num-grid">
        ${numInput("speed", "Speed", 1000, 100000)}
      </div>` : "";
    return `
      <div class="build-detail-title">${escapeHtml(item.label || "Building")}</div>
      <div class="build-section-title">Needs</div>
      <div class="build-req-list">${reqHtml}</div>
      ${directionHtml}
      ${hollowHtml}
      ${weaponHtml}
      ${pressureHtml}
      ${trackHtml}
      ${speedHtml}
    `;
  }

  function toggleButton(key, label) {
    const on = Number(buildOptions[key] || 0) !== 0;
    return `<button class="build-toggle${on ? " active" : ""}" data-build-toggle="${key}" data-value="${on ? 0 : 1}">${escapeHtml(label)}</button>`;
  }

  function numInput(key, label, min, max) {
    const value = Math.max(min, Math.min(max, Math.floor(Number(buildOptions[key] ?? min))));
    return `<label class="build-num-label">${escapeHtml(label)}<input class="build-num" data-build-num="${key}" type="number" min="${min}" max="${max}" value="${value}"></label>`;
  }

  function clearBuildPlacement(render = true) {
    selectedBuild = null;
    buildStatus = "";
    buildStatusError = false;
    updateToolCursor();
    if (render && clientPanel.classList.contains("build-panel"))
      renderBuildPanel();
  }

  function appendBuildOptions(params, item) {
    const add = key => params.set(key, String(Math.floor(Number(buildOptions[key] ?? 0))));
    add("hollow");
    add("weapon_count");
    add("plate_units"); add("plate_water"); add("plate_magma"); add("plate_track");
    add("plate_citizens"); add("plate_resets");
    add("unit_min"); add("unit_max"); add("water_min"); add("water_max");
    add("magma_min"); add("magma_max"); add("track_min"); add("track_max");
    add("track_dump"); add("dump_x"); add("dump_y"); add("friction"); add("speed");
    // DF-style material picks: mat0..matN per requirement -> "matType:matIndex", or the literal
    // "closest" (backend resolves it to the nearest matching item's material at placement time).
    for (const [idx, val] of Object.entries(buildMatPicks)) {
      if (val === "closest" || /^-?\d+:-?\d+$/.test(val)) params.set(`mat${Number(idx)}`, val);
    }
  }

  async function placeBuildDrag(x1, y1, x2, y2) {
    const item = selectedBuild;
    if (!item) return;
    const a = imagePixelClamped(x1, y1);
    const b = imagePixelClamped(x2, y2);
    if (!a || !b) return;
    const params = new URLSearchParams();
    params.set("player", player);
    params.set("px", String(a.x));
    params.set("py", String(a.y));
    params.set("px2", String(b.x));
    params.set("py2", String(b.y));
    params.set("w", String(a.w));
    params.set("h", String(a.h));
    params.set("token", item.token);
    params.set("direction", String(buildDirection));
    appendBuildOptions(params, item);
    try {
      const r = await fetch(`/build-place?${params.toString()}`, { method: "POST", cache: "no-store" });
      if (!r.ok) {
        const text = await r.text();
        throw new Error(text.trim() || "building failed");
      }
      const data = await r.json();
      buildStatus = `${item.label}: ${Number(data.count) || 1} job${Number(data.count) === 1 ? "" : "s"}`;
      buildStatusError = false;
      // Remember this building's material picks for next time ("use last material").
      if (item.token) lastBuildPicksByToken[item.token] = { ...buildMatPicks };
      renderBuildPanel();
      loadHud();
    } catch (err) {
      buildStatus = String(err.message || err || "Building failed").replace(/^building failed:\s*/i, "");
      buildStatusError = true;
      renderBuildPanel();
    }
  }

  function infoTabButton(tab, kind, activeId) {
    const active = tab.id === activeId ? " active" : "";
    return `<button class="info-tab${active}" data-info-${kind}="${escapeHtml(tab.id)}">${escapeHtml(tab.label)}</button>`;
  }

  function rowTone(text) {
    const lower = String(text || "").toLowerCase();
    if (/vacant|unavailable|wild|no job|dead|missing|new/.test(lower)) return "warn";
    if (/domesticated|selected|assigned|interested|no open|no active/.test(lower)) return "good";
    return "";
  }

  function infoPlaceIconMarkup(row) {
    const sheet = String(row?.iconSheet || "");
    if (sheet === "zone") {
      const ix = Math.max(0, Number(row.iconX) || 0);
      const iy = Math.max(0, Number(row.iconY) || 0);
      return `<span class="info-place-icon zone-icon" style="background-position:-${ix * 32}px -${iy * 32}px"></span>`;
    }
    if (sheet === "stockpile") {
      const rowIdx = Math.max(0, Number(row.iconRow) || 0);
      return `<span class="info-place-icon" style="${spIconStyle(rowIdx, 32)}"></span>`;
    }
    const iconKey = String(row?.iconKey || "");
    const bldStyle = iconKey ? bldIconStyle(iconKey, 32) : "";
    if (bldStyle)
      return `<span class="info-place-icon" style="${bldStyle}"></span>`;
    const glyph = String(row?.name || row?.category || "?").trim().slice(0, 1).toUpperCase() || "?";
    return `<span class="info-place-icon">${escapeHtml(glyph)}</span>`;
  }

  function infoRowPos(row) {
    if (!row || !row.hasPos) return null;
    const x = Number(row.x), y = Number(row.y), z = Number(row.z);
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) return null;
    return { x, y, z };
  }

  function infoRowActions(row) {
    const kind = String(row?.kind || "");
    const id = Number(row?.buildingId ?? -1);
    const itemId = Number(row?.itemId ?? -1);
    const canOpenPlace = id >= 0 && ["stockpile", "workshop", "zone", "building"].includes(kind);
    const canOpenItem = itemId >= 0 && kind === "item";
    const canOpen = canOpenPlace || canOpenItem;
    const canCenter = !!infoRowPos(row);
    if (!canOpen && !canCenter) return "";
    return `<div class="info-row-actions">
      ${canOpen ? `<button class="info-row-action" data-info-open title="Open / manage">&#128269;</button>` : ""}
      ${canCenter ? `<button class="info-row-action" data-info-center title="Center and flash">&#127909;</button>` : ""}
    </div>`;
  }

  function renderInfoRows(rows) {
    if (!Array.isArray(rows) || !rows.length)
      return "";
    return `
      <div class="info-table-head">
        <span></span><span>Name</span><span>Cat</span><span>Prof</span><span>Job / Status</span>
      </div>
      <div class="info-table">
        ${rows.map(row => {
          const hasUnit = Number(row.unitId ?? -1) >= 0;
          const kind = String(row.kind || "");
          const buildingId = Number(row.buildingId ?? -1);
          const itemId = Number(row.itemId ?? -1);
          const clickable = (hasUnit || itemId >= 0 || (buildingId >= 0 && kind)) ? " clickable" : "";
          const status = row.status || "";
          const tone = rowTone(`${status} ${row.job || ""}`);
          const badges = Array.isArray(row.badges) ? row.badges : [];
          const pos = infoRowPos(row);
          return `
            <div class="info-row${clickable}${row.muted ? " info-muted" : ""}"
              data-unit-id="${escapeHtml(row.unitId ?? -1)}"
              data-place-kind="${escapeHtml(kind)}"
              data-building-id="${escapeHtml(row.buildingId ?? -1)}"
              data-item-id="${escapeHtml(row.itemId ?? -1)}"
              ${pos ? `data-pos-x="${escapeHtml(pos.x)}" data-pos-y="${escapeHtml(pos.y)}" data-pos-z="${escapeHtml(pos.z)}"` : ""}>
              ${hasUnit ? unitPortraitMarkup(row, "info-portrait-small") : infoPlaceIconMarkup(row)}
              <div>
                <div class="info-name-main">${escapeHtml(row.name || "")}</div>
                ${row.subtitle ? `<div class="info-subtitle">${escapeHtml(row.subtitle)}</div>` : ""}
              </div>
              <div>${escapeHtml(row.category || "")}</div>
              <div>${escapeHtml(row.profession || "")}</div>
              <div>
                ${status ? `<div class="info-status ${tone}">${escapeHtml(status)}</div>` : ""}
                ${row.job ? `<div class="info-muted">${escapeHtml(row.job)}</div>` : ""}
                ${badges.length ? `<div class="info-badges">${badges.map(badge => `<span class="info-badge">${escapeHtml(badge)}</span>`).join("")}</div>` : ""}
                ${infoRowActions(row)}
              </div>
            </div>
          `;
        }).join("")}
      </div>
    `;
  }

  // Open the DF-style item window for a loose item on the ground (clicked on the map).
  // Uses the read-only "info" action so opening the panel doesn't move the camera or toggle flags.
  async function openItemPanel(id) {
    try {
      const r = await fetch(`/stock-item-action?player=${encodeURIComponent(player)}&id=${id}&action=info&t=${Date.now()}`,
        { method: "POST", cache: "no-store" });
      if (r.ok) { showStockItemSheet(await r.json()); return; }
    } catch (_) {}
    closeSelection();
  }

  function showStockItemSheet(result) {
    const title = result?.title || "Item";
    const glyph = String(title).trim().slice(0, 1).toUpperCase() || "?";
    const lines = Array.isArray(result?.lines) ? result.lines : [];
    const description = result?.description || lines[0] || "";
    const detailLines = description ? lines.slice(1) : lines;
    const holder = result?.holderUnit || null;
    const owner = result?.ownerUnit || null;
    const unit = holder || owner;
    const hasMapPos = result?.mapPos &&
      Number.isFinite(Number(result.mapPos.x)) &&
      Number.isFinite(Number(result.mapPos.y)) &&
      Number.isFinite(Number(result.mapPos.z));
    selection.className = "visible stock-item-panel";
    selection.innerHTML = `
      <div class="stock-item-sheet">
        <button class="unit-close-button" data-stock-item-close title="Close">X</button>
        <div class="stock-item-header">
          <div class="stock-item-glyph">${escapeHtml(glyph)}</div>
          <div>
            <div class="stock-item-title">${escapeHtml(title)}</div>
            ${result?.weight ? `<div class="unit-meta-line stock-item-weight">Weight: ${escapeHtml(result.weight)}</div>` : ""}
            ${result?.location ? `<div class="unit-meta-line">${escapeHtml(result.location)}</div>` : ""}
          </div>
          <div class="stock-item-action-row">
            <button class="item-flag-button${result?.forbidden ? " active" : ""}" data-item-toggle="forbid" title="${result?.forbidden ? "Unforbid item" : "Forbid item"}">&#128274;</button>
            <button class="item-flag-button${result?.dump ? " active" : ""}" data-item-toggle="dump" title="${result?.dump ? "Cancel dump" : "Mark for dumping"}">&#128465;</button>
            <button class="item-flag-button" data-item-follow title="${hasMapPos ? "Move camera to this item" : "No map location"}"${hasMapPos ? "" : " disabled"}>&#127909;</button>
            <button class="item-flag-button${result?.hidden ? " active" : ""}" data-item-toggle="hide" title="${result?.hidden ? "Show item" : "Hide item"}">&#128065;</button>
            ${unit ? `<button class="unit-icon-button" data-stock-item-unit="${escapeHtml(unit.id)}" title="View unit">&#9658;</button>` : ""}
          </div>
        </div>
        <div class="stock-item-body">
          ${description ? `<div class="stock-item-description">${escapeHtml(description)}</div>` : ""}
          ${unit ? `
            <div class="stock-item-unit">
              <div>
                <div class="stock-item-label">${holder ? "With" : "Owned by"}</div>
                <div class="stock-item-unit-name">${escapeHtml(unit.name || `Unit ${unit.id}`)}</div>
              </div>
              <button class="stock-item-view-unit" data-stock-item-unit="${escapeHtml(unit.id)}">View</button>
            </div>
          ` : ""}
          ${Array.isArray(result?.contents) && result.contents.length ? `
            <div class="stock-item-contents">
              <div class="stock-item-label">Contains ${result.contents.length}</div>
              ${result.contents.map(c => `
                <div class="stock-item-content-row" data-stock-item-open="${escapeHtml(c.id)}" title="View this item">
                  <span class="stock-item-content-name">${escapeHtml(c.name)}</span>
                  <span class="stock-item-content-view">&#128269;</span>
                </div>`).join("")}
            </div>
          ` : ""}
          ${detailLines.map(line => {
            const text = String(line);
            const idx = text.indexOf(":");
            if (idx > 0)
              return `<div class="stock-item-line"><span class="stock-item-label">${escapeHtml(text.slice(0, idx + 1))}</span>${escapeHtml(text.slice(idx + 1))}</div>`;
            return `<div class="stock-item-line">${escapeHtml(text)}</div>`;
          }).join("")}
        </div>
      </div>
    `;
    selection.querySelectorAll("[data-stock-item-unit]").forEach(button => {
      button.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        const id = Number(button.dataset.stockItemUnit);
        if (Number.isInteger(id) && id >= 0)
          openUnitById(id);
      });
    });
    const zoom = selection.querySelector("[data-stock-item-zoom]");
    if (zoom) {
      zoom.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        if (result?.mapPos)
          flashMapTile(result.mapPos);
      });
    }
    const close = selection.querySelector("[data-stock-item-close]");
    if (close) {
      close.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        closeSelection();
        focusPage();
      });
    }
    selection.querySelectorAll("[data-stock-item-open]").forEach(row => {
      row.addEventListener("click", async event => {
        event.preventDefault();
        event.stopPropagation();
        const id = Number(row.dataset.stockItemOpen);
        if (!Number.isInteger(id) || id < 0) return;
        try {
          const response = await fetch(`/stock-item-action?player=${encodeURIComponent(player)}&id=${id}&action=view&t=${Date.now()}`, {
            method: "POST", cache: "no-store"
          });
          if (response.ok) showStockItemSheet(await response.json());
        } catch (_) {}
      });
    });
    // Item flag buttons (forbid / dump / visibility) -- toggle then re-render with new state.
    const itemId = Number(result?.id ?? -1);
    selection.querySelectorAll("[data-item-toggle]").forEach(button => {
      button.addEventListener("click", async event => {
        event.preventDefault();
        event.stopPropagation();
        if (!Number.isInteger(itemId) || itemId < 0) return;
        const action = button.dataset.itemToggle;
        try {
          const r = await fetch(`/stock-item-action?player=${encodeURIComponent(player)}&id=${itemId}&action=${encodeURIComponent(action)}&t=${Date.now()}`,
            { method: "POST", cache: "no-store" });
          if (r.ok) showStockItemSheet(await r.json());
        } catch (_) {}
        focusPage();
      });
    });
    // Follow button: move this player's camera onto the item.
    const followBtn = selection.querySelector("[data-item-follow]");
    if (followBtn) {
      followBtn.addEventListener("click", async event => {
        event.preventDefault();
        event.stopPropagation();
        if (!Number.isInteger(itemId) || itemId < 0) return;
        try {
          const r = await fetch(`/stock-item-action?player=${encodeURIComponent(player)}&id=${itemId}&action=follow&t=${Date.now()}`,
            { method: "POST", cache: "no-store" });
          if (r.ok) {
            const res = await r.json();
            if (res.mapPos) flashMapTile(res.mapPos);
          }
        } catch (_) {}
        focusPage();
      });
    }
  }

  function renderStocksPanel(data) {
    const rows = Array.isArray(data.rows) ? data.rows : [];
    activeStockCategory = data.detail || activeStockCategory || "";
    let current = rows.find(row => row.job === activeStockCategory) || rows[0] || null;
    activeStockCategory = current ? (current.job || current.name || "") : "";
    const footer = data.footer || "";
    const selectedCount = current ? (current.status || "None") : "None";
    const stockItems = Array.isArray(data.stockItems) ? data.stockItems : [];
    const itemRows = stockItems.length ? stockItems.map(item => {
      const glyph = String(item.name || "?").trim().slice(0, 1).toUpperCase() || "?";
      return `
        <div class="stocks-item-row" data-stock-item-id="${escapeHtml(item.itemId ?? -1)}">
          <div class="stocks-item-icon">${escapeHtml(glyph)}</div>
          <div>
            <div class="stocks-item-name">${escapeHtml(item.name || "")}${item.status ? ` ${escapeHtml(item.status)}` : ""}</div>
            ${item.subtitle ? `<div class="stocks-item-subtitle">${escapeHtml(item.subtitle)}</div>` : ""}
          </div>
          <div class="stocks-count">${Number(item.count || 1) > 1 ? escapeHtml(item.count) : ""}</div>
          <div class="stocks-item-actions">
            <button class="stocks-item-action" data-stock-action="zoom" title="Zoom to item">X</button>
            <button class="stocks-item-action" data-stock-action="view" title="View item">&#128269;</button>
            <button class="stocks-item-action" data-stock-action="forbid" title="Forbid/unforbid">&#128274;</button>
            <button class="stocks-item-action" data-stock-action="dump" title="Dump">&#128465;</button>
            <button class="stocks-item-action" data-stock-action="hide" title="Hide">&#9679;</button>
          </div>
        </div>
      `;
    }).join("") : `<div class="stocks-detail-line stocks-detail-muted">No visible items in this category.</div>`;
    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window stocks-window">
        <div class="stocks-body">
          <div class="stocks-list">
            ${rows.map(row => {
              const key = row.job || row.name || "";
              const selected = key === activeStockCategory ? " selected" : "";
              return `
                <div class="stocks-row${selected}${row.muted ? " muted" : ""}" data-stock-key="${escapeHtml(key)}">
                  <span>${escapeHtml(row.name || "")}</span>
                  <span class="stocks-count">${escapeHtml(row.status || "None")}</span>
                </div>
              `;
            }).join("")}
          </div>
          <div class="stocks-main">
            <div class="stocks-search-row">
              <div class="stocks-search-box">...</div>
              <div class="stocks-search-button">&#128269;</div>
            </div>
            <div class="stocks-detail">
              ${current ? `
                <h2>${escapeHtml(current.name || "Stocks")}</h2>
                <div class="stocks-detail-line">Count: <strong>${escapeHtml(selectedCount)}</strong></div>
                <div class="stocks-item-list">${itemRows}</div>
              ` : `<div class="stocks-detail-line">No stock categories available.</div>`}
            </div>
          </div>
        </div>
        <div class="info-footer">
          <div class="info-search"></div>
          ${footer ? `<div>${escapeHtml(footer)}</div>` : ""}
        </div>
      </div>
    `;
    clientPanel.querySelectorAll("[data-stock-key]").forEach(row => {
      row.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        activeStockCategory = row.dataset.stockKey || "";
        openPanel("stocks", "stocks", activeStockCategory);
      });
    });
    clientPanel.querySelectorAll("[data-stock-action]").forEach(button => {
      button.addEventListener("click", async event => {
        event.preventDefault();
        event.stopPropagation();
        const row = button.closest("[data-stock-item-id]");
        const id = Number(row?.dataset.stockItemId ?? -1);
        const action = button.dataset.stockAction || "";
        if (!Number.isInteger(id) || id < 0 || !action) return;
        try {
          const response = await fetch(`/stock-item-action?player=${encodeURIComponent(player)}&id=${id}&action=${encodeURIComponent(action)}&t=${Date.now()}`, {
            method: "POST",
            cache: "no-store"
          });
          if (!response.ok) return;
          const result = await response.json();
          if (action === "view") {
            closeClientPanel();
            showStockItemSheet(result);
            if (result.mapPos)
              flashMapTile(result.mapPos);
          }
          if (action === "zoom") {
            closeClientPanel();
            closeSelection();
            if (result.mapPos)
              flashMapTile(result.mapPos);
          }
          if (action === "forbid" || action === "dump" || action === "hide")
            openPanel("stocks", "stocks", activeStockCategory);
        } catch (_) {}
      });
    });
  }

  function renderInfoPanel(data) {
    if ((data.panel || "") === "stocks" || (data.section || "") === "stocks") {
      renderStocksPanel(data);
      return;
    }
    activeInfoPanel = data.panel || activeInfoPanel || "citizens";
    activeInfoSection = data.section || activeInfoSection || defaultSectionForPanel(activeInfoPanel);
    activeInfoDetail = data.detail || "";
    const primaryTabs = Array.isArray(data.primaryTabs) ? data.primaryTabs : [];
    const sectionTabs = Array.isArray(data.sectionTabs) ? data.sectionTabs : [];
    const detailTabs = Array.isArray(data.detailTabs) ? data.detailTabs : [];
    const sideItems = Array.isArray(data.sideItems) ? data.sideItems : [];
    const messages = Array.isArray(data.messages) ? data.messages : [];
    const footer = data.footer || "";
    const bodyClass = sideItems.length ? "info-body with-side" : "info-body";
    const messageHtml = messages.map(line => `<div class="info-message">${escapeHtml(line)}</div>`).join("");
    const sideHtml = sideItems.length ? `
      <div class="info-side-list">
        ${sideItems.map((item, index) => `
          <div class="info-side-item${index === 1 ? " selected" : ""}">
            <span>${index ? "" : "+"}</span><strong>${escapeHtml(item)}</strong>
          </div>
        `).join("")}
      </div>
    ` : "";
    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs">${primaryTabs.map(tab => infoTabButton(tab, "primary", activeInfoSection)).join("")}</div>
        <div class="info-section-tabs">${sectionTabs.map(tab => infoTabButton(tab, "section", activeInfoSection)).join("")}</div>
        ${detailTabs.length ? `<div class="info-detail-tabs">${detailTabs.map(tab => infoTabButton(tab, "detail", activeInfoDetail)).join("")}</div>` : ""}
        <div class="${bodyClass}">
          ${sideHtml}
          <div class="info-main">
            ${messageHtml}
            ${renderInfoRows(data.rows)}
          </div>
        </div>
        <div class="info-footer">
          <div class="info-search"></div>
          ${footer ? `<div>${escapeHtml(footer)}</div>` : ""}
        </div>
      </div>
    `;
    clientPanel.querySelectorAll("[data-info-primary]").forEach(button => {
      button.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        const section = button.dataset.infoPrimary || "";
        openPanel(panelForInfoSection(section), section, "");
      });
    });
    clientPanel.querySelectorAll("[data-info-section]").forEach(button => {
      button.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        const section = button.dataset.infoSection || "";
        openPanel(panelForInfoSection(section), section, "");
      });
    });
    clientPanel.querySelectorAll("[data-info-detail]").forEach(button => {
      button.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        openPanel(activeInfoPanel || "citizens", activeInfoSection || "", button.dataset.infoDetail || "");
      });
    });
    clientPanel.querySelectorAll("[data-unit-id]").forEach(row => {
      row.addEventListener("click", event => {
        if (event.target.closest("[data-info-open], [data-info-center]")) return;
        event.preventDefault();
        event.stopPropagation();
        const id = Number(row.dataset.unitId);
        if (Number.isInteger(id) && id >= 0) {
          openUnitById(id);
          return;
        }
        const kind = row.dataset.placeKind || "";
        const buildingId = Number(row.dataset.buildingId ?? -1);
        const itemId = Number(row.dataset.itemId ?? -1);
        if (Number.isInteger(itemId) && itemId >= 0) {
          closeClientPanel();
          openItemPanel(itemId);
          return;
        }
        if (Number.isInteger(buildingId) && buildingId >= 0 && kind)
          openInfoPlace(kind, buildingId);
      });
    });
    clientPanel.querySelectorAll("[data-info-open]").forEach(button => {
      button.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        const row = button.closest(".info-row");
        const kind = row?.dataset.placeKind || "";
        const buildingId = Number(row?.dataset.buildingId ?? -1);
        const itemId = Number(row?.dataset.itemId ?? -1);
        if (Number.isInteger(itemId) && itemId >= 0) {
          closeClientPanel();
          openItemPanel(itemId);
          return;
        }
        if (Number.isInteger(buildingId) && buildingId >= 0)
          openInfoPlace(kind, buildingId);
      });
    });
    clientPanel.querySelectorAll("[data-info-center]").forEach(button => {
      button.addEventListener("click", async event => {
        event.preventDefault();
        event.stopPropagation();
        const row = button.closest(".info-row");
        const pos = {
          x: Number(row?.dataset.posX),
          y: Number(row?.dataset.posY),
          z: Number(row?.dataset.posZ)
        };
        if (Number.isFinite(pos.x) && Number.isFinite(pos.y) && Number.isFinite(pos.z))
          await centerAndFlashMapPos(pos);
      });
    });
  }

  function openInfoPlace(kind, id) {
    const k = String(kind || "").toLowerCase();
    closeClientPanel();
    if (k === "stockpile") { openStockpilePanel(id); return; }
    if (k === "workshop") { openWorkshopPanel(id); return; }
    if (k === "zone") { openZonePanel(id); return; }
    if (k === "building") { openBuildingPanel(id); return; }
  }

  async function openUnitById(id) {
    try {
      const response = await fetch(`/unit?player=${encodeURIComponent(player)}&id=${encodeURIComponent(id)}&t=${Date.now()}`, { cache: "no-store" });
      if (!response.ok) throw new Error("unit failed");
      const data = await response.json();
      showUnitSheet(data);
    } catch (_) {}
  }

  async function openPanel(name, section = null, detail = null) {
    setActiveToolbar(name);
    if (name !== "zone" && typeof zonePalette !== "undefined") {
      zonePalette.style.display = "none";
      zoneOverlayEnabled = false;
      zonePreset = null;
      currentZones = [];
      renderZoneOverlay();
    }
    if (name === "stockpile") { toggleStockPalette(); return; }
    if (name === "zone") { toggleZonePalette(); return; }
    if (name === "build") { openBuildPanel(); return; }
    if (name === "workorders") { openWorkOrdersPanel(); return; }
    if (name === "alerts") { openNotificationsPanel(); return; }
    clearBuildPlacement(false);
    const backendPanels = new Set(["citizens", "labor", "locations", "orders", "workorders", "nobles", "objects", "justice", "stocks"]);
    if (!backendPanels.has(name)) {
      renderLocalPanel(name);
      return;
    }
    if (name === "labor" || section === "labor") {
      openLaborPanel();
      return;
    }
    activeInfoPanel = name;
    activeInfoSection = section || defaultSectionForPanel(name);
    activeInfoDetail = detail || "";
    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading...</div></div></div>`;
    try {
      const url = `/panel?player=${encodeURIComponent(player)}&panel=${encodeURIComponent(name)}&section=${encodeURIComponent(activeInfoSection)}&detail=${encodeURIComponent(activeInfoDetail)}&t=${Date.now()}`;
      const response = await fetch(url, { cache: "no-store" });
      if (!response.ok) throw new Error("panel failed");
      renderInfoPanel(await response.json());
    } catch (_) {
      clientPanel.className = "visible info-panel";
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Panel data unavailable.</div></div></div>`;
    }
  }
