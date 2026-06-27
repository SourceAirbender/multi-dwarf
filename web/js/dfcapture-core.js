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
  // Live drag selection rectangle in natural-image-pixel space, or null. Drawn on #zoneOverlay.
  let dragPreview = null;

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
    const rect = viewClientRect();
    const scale = Math.min(rect.width / nw, rect.height / nh);
    const tileW = (nw * scale) / Math.max(1, Number(vp.w) || 1);
    const tileH = (nh * scale) / Math.max(1, Number(vp.h) || 1);
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
      const blob = await response.blob();
      const nextUrl = URL.createObjectURL(blob);
      const oldUrl = currentFrameUrl;
      currentFrameUrl = nextUrl;
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
      if (event.shiftKey) {
        sendZoom(event.deltaY < 0 ? "in" : "out");
      } else {
        queueMove(0, 0, event.deltaY < 0 ? zstep : -zstep);
      }
    }, { passive: false, capture: true });
  }

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
    const nw = view.naturalWidth;
    const nh = view.naturalHeight;
    if (!nw || !nh) return null;
    const rendered = renderedImageRect();
    if (!rendered) return null;
    const { left, top, scale } = rendered;
    const x = (event.clientX - left) / scale;
    const y = (event.clientY - top) / scale;
    if (x < 0 || y < 0 || x >= nw || y >= nh) return null;
    return { x: Math.floor(x), y: Math.floor(y), w: nw, h: nh };
  }

  function renderedImageRect() {
    const nw = view.naturalWidth;
    const nh = view.naturalHeight;
    if (!nw || !nh) return null;
    const rect = viewClientRect();
    const scale = Math.min(rect.width / nw, rect.height / nh);
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

  function screenRectForMapTile(pos) {
    if (!pos || !currentHud?.camera || !currentHud?.viewport) return null;
    if (Number(pos.z) !== Number(currentHud.camera.z)) return null;
    const vp = currentHud.viewport;
    const tx = Number(pos.x) - Number(currentHud.camera.x);
    const ty = Number(pos.y) - Number(currentHud.camera.y);
    if (tx < 0 || ty < 0 || tx >= vp.w || ty >= vp.h) return null;
    const rendered = renderedImageRect();
    if (!rendered) return null;
    return {
      left: rendered.left + (tx / vp.w) * rendered.width,
      top: rendered.top + (ty / vp.h) * rendered.height,
      width: Math.max(8, rendered.width / vp.w),
      height: Math.max(8, rendered.height / vp.h)
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
    const nw = view.naturalWidth, nh = view.naturalHeight;
    if (!nw || !nh) return;
    const rendered = renderedImageRect();
    if (!rendered) return;
    const vp = currentHud?.viewport;
    const vpW = Math.max(1, Number(vp?.w) || nw);
    const vpH = Math.max(1, Number(vp?.h) || nh);
    const tpx = nw / vpW, tpy = nh / vpH;        // tile size in natural-image pixels
    const { ax, ay, bx, by } = dragPreview;
    // snap to whole tiles, inclusive of the tile under each endpoint
    const x0 = Math.floor(Math.min(ax, bx) / tpx) * tpx;
    const x1 = (Math.floor(Math.max(ax, bx) / tpx) + 1) * tpx;
    const y0 = Math.floor(Math.min(ay, by) / tpy) * tpy;
    const y1 = (Math.floor(Math.max(ay, by) / tpy) + 1) * tpy;
    const sx = rendered.left + x0 * rendered.scale;
    const sy = rendered.top + y0 * rendered.scale;
    const sw = (x1 - x0) * rendered.scale;
    const sh = (y1 - y0) * rendered.scale;
    if (sw <= 0 || sh <= 0) return;
    ctx.save();
    ctx.imageSmoothingEnabled = false;
    ctx.fillStyle = "rgba(255, 196, 64, 0.16)";
    ctx.fillRect(sx, sy, sw, sh);
    // faint per-tile separators so the selection reads as DF tiles
    const stepX = tpx * rendered.scale, stepY = tpy * rendered.scale;
    if (stepX > 3 && stepY > 3) {
      ctx.strokeStyle = "rgba(255, 210, 90, 0.22)";
      ctx.lineWidth = 1;
      ctx.beginPath();
      for (let gx = sx + stepX; gx < sx + sw - 0.5; gx += stepX) {
        const px = Math.round(gx) + 0.5; ctx.moveTo(px, sy); ctx.lineTo(px, sy + sh);
      }
      for (let gy = sy + stepY; gy < sy + sh - 0.5; gy += stepY) {
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

  function renderZoneOverlay() {
    const ctx = resizeZoneOverlay();
    ctx.clearRect(0, 0, window.innerWidth, window.innerHeight);
    drawDragPreview(ctx);   // instant-mode selection (no-op when not dragging in instant mode)
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
    const nw = view.naturalWidth;
    const nh = view.naturalHeight;
    if (!nw || !nh) return null;
    const rect = viewClientRect();
    const scale = Math.min(rect.width / nw, rect.height / nh);
    const left = rect.left + (rect.width - nw * scale) / 2;
    const top = rect.top + (rect.height - nh * scale) / 2;
    let x = (clientX - left) / scale;
    let y = (clientY - top) / scale;
    x = Math.max(0, Math.min(nw - 1, x));
    y = Math.max(0, Math.min(nh - 1, y));
    return { x: Math.floor(x), y: Math.floor(y), w: nw, h: nh };
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
    selection.className = "";
    selection.innerHTML = `
      <div class="kind">${escapeHtml(data.kind || "tile")}</div>
      <h1>${escapeHtml(data.title || "Selection")}</h1>
      <div class="line">Tile: ${data.tile.x}, ${data.tile.y}, ${data.tile.z}</div>
      ${lines.map(line => `<div class="line">${escapeHtml(line)}</div>`).join("")}
    `;
    selection.classList.add("visible");
  }

  function closeSelection() {
    selectedUnitData = null;
    selection.className = "";
    selection.innerHTML = "";
  }

  // --- Building panel: suspend / resume / cancel construction (or remove a built building) ---
