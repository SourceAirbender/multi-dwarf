  async function performAction(action) {
    if (action === "reset") {
      await resetToHost();
      return;
    }
    try {
      await fetch(`/action?player=${encodeURIComponent(player)}&action=${encodeURIComponent(action)}`, {
        method: "POST",
        cache: "no-store"
      });
    } catch (_) {}
    loadHud();
  }

  function centerFromMinimap(event) {
    if (!currentHud) return;
    const rect = hudEls.minimap.getBoundingClientRect();
    const fx = Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width));
    const fy = Math.max(0, Math.min(1, (event.clientY - rect.top) / rect.height));
    const map = currentHud.map || { w: 1, h: 1 };
    const vp = currentHud.viewport || { w: 80, h: 50 };
    const x = Math.round(fx * map.w - vp.w / 2);
    const y = Math.round(fy * map.h - vp.h / 2);
    const z = currentHud.camera?.z ?? 0;
    resetPanPrediction();
    fetch(`/camera?player=${encodeURIComponent(player)}&x=${x}&y=${y}&z=${z}`, { method: "POST", cache: "no-store" })
      .then(loadHud).catch(() => {});
  }
  // Recenter z to the surface / deepest-discovered level at the camera's location (backend-computed).
  function recenterZ(which) {
    if (!currentHud) return;
    const mm = currentHud.minimap || {};
    const cam = currentHud.camera || { x: 0, y: 0, z: 0 };
    const z = which === "deepest" ? (mm.deepestZ ?? cam.z) : (mm.surfaceZ ?? cam.z);
    resetPanPrediction();
    fetch(`/camera?player=${encodeURIComponent(player)}&x=${cam.x}&y=${cam.y}&z=${z}`, { method: "POST", cache: "no-store" })
      .then(loadHud).catch(() => {});
  }

  document.querySelectorAll("[data-panel]").forEach(button => {
    button.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      openPanel(button.dataset.panel);
      focusPage();
    });
  });

  document.querySelectorAll("[data-action]").forEach(button => {
    button.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      performAction(button.dataset.action);
      focusPage();
    });
  });

  // --- Settings cog: toggle instant (browser-side) vs aesthetic (server-rendered) designations ---
  const settingsBtn = document.getElementById("settingsBtn");
  const settingsMenu = document.getElementById("settingsMenu");
  const setInstantRow = document.getElementById("setInstantDig");
  const setPredictiveRow = document.getElementById("setPredictivePan");
  const setUnitImagesRow = document.getElementById("setUnitImages");
  function refreshSettingsUi() {
    if (setInstantRow) setInstantRow.classList.toggle("on", instantDesignate);
    if (setPredictiveRow) setPredictiveRow.classList.toggle("on", predictivePan);
    if (setUnitImagesRow) setUnitImagesRow.classList.toggle("on", unitImagesEnabled);
    if (settingsBtn) settingsBtn.classList.toggle("sb-active", !!settingsMenu && settingsMenu.classList.contains("open"));
  }
  function setInstantDesignate(on) {
    instantDesignate = !!on;
    try { localStorage.setItem("dfplex.instantDesignate", instantDesignate ? "1" : "0"); } catch (_) {}
    if (instantDesignate) {
      // entering instant mode: drop any server-painted cursor so it doesn't linger in the frame
      if (placementActive()) sendPlacementUi(-1, -1, 0, 0, false, 0, 0, true);
    } else {
      // leaving instant mode: drop the browser preview; the server cursor resumes on next move
      dragPreview = null;
      renderZoneOverlay();
    }
    refreshSettingsUi();
  }
  function setPredictivePan(on) {
    predictivePan = !!on;
    try { localStorage.setItem("dfplex.predictivePan", predictivePan ? "1" : "0"); } catch (_) {}
    if (predictivePan) applyPanPrediction(); else clearPanPrediction();
    refreshSettingsUi();
  }
  function setUnitImagesEnabled(on) {
    unitImagesEnabled = !!on;
    try { localStorage.setItem("dfplex.unitImages", unitImagesEnabled ? "1" : "0"); } catch (_) {}
    if (selection.classList.contains("unit-sheet-panel") && selectedUnitData)
      renderUnitSheet();
    if (activeInfoPanel && clientPanel.classList.contains("visible"))
      openPanel(activeInfoPanel, activeInfoSection || "", activeInfoDetail || "");
    refreshSettingsUi();
  }
  if (settingsBtn && settingsMenu) {
    settingsBtn.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      settingsMenu.classList.toggle("open");
      refreshSettingsUi();
      focusPage();
    });
    if (setInstantRow) setInstantRow.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      setInstantDesignate(!instantDesignate);
    });
    if (setPredictiveRow) setPredictiveRow.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      setPredictivePan(!predictivePan);
    });
    if (setUnitImagesRow) setUnitImagesRow.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      setUnitImagesEnabled(!unitImagesEnabled);
    });
    // close the menu when clicking anywhere outside it
    document.addEventListener("pointerdown", event => {
      if (!settingsMenu.classList.contains("open")) return;
      if (event.target.closest("#settingsMenu, #settingsBtn")) return;
      settingsMenu.classList.remove("open");
      refreshSettingsUi();
    });
  }
  refreshSettingsUi();

  document.querySelectorAll("[data-move-z]").forEach(button => {
    button.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      queueMove(0, 0, Number(button.dataset.moveZ || 0));
      focusPage();
    });
  });

  document.querySelectorAll("[data-recenter]").forEach(button => {
    button.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      recenterZ(button.dataset.recenter);
      focusPage();
    });
  });

  hudEls.minimap.addEventListener("click", event => {
    event.preventDefault();
    event.stopPropagation();
    centerFromMinimap(event);
    focusPage();
  });

  async function inspectClick(event) {
    const pixel = imagePixelFromEvent(event);
    if (!pixel) return;
    try {
      const url = `/inspect?player=${encodeURIComponent(player)}&px=${pixel.x}&py=${pixel.y}&w=${pixel.w}&h=${pixel.h}`;
      const response = await fetch(url, { cache: "no-store" });
      if (!response.ok) throw new Error("inspect failed");
      showSelection(await response.json());
    } catch (_) {
      selection.className = "";
      selection.innerHTML = `<h1>Nothing selected</h1>`;
      selection.classList.add("visible");
    }
  }

  // --- DF-style toolbar sprites and designation menu ---
  let stockPreset = null;
  let stockRepaintId = null;
  let zonePreset = null;
  let currentTool = null; // backend-supported paint tool, or null for inspect/pan mode
  let selectedDesignation = null; // visual selection, including tools not wired yet
  let digMenuOpen = false;
  let plantMenuOpen = false;
  let smoothMenuOpen = false;
  // Dig-menu options (DF parity): priority 1-7 (default 4), marker mode, dig-through-warm/damp,
  // mine mode (0=all,1=automine,2=ore,3=gems), and whether the advanced options are expanded.
  let digPriority = 4;
  let markerMode = false;
  let warmDampMode = false;
  let digMineMode = 0;
  let digAdvOpen = false;
  let plantAdvOpen = false;
  let smoothAdvOpen = false;
  function placementActive() {
    return !!(currentTool || selectedDesignation || stockPreset || stockRepaintId || selectedBuild || zonePreset);
  }
  let lastPlacementActive = null;
  function updatePlacementMode() {
    // Tell the backend whether this player has any placement tool active, so their captured
    // frames get DF's designation grid painted in. selectedDesignation covers visually-selected
    // tools (chop/gather/smooth) that aren't backend paint tools yet -- the grid should still show.
    const active = placementActive();
    if (active === lastPlacementActive) return;
    lastPlacementActive = active;
    fetch(`/placement-mode?player=${encodeURIComponent(player)}&mode=${active ? "dig" : "none"}`,
          { method: "POST", cache: "no-store" }).catch(() => {});
    if (!active) sendPlacementUi(-1, -1, 0, 0, false, 0, 0); // clear cursor/rect on deselect
  }
  // Send the player's hover tile (and drag anchor while dragging) so the backend paints the
  // cursor / rectangle preview into this player's interface layer. Throttled.
  let lastUiSend = 0;
  function sendPlacementUi(hx, hy, w, h, drag, dx, dy, force) {
    const now = performance.now();
    if (!force && now - lastUiSend < 55) return;
    lastUiSend = now;
    // A selected building carries its footprint size; the backend previews the whole footprint
    // (e.g. 3x3 workshop) centered on the cursor instead of a single tile.
    const bw = (selectedBuild && selectedBuild.size && selectedBuild.size.w) || 0;
    const bh = (selectedBuild && selectedBuild.size && selectedBuild.size.h) || 0;
    const q = `player=${encodeURIComponent(player)}&hx=${hx}&hy=${hy}&w=${w}&h=${h}` +
              `&drag=${drag ? 1 : 0}&dx=${dx}&dy=${dy}&bw=${bw}&bh=${bh}`;
    fetch(`/placement-cursor?${q}`, { method: "POST", cache: "no-store" }).catch(() => {});
  }
  function updateToolCursor() {
    view.style.cursor = (currentTool || stockPreset || stockRepaintId || selectedBuild || zonePreset) ? "crosshair" : "";
    updatePlacementMode();
  }
  const SHEET = "/asset/interface_bits.png";
  const SPRITES = {
    lowerMenu:{normal:[4,19]},
    digMenu:{normal:[0,19]},
    dig:{normal:[0,22], active:[4,22]},
    stairs:{normal:[8,22], active:[12,22]},
    ramp:{normal:[0,25], active:[4,25]},
    channel:{normal:[8,25], active:[12,25]},
    remove:{normal:[0,28], active:[4,28]},
    chop:{normal:[0,52], active:[4,52]},
    gather:{normal:[8,52], active:[12,52]},
    smooth:{normal:[0,55], active:[4,55]},
    engrave:{normal:[0,58], active:[4,58]},
    track:{normal:[32,58], active:[36,58]},
    fortify:{normal:[8,58], active:[12,58]},
    erase:{normal:[24,28], active:[24,28]},
    citizens:{normal:[32,22]},
    orders:{normal:[36,22]},
    locations:{normal:[40,22]},
    labor:{normal:[32,25]},
    workorders:{normal:[24,19]},
    nobles:{normal:[36,25]},
    objects:{normal:[40,25]},
    justice:{normal:[32,28], active:[40,52]},
    build:{normal:[16,31]},
    stockpile:{normal:[16,34]},
    zone:{normal:[16,37], active:[20,37]}
  };
  function paintSprite(button, key, active = false) {
    const sprite = SPRITES[key];
    if (!sprite) return;
    let icon = button.querySelector(".sprite-icon");
    if (!icon) {
      button.textContent = "";
      icon = document.createElement("span");
      icon.className = "sprite-icon";
      icon.style.cssText = "width:32px;height:36px;image-rendering:pixelated;background-repeat:no-repeat;";
      button.appendChild(icon);
    }
    const cell = active && sprite.active ? sprite.active : sprite.normal;
    icon.style.backgroundImage = 'url(' + (sprite.sheet || SHEET) + ')';
    if (sprite.sheetW && sprite.sheetH)
      icon.style.backgroundSize = sprite.sheetW + "px " + sprite.sheetH + "px";
    else
      icon.style.backgroundSize = "";
    icon.style.backgroundPosition = "-" + (cell[0] * 8) + "px -" + (cell[1] * 12) + "px";
    button.classList.toggle("active", !!active);
  }
  const TBICON = {
    citizens:"citizens", labor:"labor", locations:"locations", orders:"orders",
    workorders:"workorders", nobles:"nobles", objects:"objects", justice:"justice",
    build:"build", stockpile:"stockpile", zone:"zone"
  };
  const TBGLYPH = { reset:"&#8634;" };
  function refreshToolbarSprites(activeName = "") {
    document.querySelectorAll("#bottomBar [data-panel], #bottomBar [data-action]").forEach(button => {
      const key = button.dataset.panel || button.dataset.action;
      if (TBICON[key]) {
        paintSprite(button, TBICON[key], key === activeName);
      } else if (TBGLYPH[key]) {
        button.innerHTML = TBGLYPH[key];
      }
    });
  }
  refreshToolbarSprites();

  const digSubmenu = document.getElementById("digSubmenu");
  const plantSubmenu = document.getElementById("plantSubmenu");
  const smoothSubmenu = document.getElementById("smoothSubmenu");
  const digMenuButton = document.querySelector("[data-dig-menu]");
  const digTools = new Set(["dig", "stairs", "ramp", "channel", "remove"]);
  const plantTools = new Set(["chop", "gather"]);
  const smoothTools = new Set(["smooth", "engrave", "track", "fortify"]);
  function backendToolFor(tool) {
    return ({ dig:"dig", stairs:"stairs", ramp:"ramp", channel:"channel", remove:"clear",
              erase:"clear", chop:"chop", gather:"gather", smooth:"smooth",
              engrave:"engrave", track:"track", fortify:"fortify" })[tool] || null;
  }
  function updateDesignationButtons() {
    digSubmenu.classList.toggle("visible", digMenuOpen);
    digSubmenu.setAttribute("aria-hidden", digMenuOpen ? "false" : "true");
    plantSubmenu.classList.toggle("visible", plantMenuOpen);
    plantSubmenu.setAttribute("aria-hidden", plantMenuOpen ? "false" : "true");
    smoothSubmenu.classList.toggle("visible", smoothMenuOpen);
    smoothSubmenu.setAttribute("aria-hidden", smoothMenuOpen ? "false" : "true");
    paintSprite(digMenuButton, digMenuOpen ? "lowerMenu" : "digMenu", digMenuOpen || digTools.has(selectedDesignation));
    document.querySelectorAll("[data-dig-tool]").forEach(button => {
      const tool = button.dataset.digTool;
      paintSprite(button, tool, selectedDesignation === tool);
    });
    document.querySelectorAll("[data-plant-tool]").forEach(button => {
      const tool = button.dataset.plantTool;
      button.style.display = (plantMenuOpen && selectedDesignation !== tool) ? "none" : "";
      paintSprite(button, tool, selectedDesignation === tool);
    });
    document.querySelectorAll("[data-smooth-tool]").forEach(button => {
      const tool = button.dataset.smoothTool;
      paintSprite(button, tool, selectedDesignation === tool);
    });
    document.querySelectorAll("[data-designation-tool]").forEach(button => {
      const tool = button.dataset.designationTool;
      if (tool === "chop" || tool === "gather") {
        const openForTool = plantMenuOpen && selectedDesignation === tool;
        paintSprite(button, openForTool ? "lowerMenu" : tool, openForTool || selectedDesignation === tool);
        return;
      }
      if (tool === "smooth") {
        paintSprite(button, smoothMenuOpen ? "lowerMenu" : "smooth",
                    smoothMenuOpen || smoothTools.has(selectedDesignation));
        return;
      }
      let active = selectedDesignation === tool;
      paintSprite(button, tool, active);
    });
    document.querySelectorAll("[data-dig-prio]").forEach(b =>
      b.classList.toggle("active", Number(b.dataset.digPrio) === digPriority));
    document.querySelectorAll("[data-dig-opt]").forEach(b => {
      const on = (b.dataset.digOpt === "marker") ? markerMode : warmDampMode;
      b.classList.toggle("active", on);
    });
    document.querySelectorAll("[data-dig-mode]").forEach(b =>
      b.classList.toggle("active", Number(b.dataset.digMode) === digMineMode));
    const adv = document.querySelector(".dig-adv");
    const exp = document.querySelector("[data-dig-expand]");
    if (adv) adv.classList.toggle("open", digAdvOpen);
    if (exp) exp.classList.toggle("open", digAdvOpen);
    const plantAdv = document.querySelector(".plant-adv");
    const plantExp = document.querySelector("[data-plant-expand]");
    if (plantAdv) plantAdv.classList.toggle("open", plantAdvOpen);
    if (plantExp) plantExp.classList.toggle("open", plantAdvOpen);
    const smoothAdv = document.querySelector(".smooth-adv");
    const smoothExp = document.querySelector("[data-smooth-expand]");
    if (smoothAdv) smoothAdv.classList.toggle("open", smoothAdvOpen);
    if (smoothExp) smoothExp.classList.toggle("open", smoothAdvOpen);
    updateToolCursor();
  }
  function selectDesignation(tool) {
    clearBuildPlacement(false);
    stockPreset = null;
    stockRepaintId = null;
    stockPalette.style.display = "none";
    zonePreset = null;
    if (typeof zonePalette !== "undefined") zonePalette.style.display = "none";
    zoneOverlayEnabled = false;
    currentZones = [];
    renderZoneOverlay();
    selectedDesignation = tool;
    currentTool = backendToolFor(tool);
    digMenuOpen = digMenuOpen && digTools.has(tool);
    plantMenuOpen = plantMenuOpen && plantTools.has(tool);
    smoothMenuOpen = smoothMenuOpen && smoothTools.has(tool);
    updateDesignationButtons();
  }
  digMenuButton.addEventListener("click", event => {
    event.preventDefault();
    event.stopPropagation();
    if (digMenuOpen) {
      // Closing the dig menu also deselects the tool (so the designation grid clears).
      digMenuOpen = false;
      selectedDesignation = null;
      currentTool = null;
    } else {
      digMenuOpen = true;
      plantMenuOpen = false;
      smoothMenuOpen = false;
      if (!digTools.has(selectedDesignation))
        selectedDesignation = "dig";
      currentTool = backendToolFor(selectedDesignation);
    }
    updateDesignationButtons();
    focusPage();
  });
  document.querySelectorAll("[data-dig-tool]").forEach(button => {
    button.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      digMenuOpen = true;
      plantMenuOpen = false;
      smoothMenuOpen = false;
      selectDesignation(button.dataset.digTool);
      focusPage();
    });
  });
  document.querySelectorAll("[data-plant-tool]").forEach(button => {
    button.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      digMenuOpen = false;
      plantMenuOpen = true;
      smoothMenuOpen = false;
      selectDesignation(button.dataset.plantTool);
      focusPage();
    });
  });
  document.querySelectorAll("[data-smooth-tool]").forEach(button => {
    button.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      digMenuOpen = false;
      plantMenuOpen = false;
      smoothMenuOpen = true;
      selectDesignation(button.dataset.smoothTool);
      focusPage();
    });
  });
  document.querySelectorAll("[data-designation-tool]").forEach(button => {
    button.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      const tool = button.dataset.designationTool;
      if (plantTools.has(tool)) {
        if (plantMenuOpen && selectedDesignation === tool) {
          plantMenuOpen = false;
          selectedDesignation = null;
          currentTool = null;
        } else {
          digMenuOpen = false;
          plantMenuOpen = true;
          smoothMenuOpen = false;
          selectDesignation(tool);
        }
      } else if (tool === "smooth") {
        if (smoothMenuOpen && selectedDesignation === "smooth") {
          smoothMenuOpen = false;
          selectedDesignation = null;
          currentTool = null;
        } else {
          digMenuOpen = false;
          plantMenuOpen = false;
          smoothMenuOpen = true;
          selectDesignation("smooth");
        }
      } else {
        digMenuOpen = false;
        plantMenuOpen = false;
        smoothMenuOpen = false;
        selectDesignation(tool);
      }
      updateDesignationButtons();
      focusPage();
    });
  });
  // Dig-menu options: priority 1-7 + marker / warm-damp toggles. These tweak the NEXT designation;
  // they don't change the selected tool, so the dig menu stays open.
  document.querySelectorAll("[data-dig-prio]").forEach(button => {
    button.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      digPriority = Number(button.dataset.digPrio) || 4;
      updateDesignationButtons();
      focusPage();
    });
  });
  document.querySelectorAll("[data-dig-opt]").forEach(button => {
    button.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      if (button.dataset.digOpt === "marker") markerMode = !markerMode;
      else warmDampMode = !warmDampMode;
      updateDesignationButtons();
      focusPage();
    });
  });
  document.querySelectorAll("[data-dig-mode]").forEach(button => {
    button.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      digMineMode = Number(button.dataset.digMode) || 0;
      updateDesignationButtons();
      focusPage();
    });
  });
  document.querySelector("[data-dig-expand]").addEventListener("click", event => {
    event.preventDefault();
    event.stopPropagation();
    digAdvOpen = !digAdvOpen;
    updateDesignationButtons();
    focusPage();
  });
  document.querySelector("[data-plant-expand]").addEventListener("click", event => {
    event.preventDefault();
    event.stopPropagation();
    plantAdvOpen = !plantAdvOpen;
    updateDesignationButtons();
    focusPage();
  });
  document.querySelector("[data-smooth-expand]").addEventListener("click", event => {
    event.preventDefault();
    event.stopPropagation();
    smoothAdvOpen = !smoothAdvOpen;
    updateDesignationButtons();
    focusPage();
  });
  updateDesignationButtons();

  // Dig-menu options applied to every designation request (priority/marker/warm-damp).
  function digOptsQuery() {
    return `&priority=${digPriority}&marker=${markerMode ? 1 : 0}&warmdamp=${warmDampMode ? 1 : 0}&minemode=${digMineMode}`;
  }
  async function designateClick(event) {
    const pixel = imagePixelFromEvent(event);
    if (!pixel) return;
    try {
      const url = `/designate?player=${encodeURIComponent(player)}&px=${pixel.x}&py=${pixel.y}&w=${pixel.w}&h=${pixel.h}&tool=${encodeURIComponent(currentTool)}` + digOptsQuery();
      await fetch(url, { method: "POST", cache: "no-store" });
    } catch (_) {}
  }

  // Designate a rectangle from one screen corner to another (a single click is just
  // a 1x1 rectangle). The backend maps both corners to world tiles.
  async function designateDrag(x1, y1, x2, y2) {
    if (!currentTool) return;
    const a = imagePixelClamped(x1, y1);
    const b = imagePixelClamped(x2, y2);
    if (!a || !b) return;
    try {
      const url = `/designate?player=${encodeURIComponent(player)}&px=${a.x}&py=${a.y}` +
        `&px2=${b.x}&py2=${b.y}&w=${a.w}&h=${a.h}&tool=${encodeURIComponent(currentTool)}` + digOptsQuery();
      await fetch(url, { method: "POST", cache: "no-store" });
    } catch (_) {}
  }

  // --- Stockpile placement: pick a category, then drag a rectangle to create one ---
  const STOCK_PRESETS = [
    ["Everything", "all"], ["Food", "food"], ["Stone", "stone"], ["Wood", "wood"],
    ["Furniture", "furniture"], ["Finished goods", "finished"], ["Bars & blocks", "bars"],
    ["Gems", "gems"], ["Cloth", "cloth"], ["Leather", "leather"], ["Sheets", "sheets"], ["Ammo", "ammo"],
    ["Armor", "armor"], ["Weapons", "weapons"], ["Animals", "animals"], ["Refuse", "refuse"],
    ["Corpses", "corpses"], ["Coins", "coins"]
  ];
  const stockPalette = document.createElement("div");
  stockPalette.id = "stockPalette";
  stockPalette.style.display = "none";
  stockPalette.innerHTML = `
    <div class="stock-palette-title">Stockpile &mdash; pick what it stores, then drag a rectangle on the map</div>
    <div class="stock-palette-status" data-stock-status></div>
    <div class="stock-palette-grid">
      ${STOCK_PRESETS.map(([label, key]) => `<button class="stock-preset-btn" data-stock-preset="${key}">${label}</button>`).join("")}
    </div>
    <button class="stock-palette-close" data-stock-palette-close>Done</button>
  `;
  document.body.appendChild(stockPalette);

  function setStockStatus(msg, isErr = false) {
    const el = stockPalette.querySelector("[data-stock-status]");
    if (!el) return;
    el.textContent = msg || "";
    el.classList.toggle("err", !!isErr);
  }

  function setStockPreset(key) {
    stockPreset = key;
    if (key) { // leaving any dig/designation mode
      clearBuildPlacement(false);
      stockRepaintId = null;
      zonePreset = null;
      if (typeof zonePalette !== "undefined") zonePalette.style.display = "none";
      zoneOverlayEnabled = false;
      currentZones = [];
      renderZoneOverlay();
      currentTool = null;
      selectedDesignation = null;
      digMenuOpen = false;
      plantMenuOpen = false;
      smoothMenuOpen = false;
      updateDesignationButtons();
    }
    stockPalette.querySelectorAll("[data-stock-preset]").forEach(b =>
      b.classList.toggle("active", b.dataset.stockPreset === key));
    if (key) setStockStatus("");
    updateToolCursor();
  }
  function setStockRepaint(id) {
    stockRepaintId = id;
    stockPreset = null;
    stockPalette.style.display = "none";
    zonePreset = null;
    if (typeof zonePalette !== "undefined") zonePalette.style.display = "none";
    zoneOverlayEnabled = false;
    currentZones = [];
    renderZoneOverlay();
    clearBuildPlacement(false);
    currentTool = null;
    selectedDesignation = null;
    digMenuOpen = false;
    plantMenuOpen = false;
    smoothMenuOpen = false;
    updateDesignationButtons();
    stockPalette.querySelectorAll("[data-stock-preset]").forEach(b => b.classList.remove("active"));
    updateToolCursor();
  }
  function toggleStockPalette() {
    const show = stockPalette.style.display === "none";
    stockPalette.style.display = show ? "block" : "none";
    if (show) {
      setStockPreset(stockPreset || "all");
    } else {
      setStockPreset(null);
    }
  }
  stockPalette.querySelectorAll("[data-stock-preset]").forEach(b =>
    b.addEventListener("click", event => { event.stopPropagation(); setStockPreset(b.dataset.stockPreset); focusPage(); }));
  stockPalette.querySelector("[data-stock-palette-close]").addEventListener("click", event => {
    event.stopPropagation();
    stockPalette.style.display = "none";
    setStockPreset(null);
    setActiveToolbar(null);
    focusPage();
  });

  // Create a stockpile over the dragged rectangle with the chosen category preset.
  async function createStockpileDrag(x1, y1, x2, y2) {
    if (!stockPreset) return;
    const a = imagePixelClamped(x1, y1);
    const b = imagePixelClamped(x2, y2);
    if (!a || !b) return;
    setStockStatus("Creating stockpile...");
    try {
      const url = `/stockpile?player=${encodeURIComponent(player)}&px=${a.x}&py=${a.y}` +
        `&px2=${b.x}&py2=${b.y}&w=${a.w}&h=${a.h}&preset=${encodeURIComponent(stockPreset)}`;
      const r = await fetch(url, { method: "POST", cache: "no-store" });
      const text = await r.text();
      let data = {};
      try { data = text ? JSON.parse(text) : {}; } catch (_) {}
      if (!r.ok) throw new Error(text.trim() || "stockpile request failed");
      setStockStatus("Stockpile created.");
      if (Number(data.id) >= 0) openStockpilePanel(Number(data.id));
    } catch (err) {
      setStockStatus(String(err.message || err || "Stockpile failed").replace(/^stockpile failed:\s*/i, ""), true);
    }
  }

  // --- Zone placement: pick a zone type, then drag a rectangle to create one ---
  const ZONE_TYPES = [
    ["Meeting Area", "meeting", 5, 10], ["Office", "office", 6, 9],
    ["Bedroom", "bedroom", 6, 7], ["Dormitory", "dormitory", 6, 12],
    ["Dining Hall", "dining", 6, 8], ["Barracks", "barracks", 6, 11],
    ["Pen/Pasture", "pen", 5, 6], ["Archery Range", "archery", 6, 10],
    ["Pit/Pond", "pond", 5, 7], ["Garbage Dump", "dump", 5, 5],
    ["Water Source", "water", 5, 2], ["Animal Training", "training", 5, 12],
    ["Dungeon", "dungeon", 6, 13], ["Tomb", "tomb", 6, 14],
    ["Fishing", "fishing", 5, 3], ["Gather Fruit", "gather", 5, 4],
    ["Sand", "sand", 5, 8], ["Clay", "clay", 5, 9]
  ];
  function zoneIconStyle(ix, iy) {
    return `background-position:-${ix * 32}px -${iy * 32}px`;
  }
  function zoneTypeButton(entry) {
    const [label, key, ix, iy] = entry;
    return `<button class="zone-type-btn" data-zone-type="${key}" title="${escapeHtml(label)}">
      <span class="zone-icon" style="${zoneIconStyle(ix, iy)}"></span>
      <span>${escapeHtml(label)}</span>
    </button>`;
  }
  const zonePalette = document.createElement("div");
  zonePalette.id = "zonePalette";
  zonePalette.style.display = "none";
  zonePalette.innerHTML = `
    <div class="zone-help">
      <p>Select / Edit: click a placed zone to edit it.</p>
      <p>Or pick a type below to paint a new zone.</p>
    </div>
    <button class="zone-select-btn" data-zone-select title="Click placed zones to edit them">&#128269; Select / Edit zone</button>
    <div class="zone-type-panel">
      <div class="zone-type-title">Click an icon to add a new zone.</div>
      <div class="zone-type-grid">
        ${ZONE_TYPES.map(zoneTypeButton).join("")}
      </div>
      <div class="zone-actions"><button class="stock-palette-close" data-zone-palette-close>Done</button></div>
    </div>
  `;
  document.body.appendChild(zonePalette);

  function setZonePreset(key) {
    zonePreset = key;
    if (key) { // leaving every other placement mode
      clearBuildPlacement(false);
      stockRepaintId = null;
      stockPreset = null;
      if (typeof stockPalette !== "undefined") stockPalette.style.display = "none";
      currentTool = null;
      selectedDesignation = null;
      digMenuOpen = false;
      plantMenuOpen = false;
      smoothMenuOpen = false;
      updateDesignationButtons();
    }
    zonePalette.querySelectorAll("[data-zone-type]").forEach(b =>
      b.classList.toggle("active", b.dataset.zoneType === key));
    // Select/Edit mode = no placement type chosen (key null) -> clicks inspect placed zones.
    const selBtn = zonePalette.querySelector("[data-zone-select]");
    if (selBtn) selBtn.classList.toggle("active", !key);
    updateToolCursor();
    renderZoneOverlay();
  }
  function toggleZonePalette() {
    const show = zonePalette.style.display === "none";
    zonePalette.style.display = show ? "block" : "none";
    zoneOverlayEnabled = show;
    if (show) {
      setZonePreset(null);   // start in Select/Edit mode so clicks can pick existing zones
      loadZones();
    } else {
      setZonePreset(null);
      setActiveToolbar(null);
      currentZones = [];
      renderZoneOverlay();
    }
  }
  zonePalette.querySelectorAll("[data-zone-type]").forEach(b =>
    b.addEventListener("click", event => { event.stopPropagation(); setZonePreset(b.dataset.zoneType); focusPage(); }));
  zonePalette.querySelector("[data-zone-select]").addEventListener("click", event => {
    event.stopPropagation(); setZonePreset(null); focusPage();   // Select/Edit: clicks pick placed zones
  });
  zonePalette.querySelector("[data-zone-palette-close]").addEventListener("click", event => {
    event.stopPropagation();
    zonePalette.style.display = "none";
    zoneOverlayEnabled = false;
    setZonePreset(null);
    currentZones = [];
    renderZoneOverlay();
    setActiveToolbar(null);
    focusPage();
  });
  setInterval(() => { if (zoneOverlayEnabled) loadZones(); }, 1000);

  // Create an activity zone over the dragged rectangle.
  async function createZoneDrag(x1, y1, x2, y2) {
    if (!zonePreset) return;
    const a = imagePixelClamped(x1, y1);
    const b = imagePixelClamped(x2, y2);
    if (!a || !b) return;
    try {
      const url = `/zone?player=${encodeURIComponent(player)}&px=${a.x}&py=${a.y}` +
        `&px2=${b.x}&py2=${b.y}&w=${a.w}&h=${a.h}&zone=${encodeURIComponent(zonePreset)}`;
      const r = await fetch(url, { method: "POST", cache: "no-store" });
      if (r.ok) loadZones();
    } catch (_) {}
  }
  async function repaintStockpileDrag(x1, y1, x2, y2) {
    if (!stockRepaintId) return;
    const id = stockRepaintId;
    const a = imagePixelClamped(x1, y1);
    const b = imagePixelClamped(x2, y2);
    if (!a || !b) return;
    try {
      const url = `/stockpile-repaint?player=${encodeURIComponent(player)}&id=${id}&px=${a.x}&py=${a.y}` +
        `&px2=${b.x}&py2=${b.y}&w=${a.w}&h=${a.h}`;
      const r = await fetch(url, { method: "POST", cache: "no-store" });
      if (r.ok) {
        const data = await r.json();
        setStockRepaint(null);
        openStockpilePanel(Number(data.id) || id);
      }
    } catch (_) {}
  }

  addEventListener("keydown", event => {
    // Let text inputs (stockpile rename, search) receive keys without panning the map.
    const tag = event.target && event.target.tagName;
    if (tag === "INPUT" || tag === "TEXTAREA") {
      if (event.key === "Escape") event.target.blur();
      return;
    }
    focusPage();
    if (event.key === "Escape") {
      let handledEscape = false;
      if (stockRepaintId) {
        setStockRepaint(null);
        handledEscape = true;
      } else if (clientPanel.classList.contains("visible") && clientPanel.classList.contains("build-panel")) {
        // Build menu open: ONE Escape closes the whole window (same as its X button). Without this,
        // the auto-selected building's detail panel eats the first Escape, so it would take two
        // presses to leave the build menu.
        clearBuildPlacement(false);
        closeClientPanel();
        setActiveToolbar(null);
        handledEscape = true;
      } else if (selectedBuild) {
        // Build placement active with the menu already closed (placing on the map) -> cancel it.
        clearBuildPlacement(true);
        handledEscape = true;
      } else if (selection.classList.contains("visible")) {
        closeSelection();
        handledEscape = true;
      } else if (clientPanel.classList.contains("visible")) {
        closeClientPanel();
        handledEscape = true;
      } else if (typeof zonePalette !== "undefined" && zonePalette.style.display !== "none") {
        // Zone menu open -> Escape closes it. Checked on palette VISIBILITY (not zonePreset)
        // because Select/Edit mode keeps the palette open with zonePreset === null, and that mode
        // previously couldn't be dismissed with Escape at all.
        zonePalette.style.display = "none";
        zoneOverlayEnabled = false;
        setZonePreset(null);
        currentZones = [];
        renderZoneOverlay();
        setActiveToolbar(null);
        handledEscape = true;
      } else if (stockPreset) {
        stockPalette.style.display = "none";
        setStockPreset(null);
        setActiveToolbar(null);
        handledEscape = true;
      } else if (digMenuOpen || plantMenuOpen || smoothMenuOpen || selectedDesignation || currentTool) {
        digMenuOpen = false;
        plantMenuOpen = false;
        smoothMenuOpen = false;
        selectedDesignation = null;
        currentTool = null;
        updateDesignationButtons();
        handledEscape = true;
      }
      if (handledEscape) {
        event.preventDefault();
        return;
      }
    }
    let handled = true;
    switch (event.key) {
      case "ArrowLeft": case "a": case "A": case "h": case "H":
        queueMove(-step, 0, 0); break;
      case "ArrowRight": case "d": case "D": case "l": case "L":
        queueMove(step, 0, 0); break;
      case "ArrowUp": case "w": case "W": case "k": case "K":
        queueMove(0, -step, 0); break;
      case "ArrowDown": case "s": case "S": case "j": case "J":
        queueMove(0, step, 0); break;
      case "PageUp": case ">":
        queueMove(0, 0, zstep); break;
      case "PageDown": case "<":
        queueMove(0, 0, -zstep); break;
      case "[":
        sendZoom("in"); break;
      case "]":
        sendZoom("out"); break;
      case "Home": case "r": case "R":
        resetToHost(); break;
      default:
        handled = false;
    }
    if (handled) event.preventDefault();
  });

  addEventListener("wheel", event => {
    if (event.target.closest("#clientPanel.visible, #selection.visible, #alertPopup"))
      return;
    focusPage();
    event.preventDefault();
    queueMove(0, 0, event.deltaY < 0 ? zstep : -zstep);
  }, { passive: false });

  // Camera panning is keyboard-only (WASD / arrows / PgUp-PgDn), like DF. A mouse drag
  // is reserved for rectangle designation when a dig/designation tool is active; with no
  // tool active a plain click inspects the tile.
  const digSelect = document.getElementById("digSelect");
  let pdown = false;
  let downX = 0;
  let downY = 0;
  let dragAnchor = null; // image-pixel anchor of an in-progress placement drag
  // Instant browser-side preview applies to rectangle designations / zones / stockpiles, NOT to
  // building placement (which keeps the server-rendered footprint preview centered on the cursor).
  function instantDrag() { return instantDesignate && !selectedBuild; }
  function updateDigSelect(curX, curY) {
    digSelect.style.left = Math.min(downX, curX) + "px";
    digSelect.style.top = Math.min(downY, curY) + "px";
    digSelect.style.width = Math.abs(curX - downX) + "px";
    digSelect.style.height = Math.abs(curY - downY) + "px";
  }
  view.addEventListener("pointerdown", event => {
    focusPage();
    if (event.button !== 0) return;
    pdown = true;
    downX = event.clientX;
    downY = event.clientY;
    if (currentTool || stockPreset || stockRepaintId || selectedBuild || zonePreset) {
      // The backend paints the real, tile-snapped DF selection rectangle into the frame; in
      // instant mode we draw it browser-side instead (no per-move round-trip) and skip the send.
      dragAnchor = imagePixelClamped(event.clientX, event.clientY);
      if (dragAnchor) {
        if (instantDrag()) {
          dragPreview = { ax: dragAnchor.x, ay: dragAnchor.y, bx: dragAnchor.x, by: dragAnchor.y };
          renderZoneOverlay();
        } else {
          sendPlacementUi(dragAnchor.x, dragAnchor.y, dragAnchor.w, dragAnchor.h,
                          true, dragAnchor.x, dragAnchor.y, true);
        }
      }
    }
    view.setPointerCapture(event.pointerId);
  });
  view.addEventListener("pointermove", event => {
    if (!pdown || (!currentTool && !stockPreset && !stockRepaintId && !selectedBuild && !zonePreset)) return;
    if (dragAnchor) {
      const cur = imagePixelClamped(event.clientX, event.clientY);
      if (!cur) return;
      if (instantDrag()) {
        dragPreview = { ax: dragAnchor.x, ay: dragAnchor.y, bx: cur.x, by: cur.y };
        renderZoneOverlay();
      } else {
        sendPlacementUi(cur.x, cur.y, cur.w, cur.h, true, dragAnchor.x, dragAnchor.y);
      }
    }
  });
  view.addEventListener("pointerup", event => {
    if (!pdown) return;
    pdown = false;
    digSelect.style.display = "none";
    if (dragAnchor) {
      const cur = imagePixelClamped(event.clientX, event.clientY);
      if (!instantDrag() && cur) sendPlacementUi(cur.x, cur.y, cur.w, cur.h, false, 0, 0, true);
      dragAnchor = null;
    }
    try { view.releasePointerCapture(event.pointerId); } catch (_) {}
    const clickDistance = Math.hypot(event.clientX - downX, event.clientY - downY);
    if (selectedBuild) {
      placeBuildDrag(downX, downY, event.clientX, event.clientY);
    } else if (zonePreset) {
      createZoneDrag(downX, downY, event.clientX, event.clientY);
    } else if (stockPreset) {
      createStockpileDrag(downX, downY, event.clientX, event.clientY);
    } else if (stockRepaintId) {
      repaintStockpileDrag(downX, downY, event.clientX, event.clientY);
    } else if (currentTool) {
      designateDrag(downX, downY, event.clientX, event.clientY);
    } else if (clickDistance < 8) {
      inspectClick(event);
    }
    // Hold the instant-mode preview briefly so it doesn't flash out before the server frame
    // (now carrying the committed designation) streams back, then clear it.
    if (instantDrag() && dragPreview) {
      const held = dragPreview;
      setTimeout(() => { if (dragPreview === held) { dragPreview = null; renderZoneOverlay(); } }, 380);
    }
  });
  view.addEventListener("pointercancel", () => {
    pdown = false;
    digSelect.style.display = "none";
    if (dragAnchor) {
      if (!instantDrag()) sendPlacementUi(-1, -1, 0, 0, false, 0, 0, true);
      dragAnchor = null;
    }
    if (dragPreview) { dragPreview = null; renderZoneOverlay(); }
  });

  // --- DF-style hover tooltip: shows what's on the tile under the cursor ---
  const hoverInfo = document.getElementById("hoverInfo");
  let hoverPx = -1;
  let hoverPy = -1;
  let hoverAt = 0;
  let hoverBusy = false;
  function renderHover(d) {
    const lines = Array.isArray(d.lines) ? d.lines : [];
    let html = lines.map(l => `<div class="hv-line">${escapeHtml(l)}</div>`).join("");
    if (d.material) html += `<div class="hv-mat">${escapeHtml(d.material)}</div>`;
    if (!html) { hoverInfo.style.display = "none"; return; }
    hoverInfo.innerHTML = html;
    hoverInfo.style.display = "block";
  }
  async function fetchHover(px, py, w, h) {
    if (hoverBusy) return;
    hoverBusy = true;
    try {
      const url = `/hover?player=${encodeURIComponent(player)}&px=${px}&py=${py}&w=${w}&h=${h}`;
      const r = await fetch(url, { cache: "no-store" });
      if (r.ok) renderHover(await r.json());
    } catch (_) {
    } finally {
      hoverBusy = false;
    }
  }
  view.addEventListener("pointermove", event => {
    if (pdown) return; // suppressed while click-dragging a designation
    const pixel = imagePixelFromEvent(event);
    if (!pixel) { hoverInfo.style.display = "none"; return; }
    if (placementActive() && !instantDrag())
      sendPlacementUi(pixel.x, pixel.y, pixel.w, pixel.h, false, 0, 0);
    const now = performance.now();
    if (now - hoverAt < 80) return;
    if (Math.abs(pixel.x - hoverPx) < 3 && Math.abs(pixel.y - hoverPy) < 3) return;
    hoverAt = now;
    hoverPx = pixel.x;
    hoverPy = pixel.y;
    fetchHover(pixel.x, pixel.y, pixel.w, pixel.h);
  });
  view.addEventListener("pointerleave", () => {
    hoverInfo.style.display = "none";
    if (!pdown && placementActive()) sendPlacementUi(-1, -1, 0, 0, false, 0, 0, true);
  });
  view.addEventListener("pointerdown", () => { hoverInfo.style.display = "none"; });
