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

  const params = new URLSearchParams(location.search);
  const stored = localStorage.getItem("dfcapture.player");
  const fresh = (crypto.randomUUID ? crypto.randomUUID() :
    `p-${Date.now().toString(36)}-${Math.floor(Math.random() * 1e9).toString(36)}`);
  const player = params.get("player") || stored || fresh;
  localStorage.setItem("dfcapture.player", player);

  const view = document.getElementById("view");
  const zoneOverlay = document.getElementById("zoneOverlay");
  const selection = document.getElementById("selection");
  const clientPanel = document.getElementById("clientPanel");
  const tileFlash = document.getElementById("tileFlash");
  const hudEls = {
    fortName: document.getElementById("fortName"),
    siteName: document.getElementById("siteName"),
    rankName: document.getElementById("rankName"),
    population: document.getElementById("population"),
    food: document.getElementById("food"),
    drink: document.getElementById("drink"),
    moon: document.getElementById("moon"),
    dateDay: document.getElementById("dateDay"),
    dateMonth: document.getElementById("dateMonth"),
    dateSeason: document.getElementById("dateSeason"),
    dateYear: document.getElementById("dateYear"),
    minimap: document.getElementById("minimapGrid"),
    elevation: document.getElementById("elevation")
  };
  const alertStack = document.getElementById("alertStack");
  const alertPopup = document.getElementById("alertPopup");
  let currentHud = null;
  let notificationState = { alerts: [], recent: [] };
  let currentZones = [];
  let zoneSnapshotCamera = null;
  let zoneSnapshotViewport = null;
  let zoneOverlayEnabled = false;
  // When true, dig/zone/stockpile drag selections are previewed instantly in the browser
  // (no per-mousemove server round-trip). Persisted per-browser. Default off = DF's
  // server-rendered selection grid (the "aesthetic" path, with the slight drag latency).
  let instantDesignate = false;
  try { instantDesignate = localStorage.getItem("dfplex.instantDesignate") === "1"; } catch (_) {}
  // Live drag selection rectangle in viewport-tile space, or null. Drawn on #zoneOverlay.
  let dragPreview = null;
  // Presence follow-camera state (used by the presence block further down): the player we're
  // following, and the last camera we applied for them so we only POST /camera when it changes.
  let followTarget = null;
  let followedCamKey = "";

  // --- Predictive camera panning ----------------------------------------------------------
  // The map is server-rendered, so a pan key normally waits a full round-trip before the frame
  // moves. To hide that, translate the CURRENT frame immediately in the pan direction, then let
  // it reconcile: every frame carries the camera it was rendered at (X-DFCapture-Camera header),
  // so we shift the displayed frame by (frameCam - predictedCam) tiles. As real frames catch up,
  // the shift decays to 0 with no snap -- the shifted old frame and the caught-up new frame show
  // identical content at the same place. Off (or offset 0) => behaves exactly as before.
  let predictivePan = true;
  try { const v = localStorage.getItem("dfplex.predictivePan"); predictivePan = (v === null) ? true : (v === "1"); } catch (_) {}
  // Native unit portraits/body sprites touch DF's unit texture fields and have been crash-prone
  // when the host opens related native unit lists. Default off for stability; users can opt in.
  let unitImagesEnabled = false;
  try { unitImagesEnabled = localStorage.getItem("dfplex.unitImages") === "1"; } catch (_) {}
  let predictedCam = null;          // where the camera "should" be from local input {x,y,z}
  let frameCam = null;              // camera the currently shown frame was rendered at {x,y,z}
  let frameGrid = null;             // renderer origin/zoom for the currently shown frame
  let prevFrameCam = null;
  let panStalled = 0;               // consecutive frames where frameCam didn't change
  let lastPanInputAt = 0;
  const panOffset = { x: 0, y: 0 }; // px the #view is currently translated by
  const PAN_CAP_TILES = 16;         // bound the lead so a desync can't slide the frame far off

  // The view's true (untransformed) client rect: subtract the predictive translate so all the
  // tile math stays locked to the real camera position regardless of the visual shift.
  function viewClientRect() {
    const r = view.getBoundingClientRect();
    return { left: r.left - panOffset.x, top: r.top - panOffset.y, width: r.width, height: r.height };
  }
  function setPanOffset(x, y) {
    if (x === panOffset.x && y === panOffset.y) return;
    panOffset.x = x; panOffset.y = y;
    view.style.transform = (x || y) ? `translate3d(${x}px, ${y}px, 0)` : "";
  }
  function clearPanPrediction() { setPanOffset(0, 0); }
  function resetPanPrediction() { predictedCam = null; prevFrameCam = null; panStalled = 0; clearPanPrediction(); }
  function clampPredicted() {
    if (!predictedCam) return;
    const map = currentHud && currentHud.map, vp = currentHud && currentHud.viewport;
    if (map && vp) {
      predictedCam.x = Math.max(0, Math.min(predictedCam.x, Math.max(0, (Number(map.w) || 0) - (Number(vp.w) || 0))));
      predictedCam.y = Math.max(0, Math.min(predictedCam.y, Math.max(0, (Number(map.h) || 0) - (Number(vp.h) || 0))));
    }
  }
  function applyPanPrediction() {
    if (!predictivePan || !predictedCam || !frameCam || frameCam.z !== predictedCam.z) { clearPanPrediction(); return; }
    const vp = currentHud && currentHud.viewport;
    const nw = view.naturalWidth, nh = view.naturalHeight;
    if (!vp || !nw || !nh) { clearPanPrediction(); return; }
    const grid = captureTileGrid();
    if (!grid) { clearPanPrediction(); return; }
    const tileW = (grid.naturalX(1) - grid.naturalX(0)) * grid.rendered.scale;
    const tileH = (grid.naturalY(1) - grid.naturalY(0)) * grid.rendered.scale;
    let dxT = predictedCam.x - frameCam.x;
    let dyT = predictedCam.y - frameCam.y;
    dxT = Math.max(-PAN_CAP_TILES, Math.min(PAN_CAP_TILES, dxT));
    dyT = Math.max(-PAN_CAP_TILES, Math.min(PAN_CAP_TILES, dyT));
    setPanOffset(-dxT * tileW, -dyT * tileH);
  }
  // Called the instant a pan key is pressed: advance the predicted camera and shift immediately.
  function notePanInput(dx, dy, dz) {
    if (!predictedCam) return;       // wait for the first frame to seed predictedCam
    predictedCam.x += dx; predictedCam.y += dy; predictedCam.z += dz;
    clampPredicted();
    lastPanInputAt = performance.now();
    applyPanPrediction();
  }
  function parseFrameCamera(headerVal) {
    if (!headerVal) return null;
    const parts = String(headerVal).split(",");
    if (parts.length < 3) return null;
    const x = Number(parts[0]), y = Number(parts[1]), z = Number(parts[2]);
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) return null;
    return { x, y, z };
  }
  function parseFrameGrid(headerVal) {
    if (!headerVal) return null;
    const parts = String(headerVal).split(",").map(Number);
    if (parts.length < 5 || parts.some(v => !Number.isFinite(v))) return null;
    const [originX, originY, zoomFactor, w, h] = parts;
    if (zoomFactor <= 0 || w <= 0 || h <= 0) return null;
    return { originX, originY, zoomFactor, w: Math.floor(w), h: Math.floor(h) };
  }
  // Reconcile predicted vs the just-arrived frame camera. Adopt truth on a teleport (large jump)
  // or when we're idle and the server has clearly stalled at a different spot (a dropped move);
  // otherwise let the natural per-frame decay handle normal catch-up.
  function reconcilePredicted(fc) {
    if (!fc) return;
    if (!predictedCam) { predictedCam = { x: fc.x, y: fc.y, z: fc.z }; prevFrameCam = { x: fc.x, y: fc.y, z: fc.z }; return; }
    const dx = predictedCam.x - fc.x, dy = predictedCam.y - fc.y;
    const idle = (performance.now() - lastPanInputAt) > 250;
    const teleport = (predictedCam.z !== fc.z && idle)
                     || Math.abs(dx) > 3 * PAN_CAP_TILES || Math.abs(dy) > 3 * PAN_CAP_TILES;
    if (prevFrameCam && fc.x === prevFrameCam.x && fc.y === prevFrameCam.y && fc.z === prevFrameCam.z) panStalled++;
    else panStalled = 0;
    if (teleport || (idle && panStalled >= 2 && (dx !== 0 || dy !== 0 || predictedCam.z !== fc.z))) {
      predictedCam = { x: fc.x, y: fc.y, z: fc.z };
    }
    prevFrameCam = { x: fc.x, y: fc.y, z: fc.z };
  }
  let pinnedAlertKey = null;
  let notificationFilterType = null;
  let lastNotificationPanelSignature = "";
  let selectedUnitData = null;
  let activeUnitTab = "Overview";
  let activeUnitDetailTab = null;
  let activeInfoPanel = null;
  let activeInfoSection = null;
  let activeInfoDetail = null;
  let activeStockCategory = "";
  let activeWorkshopTab = "tasks";
  let workshopAddMode = false;
  let workshopOrderAddMode = false;
  let workshopRenameMode = false;
  let farmRenameMode = false;
  let zoneRenameMode = false;
  let farmSelectedSeason = 0;
  let workshopStatusMsg = "";
  let workshopStatusIsError = false;
  function focusPage() {
    try { view.focus({ preventScroll: true }); } catch (_) {}
  }
  setTimeout(focusPage, 0);

  const frameIntervalMs = 125;
  let currentFrameUrl = "";
  let frameSeq = 0;
  const ZONE_SHEET_URL = "/asset/activity_zones.png";
  const zoneSheet = new Image();
  zoneSheet.onload = () => renderZoneOverlay();
  zoneSheet.src = ZONE_SHEET_URL;
  const cursorSheet = new Image();
  cursorSheet.onload = () => renderZoneOverlay();
  cursorSheet.src = "/asset/cursors.png";
  view.addEventListener("load", () => renderZoneOverlay());

  function scheduleFrame(delay = frameIntervalMs) {
    setTimeout(loadFrame, delay);
  }

  async function loadFrame() {
    try {
      const response = await fetch(`/frame.jpg?player=${encodeURIComponent(player)}&ui=0&t=${Date.now()}-${frameSeq++}`, {
        cache: "no-store"
      });
      if (!response.ok) throw new Error("frame failed");
      const fc = parseFrameCamera(response.headers.get("X-DFCapture-Camera"));
      const fg = parseFrameGrid(response.headers.get("X-DFCapture-Grid"));
      const blob = await response.blob();
      const nextUrl = URL.createObjectURL(blob);
      const oldUrl = currentFrameUrl;
      currentFrameUrl = nextUrl;
      frameGrid = fg;
      view.src = nextUrl;
      if (oldUrl) setTimeout(() => URL.revokeObjectURL(oldUrl), 1000);
      if (fc) { frameCam = fc; reconcilePredicted(fc); applyPanPrediction(); }
      scheduleFrame();
    } catch (_) {
      scheduleFrame(500);
    }
  }
  const step = 10;
  const zstep = 1;
  let queued = { dx: 0, dy: 0, dz: 0 };
  let sending = false;

  function queueMove(dx, dy, dz) {
    followTarget = null;        // any manual pan releases follow-camera
    if (typeof window.syncMinimapControls === "function") window.syncMinimapControls();
    notePanInput(dx, dy, dz);   // instant predictive shift before the server round-trip
    queued.dx += dx;
    queued.dy += dy;
    queued.dz += dz;
    if (sending) return;
    sending = true;
    requestAnimationFrame(flushMove);
  }

  async function flushMove() {
    const move = queued;
    queued = { dx: 0, dy: 0, dz: 0 };
    const url = `/camera?player=${encodeURIComponent(player)}&dx=${move.dx}&dy=${move.dy}&dz=${move.dz}`;
    try { await fetch(url, { method: "POST", cache: "no-store" }); } catch (_) {}
    loadHud();
    if (zoneOverlayEnabled) loadZones();
    sending = false;
    if (queued.dx || queued.dy || queued.dz) {
      sending = true;
      requestAnimationFrame(flushMove);
    }
  }

  // Real per-player zoom (changes how much of the world is visible, like DF's [ ]).
  // The plugin re-renders this player's next frame at their own viewport zoom factor.
  let zoomBusy = false;
  function sendZoom(dir) {
    if (zoomBusy) return;             // coalesce rapid presses
    zoomBusy = true;
    fetch(`/zoom?player=${encodeURIComponent(player)}&dir=${dir}`, { method: "POST", cache: "no-store" })
      .catch(() => {})
      .finally(() => {
        zoomBusy = false;
        loadHud();
        if (zoneOverlayEnabled) loadZones();
      });
  }

  async function resetToHost() {
    resetPanPrediction();
    try {
      await fetch(`/reset?player=${encodeURIComponent(player)}`, { method: "POST", cache: "no-store" });
    } catch (_) {}
    loadHud();
    if (zoneOverlayEnabled) loadZones();
  }

  function isTextEditingTarget(target) {
    const tag = target && target.tagName;
    return tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT" || !!target?.isContentEditable;
  }

  function handleCameraKey(event) {
    if (!event || isTextEditingTarget(event.target)) return false;
    if (event.altKey || event.metaKey || event.ctrlKey) return false;
    switch (event.key) {
      case "ArrowLeft": case "a": case "A": case "h": case "H":
        queueMove(-step, 0, 0); return true;
      case "ArrowRight": case "d": case "D": case "l": case "L":
        queueMove(step, 0, 0); return true;
      case "ArrowUp": case "w": case "W": case "k": case "K":
        queueMove(0, -step, 0); return true;
      case "ArrowDown": case "s": case "S": case "j": case "J":
        queueMove(0, step, 0); return true;
      case "PageUp": case ">": case "e": case "E":
        queueMove(0, 0, zstep); return true;
      case "PageDown": case "<": case "q": case "Q":
        queueMove(0, 0, -zstep); return true;
      case "[": case "=": case "+":
        sendZoom("in"); return true;
      case "]": case "-": case "_":
        sendZoom("out"); return true;
      case "Home": case "r": case "R":
        resetToHost(); return true;
      default:
        return false;
    }
  }

  if (!window.__dfcaptureCoreCameraControlsBound) {
    window.__dfcaptureCoreCameraControlsBound = true;
    addEventListener("keydown", event => {
      if (handleCameraKey(event)) {
        focusPage();
        event.__dfcaptureCameraHandled = true;
        event.preventDefault();
        event.stopImmediatePropagation();
      }
    }, { capture: true });
    addEventListener("wheel", event => {
      if (event.target.closest("#clientPanel.visible, #selection.visible, #alertPopup"))
        return;
      focusPage();
      event.preventDefault();
      event.stopImmediatePropagation();
      if ((event.ctrlKey || event.metaKey) && window.DFCaptureUIScale) {
        window.DFCaptureUIScale.adjust(event.deltaY < 0 ? 1 : -1);
        return;
      }
      if (event.shiftKey) {
        sendZoom(event.deltaY < 0 ? "in" : "out");
      } else {
        queueMove(0, 0, event.deltaY < 0 ? zstep : -zstep);
      }
    }, { passive: false, capture: true });
  }

  // --- Presence: named cursors + follow-camera (world-coordinate, drawn on #zoneOverlay) ------
  // Pure browser-to-browser relay via /presence; touches no DF state. Each client posts its cursor
  // world-tile + camera; every client polls the others and draws their cursors on this player's own
  // view using the same transforms the zone overlay uses, interpolated for smoothness.
  const PRESENCE_POLL_MS = 120;
  const PRESENCE_POST_MS = 90;
  const presencePeers = new Map();   // id -> {has,x,y,z,hasCam,cx,cy,cz,name,color,fromX,fromY,fromT}
  let myTile = null;
  let lastPresencePostAt = 0;
  let rosterEl = null;
  const activePings = new Map();      // ping id -> {x,y,z,name,color,startT}
  let lastPingId = 0;

  // World tile for an image-pixel point (used to convert a drag anchor into world coords).
  function worldTileFromImagePx(px) {
    const cam = currentHud?.camera, vp = currentHud?.viewport;
    if (!px || !cam || !vp || !px.w || !px.h) return null;
    const vw = Math.max(1, Number(vp.w) || 1);
    const vh = Math.max(1, Number(vp.h) || 1);
    return {
      x: Number(cam.x) + Math.floor(px.x * vw / px.w),
      y: Number(cam.y) + Math.floor(px.y * vh / px.h),
      z: Number(cam.z)
    };
  }

  // This player's tool + in-progress designation drag, read from the (shared, global-lexical)
  // placement.js state. try/catch so a not-yet-loaded var can never break a presence post.
  function readLocalState() {
    let tool = "", drag = null;
    try {
      tool = (currentTool ? String(currentTool) : "")
           || (selectedBuild ? "build" : "")
           || (zonePreset ? "zone" : "")
           || ((stockPreset || stockRepaintId) ? "stockpile" : "")
           || (selectedDesignation ? String(selectedDesignation) : "");
      if (pdown && dragAnchor && tool) {
        const a = worldTileFromImagePx(dragAnchor);
        if (a && myTile) drag = { ax: a.x, ay: a.y, bx: myTile.x, by: myTile.y, az: myTile.z };
      }
    } catch (_) {}
    return { tool, drag };
  }

  function readLocalFocus() {
    try {
      if (selectedUnitData) return "a unit";
      if (activeInfoPanel) return String(activeInfoPanel);
      if (selection && selection.classList.contains("visible")) return "inspecting";
    } catch (_) {}
    return "";
  }

  function playerColor(id) {
    let h = 0;
    const s = String(id);
    for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) >>> 0;
    return `hsl(${h % 360} 78% 60%)`;
  }
  const myColor = playerColor(player);

  function worldTileFromEvent(event) {
    const cam = currentHud?.camera, vp = currentHud?.viewport;
    const nw = view.naturalWidth, nh = view.naturalHeight;
    if (!cam || !vp || !nw || !nh) return null;
    const px = imagePixelFromEvent(event);
    if (!px) return null;
    return worldTileFromImagePx(px);
  }

  function postPresence(force) {
    const now = performance.now();
    if (!force && now - lastPresencePostAt < PRESENCE_POST_MS) return;
    lastPresencePostAt = now;
    const cam = currentHud?.camera, vp = currentHud?.viewport;
    const local = readLocalState();
    const q = new URLSearchParams();
    q.set("player", player);
    q.set("name", window.DFCaptureSession?.displayName() || player);
    q.set("color", myColor);
    if (myTile) { q.set("has", "1"); q.set("x", myTile.x); q.set("y", myTile.y); q.set("z", myTile.z); }
    else q.set("has", "0");
    if (cam) { q.set("cx", cam.x); q.set("cy", cam.y); q.set("cz", cam.z); }
    if (vp) { q.set("vw", vp.w); q.set("vh", vp.h); }
    if (local.tool) q.set("tool", local.tool);
    const focus = readLocalFocus();
    if (focus) q.set("focus", focus);
    if (local.drag) {
      q.set("hasdrag", "1");
      q.set("dax", local.drag.ax); q.set("day", local.drag.ay);
      q.set("dbx", local.drag.bx); q.set("dby", local.drag.by); q.set("daz", local.drag.az);
    }
    fetch("/presence?" + q.toString(), { method: "POST", cache: "no-store" }).catch(() => {});
  }

  function followPeerCamera(peer) {
    if (!peer || !peer.hasCam) return;
    const key = `${peer.cx},${peer.cy},${peer.cz}`;
    if (key === followedCamKey) return;
    followedCamKey = key;
    fetch(`/camera?player=${encodeURIComponent(player)}&x=${peer.cx}&y=${peer.cy}&z=${peer.cz}`, { method: "POST", cache: "no-store" })
      .then(() => { resetPanPrediction(); if (typeof loadHud === "function") loadHud(); if (zoneOverlayEnabled) loadZones(); })
      .catch(() => {});
  }

  async function pollPresence() {
    try {
      const r = await fetch(`/presence?player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
      if (!r.ok) return;
      const data = await r.json();
      const now = performance.now();
      const seen = new Set();
      for (const p of (data.peers || [])) {
        if (!p || p.player === player) continue;
        seen.add(p.player);
        const prev = presencePeers.get(p.player);
        const z = Number(p.z) || 0, x = Number(p.x) || 0, y = Number(p.y) || 0;
        const peer = {
          id: p.player, has: !!p.has, x, y, z,
          hasCam: !!p.hasCam, cx: Number(p.cx) || 0, cy: Number(p.cy) || 0, cz: Number(p.cz) || 0,
          vw: Number(p.vw) || 0, vh: Number(p.vh) || 0,
          tool: p.tool || "", focus: p.focus || "",
          hasDrag: !!p.hasDrag, dax: Number(p.dax) || 0, day: Number(p.day) || 0,
          dbx: Number(p.dbx) || 0, dby: Number(p.dby) || 0, daz: Number(p.daz) || 0,
          name: p.name || p.player, color: p.color || playerColor(p.player),
          fromX: (prev && prev.has && prev.z === z) ? prev.x : x,
          fromY: (prev && prev.has && prev.z === z) ? prev.y : y,
          fromT: now
        };
        presencePeers.set(p.player, peer);
        if (followTarget === p.player) followPeerCamera(peer);
      }
      for (const k of [...presencePeers.keys()]) if (!seen.has(k)) presencePeers.delete(k);
      for (const ping of (data.pings || [])) {
        const id = Number(ping.id) || 0;
        if (id > lastPingId && !activePings.has(id)) {
          activePings.set(id, { x: Number(ping.x) || 0, y: Number(ping.y) || 0, z: Number(ping.z) || 0,
                                name: ping.name || "", color: ping.color || "#ffdf4d", startT: now });
        }
        if (id > lastPingId) lastPingId = id;
      }
      updateRoster();
      if (typeof window.syncPresenceElevationMarkers === "function")
        window.syncPresenceElevationMarkers();
    } catch (_) {}
  }

  function roundRectPath(ctx, x, y, w, h, r) {
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + w, y, x + w, y + h, r);
    ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r);
    ctx.arcTo(x, y, x + w, y, r);
    ctx.closePath();
  }

  function drawPeerCursor(ctx, x, y, color, name, tool, dz) {
    ctx.save();
    ctx.globalAlpha = dz ? 0.45 : 1;      // fade off-elevation "ghost" cursors
    ctx.lineJoin = "round";
    ctx.beginPath();
    ctx.moveTo(x, y);
    ctx.lineTo(x + 13, y + 4.5);
    ctx.lineTo(x + 5.5, y + 6.5);
    ctx.lineTo(x + 4.5, y + 14);
    ctx.closePath();
    ctx.fillStyle = color;
    ctx.strokeStyle = "rgba(0,0,0,0.78)";
    ctx.lineWidth = 1.5;
    ctx.fill();
    ctx.stroke();
    const badge = dz ? (dz > 0 ? " ▲" + dz : " ▼" + (-dz)) : "";
    const label = String(name).slice(0, 18) + (tool ? " · " + tool : "") + badge;
    ctx.globalAlpha = dz ? 0.75 : 1;
    ctx.font = "600 12px system-ui, -apple-system, sans-serif";
    ctx.textBaseline = "middle";
    const tw = ctx.measureText(label).width;
    const px = x + 14, py = y + 1;
    ctx.fillStyle = "rgba(0,0,0,0.62)";
    roundRectPath(ctx, px, py, tw + 10, 17, 4);
    ctx.fill();
    ctx.fillStyle = color;
    ctx.fillText(label, px + 5, py + 9.5);
    ctx.restore();
  }

  // A peer's in-progress designation rectangle, in their color.
  function drawPeerDrag(ctx, p, cam) {
    if (!p.hasDrag || Number(p.daz) !== Number(cam.z)) return;
    const a = screenRectForMapTile({ x: Math.min(p.dax, p.dbx), y: Math.min(p.day, p.dby), z: cam.z });
    const b = screenRectForMapTile({ x: Math.max(p.dax, p.dbx), y: Math.max(p.day, p.dby), z: cam.z });
    if (!a || !b) return;
    const x = a.left, y = a.top, w = (b.left + b.width) - a.left, h = (b.top + b.height) - a.top;
    if (w <= 0 || h <= 0) return;
    ctx.save();
    ctx.globalAlpha = 0.14;
    ctx.fillStyle = p.color;
    ctx.fillRect(x + 1, y + 1, w - 2, h - 2);
    ctx.globalAlpha = 0.9;
    ctx.strokeStyle = p.color;
    ctx.lineWidth = 2;
    ctx.setLineDash([5, 3]);
    ctx.strokeRect(x + 1, y + 1, w - 2, h - 2);
    ctx.restore();
  }

  function drawPresence(ctx) {
    if (presencePeers.size === 0) return;
    const cam = currentHud?.camera;
    if (!cam) return;
    const now = performance.now();
    for (const p of presencePeers.values()) {
      drawPeerDrag(ctx, p, cam);           // their live designation rectangle
      if (!p.has) continue;
      const onZ = Number(p.z) === Number(cam.z);
      const a = Math.max(0, Math.min(1, (now - p.fromT) / PRESENCE_POLL_MS));
      const wx = p.fromX + (p.x - p.fromX) * a;
      const wy = p.fromY + (p.y - p.fromY) * a;
      const rect = screenRectForMapTile({ x: wx, y: wy, z: cam.z });  // positioned on our z-plane
      if (!rect) continue;
      const dz = Number(p.z) - Number(cam.z);
      drawPeerCursor(ctx, rect.left, rect.top, p.color, p.name, onZ ? p.tool : "", onZ ? 0 : dz);
    }
  }

  // "Look here" pings: expanding rings + name, fading over PING_DURATION_MS.
  const PING_DURATION_MS = 1400;
  function drawPings(ctx) {
    if (activePings.size === 0) return;
    const cam = currentHud?.camera;
    if (!cam) { activePings.clear(); return; }
    const now = performance.now();
    for (const [id, ping] of activePings) {
      const t = (now - ping.startT) / PING_DURATION_MS;
      if (t >= 1) { activePings.delete(id); continue; }
      if (Number(ping.z) !== Number(cam.z)) continue;
      const rect = screenRectForMapTile({ x: ping.x, y: ping.y, z: cam.z });
      if (!rect) continue;
      const cx = rect.left + rect.width / 2, cy = rect.top + rect.height / 2, maxR = 42;
      ctx.save();
      for (let k = 0; k < 2; k++) {
        const tt = t - k * 0.22;
        if (tt <= 0 || tt >= 1) continue;
        ctx.globalAlpha = (1 - tt) * 0.85;
        ctx.strokeStyle = ping.color;
        ctx.lineWidth = 3;
        ctx.beginPath();
        ctx.arc(cx, cy, 6 + tt * maxR, 0, Math.PI * 2);
        ctx.stroke();
      }
      ctx.globalAlpha = Math.max(0, 1 - t);
      ctx.fillStyle = ping.color;
      ctx.beginPath(); ctx.arc(cx, cy, 4, 0, Math.PI * 2); ctx.fill();
      if (ping.name) {
        ctx.font = "600 12px system-ui, sans-serif";
        ctx.textAlign = "center";
        const w = ctx.measureText(ping.name).width + 8;
        ctx.fillStyle = "rgba(0,0,0,0.6)";
        roundRectPath(ctx, cx - w / 2, cy - maxR - 2, w, 16, 4); ctx.fill();
        ctx.fillStyle = ping.color;
        ctx.fillText(ping.name, cx, cy - maxR + 8);
        ctx.textAlign = "left";
      }
      ctx.restore();
    }
  }

  function ensureRoster() {
    if (rosterEl) return rosterEl;
    rosterEl = document.createElement("div");
    rosterEl.id = "presenceRoster";
    rosterEl.style.cssText = "position:fixed;top:6px;left:50%;transform:translateX(-50%);z-index:60;display:none;gap:6px;pointer-events:auto;";
    document.body.appendChild(rosterEl);
    return rosterEl;
  }

  function updateRoster() {
    const el = ensureRoster();
    el.innerHTML = "";
    for (const [id, p] of presencePeers) {
      const on = followTarget === id;
      const focusTxt = p.focus ? "  ·  " + p.focus : "";
      const chip = document.createElement("button");
      chip.textContent = (on ? "◉ " : "") + String(p.name).slice(0, 18) + focusTxt;
      chip.title = (p.focus ? p.name + " — " + p.focus + ". " : "") +
                   (on ? "Following — click to stop" : "Click to follow " + p.name);
      chip.style.cssText = "pointer-events:auto;border:1px solid rgba(0,0,0,.5);border-radius:12px;padding:2px 10px;cursor:pointer;color:#111;font:600 12px system-ui,sans-serif;background:" + p.color + ";opacity:" + (on ? "1" : ".85") + ";";
      chip.onclick = () => {
        followTarget = (followTarget === id) ? null : id;
        followedCamKey = "";
        updateRoster();
        if (typeof window.syncMinimapControls === "function") window.syncMinimapControls();
      };
      el.appendChild(chip);
    }
    el.style.display = presencePeers.size ? "flex" : "none";
  }

  function presenceTick() {
    if (presencePeers.size > 0 || activePings.size > 0) renderZoneOverlay();
    requestAnimationFrame(presenceTick);
  }

  if (!window.__dfcapturePresenceStarted) {
    window.__dfcapturePresenceStarted = true;
    window.dfPresencePeers = () => [...presencePeers.values()];   // for the minimap viewport boxes
    view.addEventListener("pointermove", event => { myTile = worldTileFromEvent(event); postPresence(); });
    view.addEventListener("pointerleave", () => { myTile = null; postPresence(true); });
    // Alt+click on the map fires a "look here" ping (intercepted before it becomes a designation).
    view.addEventListener("pointerdown", event => {
      if (event.button === 0 && window.DFCaptureChat?.isPicking?.()) {
        event.preventDefault();
        event.stopImmediatePropagation();
        const t = worldTileFromEvent(event) || myTile;
        if (t) window.DFCaptureChat.consumeMapPick(t);
        return;
      }
      if (!event.altKey || event.button !== 0) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      const t = worldTileFromEvent(event) || myTile;
      if (!t) return;
      if (window.DFCaptureChat?.sendLocation) {
        window.DFCaptureChat.sendLocation(t);
        return;
      }
      const q = new URLSearchParams({ player, name: player, color: myColor, x: t.x, y: t.y, z: t.z });
      fetch("/ping?" + q.toString(), { method: "POST", cache: "no-store" }).catch(() => {});
    }, { capture: true });
    setInterval(pollPresence, PRESENCE_POLL_MS);
    setInterval(() => postPresence(true), 1500);
    requestAnimationFrame(presenceTick);
  }

  window.dfcFollowState = () => ({ following: followTarget });
  window.dfcStopFollowing = () => {
    followTarget = null;
    followedCamKey = "";
    updateRoster();
    if (typeof window.syncMinimapControls === "function") window.syncMinimapControls();
  };

  function startDfcapture() {
    if (window.__dfcaptureStarted) return;
    window.__dfcaptureStarted = true;
    loadFrame();
    if (typeof loadHud === "function") {
      loadHud();
      setInterval(loadHud, 1000);
    }
    if (typeof loadNotifications === "function") {
      loadNotifications();
      setInterval(loadNotifications, 500);
    }
  }

  function imagePixelFromEvent(event) {
    return tileAddressFromClient(event.clientX, event.clientY, false);
  }

  function renderedImageRect() {
    const nw = view.naturalWidth;
    const nh = view.naturalHeight;
    if (!nw || !nh) return null;
    const rect = viewClientRect();
    // #view uses object-fit: cover. Use the same scale and centered crop here so hover, clicks,
    // designations, zones, presence cursors, pings, and predictive pan all address the exact
    // natural-image pixel shown under the pointer.
    const scale = Math.max(rect.width / nw, rect.height / nh);
    const width = nw * scale;
    const height = nh * scale;
    return {
      left: rect.left + (rect.width - width) / 2,
      top: rect.top + (rect.height - height) / 2,
      width,
      height,
      scale
    };
  }

  // DF's renderer does not start the map at image pixel (0,0). renderer_2d places tile boundary
  // n at origin + floor(viewport_zoom_factor * 32 * n / 128). The frame endpoint records those
  // exact values while the per-player zoom guard is active, so every browser overlay and input
  // addresses the same pixels that DF rendered.
  function captureTileGrid() {
    const nw = view.naturalWidth;
    const nh = view.naturalHeight;
    const rendered = renderedImageRect();
    const fg = frameGrid;
    if (!nw || !nh || !fg || !rendered) return null;
    const w = Math.max(1, Number(fg.w) || 1);
    const h = Math.max(1, Number(fg.h) || 1);
    const stepNumerator = Math.max(1, Number(fg.zoomFactor) || 1) * 32;
    const originX = Number(fg.originX) || 0;
    const originY = Number(fg.originY) || 0;
    return {
      nw, nh, w, h, rendered,
      naturalX: tile => originX +
        Math.floor(stepNumerator * Math.max(0, Math.min(w, tile)) / 128),
      naturalY: tile => originY +
        Math.floor(stepNumerator * Math.max(0, Math.min(h, tile)) / 128)
    };
  }

  function tileAtNaturalPixel(pixel, count, boundary) {
    if (pixel < boundary(0) || pixel >= boundary(count)) return -1;
    let lo = 0, hi = count;
    while (lo + 1 < hi) {
      const mid = Math.floor((lo + hi) / 2);
      if (pixel < boundary(mid)) hi = mid;
      else lo = mid;
    }
    return lo;
  }

  function tileAddressFromClient(clientX, clientY, clamp) {
    const grid = captureTileGrid();
    if (!grid) return null;
    let nx = (clientX - grid.rendered.left) / grid.rendered.scale;
    let ny = (clientY - grid.rendered.top) / grid.rendered.scale;
    if (clamp) {
      nx = Math.max(grid.naturalX(0), Math.min(grid.naturalX(grid.w) - 1, nx));
      ny = Math.max(grid.naturalY(0), Math.min(grid.naturalY(grid.h) - 1, ny));
    }
    const tx = tileAtNaturalPixel(nx, grid.w, grid.naturalX);
    const ty = tileAtNaturalPixel(ny, grid.h, grid.naturalY);
    if (tx < 0 || ty < 0) return null;
    return { x: tx, y: ty, w: grid.w, h: grid.h };
  }

  function screenRectForMapTile(pos) {
    if (!pos || !currentHud?.camera || !currentHud?.viewport) return null;
    if (Number(pos.z) !== Number(currentHud.camera.z)) return null;
    const vp = currentHud.viewport;
    const tx = Number(pos.x) - Number(currentHud.camera.x);
    const ty = Number(pos.y) - Number(currentHud.camera.y);
    if (tx < 0 || ty < 0 || tx >= vp.w || ty >= vp.h) return null;
    const grid = captureTileGrid();
    if (!grid) return null;
    const x0 = grid.naturalX(tx), x1 = grid.naturalX(tx + 1);
    const y0 = grid.naturalY(ty), y1 = grid.naturalY(ty + 1);
    return {
      left: grid.rendered.left + x0 * grid.rendered.scale,
      top: grid.rendered.top + y0 * grid.rendered.scale,
      width: Math.max(1, (x1 - x0) * grid.rendered.scale),
      height: Math.max(1, (y1 - y0) * grid.rendered.scale)
    };
  }

  function resizeZoneOverlay() {
    const dpr = Math.max(1, window.devicePixelRatio || 1);
    const w = Math.max(1, Math.ceil(window.innerWidth));
    const h = Math.max(1, Math.ceil(window.innerHeight));
    if (zoneOverlay.width !== Math.ceil(w * dpr) || zoneOverlay.height !== Math.ceil(h * dpr)) {
      zoneOverlay.width = Math.ceil(w * dpr);
      zoneOverlay.height = Math.ceil(h * dpr);
      zoneOverlay.style.width = `${w}px`;
      zoneOverlay.style.height = `${h}px`;
    }
    const ctx = zoneOverlay.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return ctx;
  }

  function zoneExtentAt(zone, lx, ly) {
    const w = Number(zone.w) || 0;
    const h = Number(zone.h) || 0;
    if (lx < 0 || ly < 0 || lx >= w || ly >= h) return false;
    const ext = typeof zone.extents === "string" ? zone.extents : "";
    return ext.charAt(lx + ly * w) === "1";
  }

  function zoneShapeRow(zone, lx, ly) {
    const n = zoneExtentAt(zone, lx, ly - 1);
    const s = zoneExtentAt(zone, lx, ly + 1);
    const w = zoneExtentAt(zone, lx - 1, ly);
    const e = zoneExtentAt(zone, lx + 1, ly);
    const mask = (n ? 1 : 0) | (s ? 2 : 0) | (w ? 4 : 0) | (e ? 8 : 0);
    return ({
      15:0, 5:1, 9:2, 10:3, 6:4, 1:5, 8:6, 4:7,
      2:8, 3:9, 12:10, 13:11, 14:12, 7:13, 11:14, 0:15
    })[mask] ?? 15;
  }

  // Instant-mode drag selection: draw the tile-snapped golden rectangle directly on the
  // overlay canvas so it tracks the cursor with zero server round-trips. Works in
  // natural-image-pixel space (the same coords designateDrag commits with) and snaps to
  // whole tiles using the live viewport size, so the preview lands on exactly the tiles
  // the server will designate on release.
  function drawDragPreview(ctx) {
    if (!dragPreview) return;
    const grid = captureTileGrid();
    if (!grid) return;
    const { ax, ay, bx, by } = dragPreview;
    const tx0 = Math.max(0, Math.min(grid.w - 1, Math.min(ax, bx)));
    const tx1 = Math.max(1, Math.min(grid.w, Math.max(ax, bx) + 1));
    const ty0 = Math.max(0, Math.min(grid.h - 1, Math.min(ay, by)));
    const ty1 = Math.max(1, Math.min(grid.h, Math.max(ay, by) + 1));
    const x0 = grid.naturalX(tx0), x1 = grid.naturalX(tx1);
    const y0 = grid.naturalY(ty0), y1 = grid.naturalY(ty1);
    const sx = grid.rendered.left + x0 * grid.rendered.scale;
    const sy = grid.rendered.top + y0 * grid.rendered.scale;
    const sw = (x1 - x0) * grid.rendered.scale;
    const sh = (y1 - y0) * grid.rendered.scale;
    if (sw <= 0 || sh <= 0) return;
    ctx.save();
    ctx.imageSmoothingEnabled = false;
    ctx.fillStyle = "rgba(255, 196, 64, 0.16)";
    ctx.fillRect(sx, sy, sw, sh);
    // faint per-tile separators so the selection reads as DF tiles
    const minStepX = (grid.naturalX(1) - grid.naturalX(0)) * grid.rendered.scale;
    const minStepY = (grid.naturalY(1) - grid.naturalY(0)) * grid.rendered.scale;
    if (minStepX > 3 && minStepY > 3) {
      ctx.strokeStyle = "rgba(255, 210, 90, 0.22)";
      ctx.lineWidth = 1;
      ctx.beginPath();
      for (let tx = tx0 + 1; tx < tx1; ++tx) {
        const gx = grid.rendered.left + grid.naturalX(tx) * grid.rendered.scale;
        const px = Math.round(gx) + 0.5; ctx.moveTo(px, sy); ctx.lineTo(px, sy + sh);
      }
      for (let ty = ty0 + 1; ty < ty1; ++ty) {
        const gy = grid.rendered.top + grid.naturalY(ty) * grid.rendered.scale;
        const py = Math.round(gy) + 0.5; ctx.moveTo(sx, py); ctx.lineTo(sx + sw, py);
      }
      ctx.stroke();
    }
    // crisp gold border + corner brackets (DF selection feel)
    const L = Math.round(sx) + 1, T = Math.round(sy) + 1;
    const R = Math.round(sx + sw) - 1, B = Math.round(sy + sh) - 1;
    ctx.strokeStyle = "rgba(255, 214, 92, 0.95)";
    ctx.lineWidth = 2;
    ctx.strokeRect(L, T, R - L, B - T);
    const c = Math.max(3, Math.min(10, (R - L) / 2, (B - T) / 2));
    ctx.strokeStyle = "rgba(255, 236, 150, 1)";
    ctx.beginPath();
    ctx.moveTo(L, T + c); ctx.lineTo(L, T); ctx.lineTo(L + c, T);
    ctx.moveTo(R - c, T); ctx.lineTo(R, T); ctx.lineTo(R, T + c);
    ctx.moveTo(L, B - c); ctx.lineTo(L, B); ctx.lineTo(L + c, B);
    ctx.moveTo(R - c, B); ctx.lineTo(R, B); ctx.lineTo(R, B - c);
    ctx.stroke();
    ctx.restore();
  }

  function drawNativePlacementGrid(ctx) {
    let active = false;
    try { active = placementActive(); } catch (_) {}
    if (!active || !cursorSheet.complete) return;
    const grid = captureTileGrid();
    if (!grid) return;
    ctx.save();
    ctx.imageSmoothingEnabled = false;
    for (let tx = 0; tx < grid.w; ++tx) {
      const nx0 = grid.naturalX(tx), nx1 = grid.naturalX(tx + 1);
      const sx = grid.rendered.left + nx0 * grid.rendered.scale;
      const sw = (nx1 - nx0) * grid.rendered.scale;
      for (let ty = 0; ty < grid.h; ++ty) {
        const ny0 = grid.naturalY(ty), ny1 = grid.naturalY(ty + 1);
        const sy = grid.rendered.top + ny0 * grid.rendered.scale;
        const sh = (ny1 - ny0) * grid.rendered.scale;
        // CURSORS:0:22 is VIEWPORT_GRID, DF's native transparent gold grid cell.
        ctx.drawImage(cursorSheet, 0, 22 * 32, 32, 32, sx, sy, sw, sh);
      }
    }
    ctx.restore();
  }

  function renderZoneOverlay() {
    const ctx = resizeZoneOverlay();
    ctx.clearRect(0, 0, window.innerWidth, window.innerHeight);
    drawNativePlacementGrid(ctx);
    drawDragPreview(ctx);   // instant-mode selection (no-op when not dragging in instant mode)
    drawPresence(ctx);      // other players' cursors + their live designation rectangles
    drawPings(ctx);         // transient "look here" pings
    const cam = zoneSnapshotCamera || currentHud?.camera;
    const vp = zoneSnapshotViewport || currentHud?.viewport;
    if (!zoneOverlayEnabled || !cam || !vp || !zoneSheet.complete)
      return;
    const rendered = renderedImageRect();
    if (!rendered) return;
    const vpW = Math.max(1, Number(vp.w) || 1);
    const vpH = Math.max(1, Number(vp.h) || 1);
    const camX = Number(cam.x) || 0;
    const camY = Number(cam.y) || 0;
    const tileW = rendered.width / vpW;
    const tileH = rendered.height / vpH;
    ctx.imageSmoothingEnabled = false;

    for (const zone of currentZones) {
      if (Number(zone.z) !== Number(cam.z)) continue;
      const zw = Number(zone.w) || 0;
      const zh = Number(zone.h) || 0;
      const zx = Number(zone.x) || 0;
      const zy = Number(zone.y) || 0;
      const stateCol = zone.active ? 2 : 0;
      let iconDrawn = false;
      for (let ly = 0; ly < zh; ly++) {
        for (let lx = 0; lx < zw; lx++) {
          if (!zoneExtentAt(zone, lx, ly)) continue;
          const wx = zx + lx;
          const wy = zy + ly;
          const tx = wx - camX;
          const ty = wy - camY;
          if (tx < 0 || ty < 0 || tx >= vpW || ty >= vpH) continue;
          const dx = Math.round(rendered.left + tx * tileW);
          const dy = Math.round(rendered.top + ty * tileH);
          const dw = Math.max(1, Math.round(rendered.left + (tx + 1) * tileW) - dx);
          const dh = Math.max(1, Math.round(rendered.top + (ty + 1) * tileH) - dy);
          ctx.drawImage(zoneSheet, stateCol * 32, zoneShapeRow(zone, lx, ly) * 32, 32, 32,
            dx, dy, dw, dh);
          if (!iconDrawn) {
            const ix = Math.max(0, Math.min(7, Number(zone.iconX) || 0));
            const iy = Math.max(0, Math.min(15, Number(zone.iconY) || 0));
            ctx.drawImage(zoneSheet, ix * 32, iy * 32, 32, 32,
              dx, dy, dw, dh);
            iconDrawn = true;
          }
        }
      }
    }
  }

  async function loadZones() {
    if (!zoneOverlayEnabled) {
      currentZones = [];
      zoneSnapshotCamera = null;
      zoneSnapshotViewport = null;
      renderZoneOverlay();
      return;
    }
    try {
      const response = await fetch(`/zones?player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
      if (!response.ok) throw new Error("zones failed");
      const data = await response.json();
      currentZones = Array.isArray(data.zones) ? data.zones : [];
      zoneSnapshotCamera = data.camera || currentHud?.camera || null;
      zoneSnapshotViewport = data.viewport || currentHud?.viewport || null;
      renderZoneOverlay();
    } catch (_) {}
  }
  addEventListener("resize", renderZoneOverlay);
  view.addEventListener("load", renderZoneOverlay);

  const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

  async function flashMapTile(pos) {
    if (!pos) return;
    await loadHud();
    await sleep(80);
    const rect = screenRectForMapTile(pos);
    if (!rect) return;
    tileFlash.style.left = `${rect.left}px`;
    tileFlash.style.top = `${rect.top}px`;
    tileFlash.style.width = `${rect.width}px`;
    tileFlash.style.height = `${rect.height}px`;
    for (let i = 0; i < 4; i++) {
      tileFlash.style.display = "block";
      await sleep(150);
      tileFlash.style.display = "none";
      await sleep(120);
    }
  }

  // Like imagePixelFromEvent but clamps to the image edges, so a drag that ends
  // slightly off-frame still produces a valid corner for rectangle designation.
  function imagePixelClamped(clientX, clientY) {
    return tileAddressFromClient(clientX, clientY, true);
  }

  function selectionBuildingId(data) {
    const direct = Number(data?.buildingId ?? data?.building_id ?? -1);
    if (Number.isInteger(direct) && direct >= 0) return direct;
    const lines = Array.isArray(data?.lines) ? data.lines : [];
    for (const line of lines) {
      const m = String(line || "").match(/\bBuilding id:\s*(\d+)/i);
      if (m) return Number(m[1]);
    }
    return -1;
  }

  function showSelection(data) {
    const kind = String(data.kind || "").toLowerCase();
    const buildingId = selectionBuildingId(data);
    if (kind === "workshop" && buildingId >= 0) {
      openWorkshopPanel(buildingId);
      return;
    }
    if (kind === "unit" && data.unit) {
      showUnitSheet(data);
      return;
    }
    if (kind === "stockpile" && buildingId >= 0) {
      openStockpilePanel(buildingId);
      return;
    }
    if (kind === "building" && buildingId >= 0) {
      openBuildingPanel(buildingId);
      return;
    }
    if (kind === "item" && Number(data.itemId) >= 0) {
      openItemPanel(Number(data.itemId));
      return;
    }
    if (kind === "zone" && buildingId >= 0) {
      openZonePanel(buildingId);
      return;
    }
    const lines = Array.isArray(data.lines) ? data.lines : [];
    const tile = data.tile || {};
    selection.className = "";
    selection.innerHTML = `
      <div class="kind">${escapeHtml(data.kind || "tile")}</div>
      <h1>${escapeHtml(data.title || "Selection")}</h1>
      <div class="line">Tile: ${tile.x}, ${tile.y}, ${tile.z}</div>
      ${lines.map(line => `<div class="line">${escapeHtml(line)}</div>`).join("")}
      <div class="tile-detail-actions">
        <button type="button" data-tile-occupants>View everything on this tile</button>
        <button type="button" data-tile-engraving>Engraving details</button>
      </div>
    `;
    selection.classList.add("visible");
    selection.querySelector("[data-tile-occupants]")?.addEventListener("click", event => {
      event.preventDefault();
      if (typeof window.showTileOccupants === "function")
        window.showTileOccupants(tile).catch(() => {});
    });
    const engravingButton = selection.querySelector("[data-tile-engraving]");
    engravingButton?.addEventListener("click", event => {
      event.preventDefault();
      if (typeof window.showEngraving === "function")
        window.showEngraving(tile).catch(() => { engravingButton.disabled = true; });
    });
  }

  function closeSelection() {
    selectedUnitData = null;
    selection.className = "";
    selection.innerHTML = "";
  }

  // --- Building panel: suspend / resume / cancel construction (or remove a built building) ---
