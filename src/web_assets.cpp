#include "web_assets.h"

#include <string>

namespace dfcapture_public {

const char* index_html() {
    static const std::string html = std::string(R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>dfcapture</title>
  <style>
    :root {
      color-scheme: dark;
      --bg: #111;
      --panel: #191919;
      --line: #c28713;
      --text: #f8f0dc;
      --muted: #b9afa2;
      --green: #35d64b;
      --yellow: #ffd15a;
    }

    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      background: var(--bg);
      color: var(--text);
      font: 13px/1.25 Consolas, "Liberation Mono", monospace;
      overflow: hidden;
    }

    .topbar {
      height: 70px;
      display: grid;
      grid-template-columns: minmax(180px, 1.2fr) auto auto auto 220px;
      align-items: stretch;
      gap: 14px;
      padding: 7px 10px;
      border-bottom: 2px solid var(--line);
      background: #090909;
    }

    .fort {
      display: grid;
      align-content: center;
      min-width: 0;
    }

    .fort-name, .date-line {
      color: #fff;
      font-weight: 700;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .site-rank, .small {
      color: var(--muted);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .pop {
      display: grid;
      align-content: center;
      gap: 4px;
    }

    .pop-row {
      display: flex;
      align-items: center;
      gap: 4px;
    }

    .mood-dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      border: 1px solid #000;
      background: #666;
    }

    .stocks {
      display: grid;
      grid-template-columns: auto auto;
      align-content: center;
      gap: 4px 16px;
      min-width: 150px;
    }

    .label {
      color: #fff;
      font-weight: 700;
    }

    .value {
      color: var(--yellow);
      font-weight: 700;
    }

    .date {
      display: grid;
      grid-template-columns: 34px auto;
      align-content: center;
      align-items: center;
      gap: 8px;
      min-width: 170px;
    }

    #moon {
      width: 30px;
      height: 30px;
      image-rendering: pixelated;
    }

    .mapbox {
      display: grid;
      grid-template-columns: 124px auto;
      gap: 6px;
      align-items: center;
      justify-content: end;
      min-width: 0;
    }

    #minimap {
      width: 124px;
      height: 54px;
      border: 1px solid var(--line);
      background: #222;
      image-rendering: pixelated;
      cursor: crosshair;
    }

    .map-actions {
      display: grid;
      grid-template-columns: repeat(3, 36px);
      gap: 4px;
    }

    .stage {
      height: calc(100vh - 70px);
      position: relative;
      background: #000;
    }

    .frame {
      width: 100%;
      height: 100%;
      object-fit: contain;
      image-rendering: auto;
      display: block;
    }

    .controls {
      position: absolute;
      right: 16px;
      bottom: 16px;
      display: grid;
      grid-template-columns: repeat(3, 40px);
      gap: 6px;
      padding: 8px;
      border: 2px solid var(--line);
      background: rgba(12, 12, 12, 0.92);
    }

    .tool-palette {
      position: absolute;
      left: 50%;
      bottom: 16px;
      transform: translateX(-50%);
      display: grid;
      grid-template-columns: repeat(6, minmax(62px, auto));
      gap: 5px;
      padding: 8px;
      border: 2px solid var(--line);
      background: rgba(12, 12, 12, 0.92);
      z-index: 4;
    }

    .tool-palette .tool {
      min-width: 62px;
      height: 30px;
      padding: 0 7px;
    }

    .tool-palette .tool.active,
    .priority button.active {
      background: #0f6d2a;
      color: #fff;
    }

    .priority {
      display: contents;
    }

    .priority-label {
      grid-column: span 2;
      align-self: center;
      color: var(--muted);
      text-align: right;
      padding-right: 4px;
    }

    .priority button {
      min-width: 31px;
      height: 28px;
    }

    .selection-box {
      position: absolute;
      display: none;
      pointer-events: none;
      border: 2px solid #ffd15a;
      background: rgba(255, 209, 90, 0.16);
      box-shadow: 0 0 0 1px #000 inset;
      z-index: 3;
    }

    button {
      height: 32px;
      border: 1px solid var(--line);
      background: #1d1d1d;
      color: #ffd15a;
      font: inherit;
      cursor: pointer;
    }

    button:hover {
      background: #3a2a0e;
    }

    .status {
      position: absolute;
      left: 12px;
      bottom: 12px;
      max-width: min(720px, 70vw);
      padding: 5px 8px;
      color: var(--muted);
      border: 1px solid rgba(194, 135, 19, 0.6);
      background: rgba(9, 9, 9, 0.86);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .alerts {
      position: absolute;
      left: 12px;
      top: 12px;
      display: grid;
      gap: 6px;
      z-index: 2;
    }

    .alert-button {
      width: 34px;
      height: 34px;
      border-radius: 50%;
      border: 2px solid var(--line);
      background: rgba(20, 20, 20, 0.92);
      color: #fff;
      display: grid;
      place-items: center;
      font-weight: 700;
      box-shadow: 0 1px 0 #000;
    }

    .alert-button:hover,
    .alert-button.active {
      background: #3a2a0e;
      transform: translateX(2px);
    }

    .alert-popover {
      position: absolute;
      left: 54px;
      top: 12px;
      display: none;
      min-width: 360px;
      max-width: min(760px, 72vw);
      padding: 10px 12px;
      border: 2px solid var(--line);
      background: rgba(10, 10, 10, 0.96);
      color: var(--text);
      z-index: 3;
      white-space: normal;
      box-shadow: 0 2px 0 #000;
    }

    .alert-popover.visible {
      display: block;
    }

    .alert-help {
      color: var(--muted);
      margin-bottom: 6px;
    }

    .alert-line {
      color: #fff;
      margin-top: 3px;
    }

    .alert-line.important {
      color: #ff4b4b;
    }

    .alert-line.good {
      color: #35d64b;
    }
  </style>
</head>
<body>
  <header class="topbar">
    <section class="fort">
      <div class="fort-name" id="fortName">Fortress</div>
      <div class="site-rank" id="siteRank">Outpost</div>
    </section>
    <section class="pop">
      <div><span class="label">Pop</span> <span class="value" id="popTotal">0</span></div>
      <div class="pop-row" id="happiness"></div>
    </section>
    <section class="stocks">
      <div class="label">Food</div><div class="value" id="food">~0</div>
      <div class="label">Drink</div><div class="value" id="drink">~0</div>
    </section>
    <section class="date">
      <canvas id="moon" width="30" height="30"></canvas>
      <div>
        <div class="date-line" id="monthLine">1st Granite</div>
        <div class="date-line" id="seasonLine">Early Spring</div>
        <div class="date-line" id="yearLine">Year 1</div>
      </div>
    </section>
    <section class="mapbox">
      <canvas id="minimap" width="124" height="54"></canvas>
      <div class="map-actions">
        <button id="homeBtn" title="Center on host camera">Home</button>
        <button id="surfaceBtn" title="Move to surface elevation">Surf</button>
        <button id="deepBtn" title="Move to deepest discovered elevation">Deep</button>
        <button id="refreshBtn" title="Restart stream">Live</button>
        <button id="resetBtn" title="Reset this browser camera and diagnostics">Reset</button>
      </div>
    </section>
  </header>
  <main class="stage">
    <img class="frame" id="frame" alt="">
    <div class="selection-box" id="selectionBox"></div>
    <div class="alerts" id="alerts" aria-label="announcement alerts"></div>
    <div class="alert-popover" id="alertPopover"></div>
    <div class="status" id="status">connecting...</div>
    <div class="tool-palette" id="toolPalette" aria-label="designation tools">
      <button class="tool" data-tool="dig">Dig</button>
      <button class="tool" data-tool="channel">Channel</button>
      <button class="tool" data-tool="ramp">Ramp</button>
      <button class="tool" data-tool="stairs">Stairs</button>
      <button class="tool" data-tool="up">Up</button>
      <button class="tool" data-tool="down">Down</button>
      <button class="tool" data-tool="smooth">Smooth</button>
      <button class="tool" data-tool="engrave">Engrave</button>
      <button class="tool" data-tool="fortify">Fortify</button>
      <button class="tool" data-tool="track">Track</button>
      <button class="tool" data-tool="chop">Chop</button>
      <button class="tool" data-tool="gather">Gather</button>
      <button class="tool" data-tool="clear">Erase</button>
      <span class="priority-label">Priority</span>
      <span class="priority" id="priorityButtons"></span>
    </div>
    <div class="controls" aria-label="camera controls">
      <button data-dx="0" data-dy="-10">N</button>
      <button data-dx="0" data-dy="0" data-dz="1">Up</button>
      <button data-dx="10" data-dy="0">E</button>
      <button data-dx="-10" data-dy="0">W</button>
      <button data-dx="0" data-dy="0" data-dz="-1">Dn</button>
      <button data-dx="0" data-dy="10">S</button>
      <button id="zoomInBtn" title="Zoom in">Z+</button>
      <button id="zoomResetBtn" title="Reset zoom">100</button>
      <button id="zoomOutBtn" title="Zoom out">Z-</button>
    </div>
  </main>
  <script>
)HTML") +
R"JS(
    const params = new URLSearchParams(location.search);
    const player = params.get("player") || "default";
    const frame = document.getElementById("frame");
    const status = document.getElementById("status");
    const alertsRail = document.getElementById("alerts");
    const alertPopover = document.getElementById("alertPopover");
    const minimap = document.getElementById("minimap");
    const minimapCtx = minimap.getContext("2d");
    const moon = document.getElementById("moon");
    const moonCtx = moon.getContext("2d");
    const selectionBox = document.getElementById("selectionBox");
    const toolPalette = document.getElementById("toolPalette");
    const priorityButtons = document.getElementById("priorityButtons");
    let streamToken = 0;
    let moving = false;
    let hud = null;
    let lastNotifications = null;
    let lastState = null;
    let activeTool = null;
    let designationPriority = 4;
    let dragStart = null;
    let lastCursorSent = 0;

    const moodColors = ["#35d64b", "#68da52", "#a3d64b", "#ffd15a", "#ffae42", "#ff7040", "#ff4b4b"];
    const reportColors = [
      "#202020", "#1f4bd6", "#1aa13a", "#20b8c8", "#b03030", "#a02cc0", "#b67a20", "#b8b8b8",
      "#777777", "#668cff", "#41e060", "#55f0ff", "#ff4b4b", "#ff4fff", "#ffd15a", "#ffffff"
    ];
    const minimapColors = [
      "#6d5638", "#9b6f24", "#7c8586", "#3a3a3a", "#4e4e4e",
      "#8c8b78", "#9c8b53", "#315d9b", "#b53030", "#4c8f36",
      "#1f5f2e", "#7c7b2e", "#d9e4e8", "#a4a5a0", "#111111"
    ];

    function ordinal(day) {
      const mod10 = day % 10;
      const mod100 = day % 100;
      if (mod10 === 1 && mod100 !== 11) return `${day}st`;
      if (mod10 === 2 && mod100 !== 12) return `${day}nd`;
      if (mod10 === 3 && mod100 !== 13) return `${day}rd`;
      return `${day}th`;
    }

    function setStatus(text) {
      status.textContent = text;
    }

    function frameImageRect() {
      const rect = frame.getBoundingClientRect();
      const naturalW = frame.naturalWidth || rect.width || 1;
      const naturalH = frame.naturalHeight || rect.height || 1;
      const imageRatio = naturalW / naturalH;
      const rectRatio = rect.width / Math.max(1, rect.height);
      if (rectRatio > imageRatio) {
        const height = rect.height;
        const width = height * imageRatio;
        const left = rect.left + (rect.width - width) / 2;
        return { left, top: rect.top, width, height, right: left + width, bottom: rect.top + height };
      }
      const width = rect.width;
      const height = width / imageRatio;
      const top = rect.top + (rect.height - height) / 2;
      return { left: rect.left, top, width, height, right: rect.left + width, bottom: top + height };
    }

    function eventToFramePixel(event) {
      const rect = frameImageRect();
      if (event.clientX < rect.left || event.clientY < rect.top ||
          event.clientX > rect.right || event.clientY > rect.bottom) {
        return null;
      }
      const w = frame.naturalWidth || Math.max(1, Math.round(rect.width));
      const h = frame.naturalHeight || Math.max(1, Math.round(rect.height));
      const px = Math.max(0, Math.min(w - 1, Math.floor((event.clientX - rect.left) / rect.width * w)));
      const py = Math.max(0, Math.min(h - 1, Math.floor((event.clientY - rect.top) / rect.height * h)));
      return { px, py, w, h, rect };
    }

    function setActiveTool(tool) {
      activeTool = activeTool === tool ? null : tool;
      dragStart = null;
      selectionBox.style.display = "none";
      document.querySelectorAll(".tool").forEach((button) => {
        button.classList.toggle("active", button.dataset.tool === activeTool);
      });
      const mode = activeTool ? activeTool : "none";
      const qs = new URLSearchParams({ player, mode });
      fetch(`/placement-mode?${qs.toString()}`, { method: "POST", cache: "no-store" }).catch(() => {});
      setStatus(activeTool ? `${activeTool} designation active` : "designation tool cleared");
    }

    function updateSelectionBox(a, b) {
      if (!a || !b) {
        selectionBox.style.display = "none";
        return;
      }
      const rect = a.rect;
      const x1 = Math.min(a.px, b.px) / Math.max(1, a.w) * rect.width + rect.left;
      const y1 = Math.min(a.py, b.py) / Math.max(1, a.h) * rect.height + rect.top;
      const x2 = (Math.max(a.px, b.px) + 1) / Math.max(1, a.w) * rect.width + rect.left;
      const y2 = (Math.max(a.py, b.py) + 1) / Math.max(1, a.h) * rect.height + rect.top;
      selectionBox.style.left = `${Math.round(x1)}px`;
      selectionBox.style.top = `${Math.round(y1)}px`;
      selectionBox.style.width = `${Math.max(2, Math.round(x2 - x1))}px`;
      selectionBox.style.height = `${Math.max(2, Math.round(y2 - y1))}px`;
      selectionBox.style.display = "block";
    }

    function sendPlacementCursor(point) {
      const now = performance.now();
      if (now - lastCursorSent < 70) return;
      lastCursorSent = now;
      const params = point
        ? {
            player,
            hx: point.px,
            hy: point.py,
            w: point.w,
            h: point.h,
            drag: dragStart ? 1 : 0,
            dx: dragStart ? dragStart.px : -1,
            dy: dragStart ? dragStart.py : -1,
            bw: 0,
            bh: 0,
          }
        : { player, hx: -1, hy: -1, w: 0, h: 0, drag: 0, dx: -1, dy: -1, bw: 0, bh: 0 };
      const qs = new URLSearchParams(params);
      fetch(`/placement-cursor?${qs.toString()}`, { method: "POST", cache: "no-store" }).catch(() => {});
    }

    async function designateRectangle(a, b) {
      if (!activeTool || !a || !b) return;
      const qs = new URLSearchParams({
        player,
        tool: activeTool,
        priority: designationPriority,
        px: a.px,
        py: a.py,
        px2: b.px,
        py2: b.py,
        w: a.w,
        h: a.h,
      });
      const res = await fetch(`/designate?${qs.toString()}`, { method: "POST", cache: "no-store" });
      if (!res.ok) throw new Error(await res.text());
      const data = await res.json();
      setStatus(`${activeTool}: ${data.count} tile${data.count === 1 ? "" : "s"} marked`);
      refreshHud().catch(() => {});
    }

    function reportColor(row) {
      const idx = Math.max(0, Math.min(reportColors.length - 1, Number(row && row.color || 7)));
      return reportColors[idx];
    }

    function alertIcon(entry) {
      const report = entry.reports && entry.reports.length ? entry.reports[0] : entry;
      if (!report || !report.text) return "!";
      const text = report.text.toLowerCase();
      if (text.includes("summer") || text.includes("autumn") || text.includes("winter") || text.includes("spring")) return "*";
      if (text.includes("struck down") || text.includes("death") || text.includes("dead")) return "X";
      if (text.includes("completed") || text.includes("grown") || text.includes("became")) return "+";
      return "!";
    }

    function targetForNotification(entry) {
      if (entry && entry.target) return entry.target;
      if (entry && entry.pos) return entry.pos;
      if (entry && entry.reports) {
        const found = entry.reports.find((row) => row.pos);
        if (found) return found.pos;
      }
      return null;
    }

    function reportsForEntry(entry) {
      if (!entry) return [];
      if (entry.reports && entry.reports.length) return entry.reports;
      return [entry];
    }

    function showAlertPopover(entry) {
      const rows = reportsForEntry(entry).slice(0, 5);
      alertPopover.innerHTML = "";
      const help = document.createElement("div");
      help.className = "alert-help";
      help.textContent = "Click an alert with a position to center the camera.";
      alertPopover.appendChild(help);

      rows.forEach((row) => {
        const line = document.createElement("div");
        line.className = "alert-line";
        if (row.color === 4 || row.color === 12) line.classList.add("important");
        if (row.color === 2 || row.color === 10) line.classList.add("good");
        line.textContent = row.repeatCount > 0 ? `${row.text} x${row.repeatCount + 1}` : row.text;
        line.style.color = reportColor(row);
        alertPopover.appendChild(line);
      });
      alertPopover.classList.add("visible");
    }

    function hideAlertPopover() {
      alertPopover.classList.remove("visible");
    }

    function renderNotifications(data) {
      lastNotifications = data;
      alertsRail.innerHTML = "";
      const entries = (data.alerts && data.alerts.length ? data.alerts : (data.recent || []).slice(0, 6)).slice(0, 10);
      entries.forEach((entry, idx) => {
        const report = reportsForEntry(entry)[0] || {};
        const button = document.createElement("button");
        button.className = "alert-button";
        button.textContent = alertIcon(entry);
        button.title = report.text || "Announcement";
        button.style.borderColor = reportColor(report);
        button.addEventListener("mouseenter", () => showAlertPopover(entry));
        button.addEventListener("mouseleave", hideAlertPopover);
        button.addEventListener("click", () => {
          document.querySelectorAll(".alert-button.active").forEach((el) => el.classList.remove("active"));
          button.classList.add("active");
          showAlertPopover(entry);
          const target = targetForNotification(entry);
          if (target) {
            setCamera(target.x, target.y, target.z).catch((err) => setStatus(err.message));
          } else {
            setStatus(`notification ${idx + 1} has no map position`);
          }
        });
        alertsRail.appendChild(button);
      });
    }

    async function refreshNotifications() {
      const res = await fetch(`/notifications?player=${encodeURIComponent(player)}`, { cache: "no-store" });
      if (!res.ok) throw new Error(await res.text());
      renderNotifications(await res.json());
    }

    function renderState(data) {
      lastState = data;
      if (!data || !data.capture || !data.capture.failures) return;
      const c = data.capture;
      if (c.failures > 0 && c.lastError) {
        setStatus(`capture warnings: ${c.failures} failures, last: ${c.lastError}`);
      }
    }

    async function refreshState() {
      const res = await fetch(`/state?player=${encodeURIComponent(player)}`, { cache: "no-store" });
      if (!res.ok) throw new Error(await res.text());
      renderState(await res.json());
    }

)JS" +
R"JS(
    function drawMoon(icon) {
      const cx = 15;
      const cy = 15;
      moonCtx.clearRect(0, 0, 30, 30);
      moonCtx.fillStyle = "#080808";
      moonCtx.fillRect(0, 0, 30, 30);
      moonCtx.fillStyle = "#6f67b8";
      moonCtx.beginPath();
      moonCtx.arc(cx, cy, 13, 0, Math.PI * 2);
      moonCtx.fill();
      moonCtx.fillStyle = "#080808";
      const phase = Math.max(0, Math.min(7, Number(icon || 0)));
      const offsets = [8, 5, 0, -20, -8, -5, 0, 20];
      moonCtx.beginPath();
      moonCtx.arc(cx + offsets[phase], cy, 13, 0, Math.PI * 2);
      moonCtx.fill();
      moonCtx.strokeStyle = "#b6b0ef";
      moonCtx.strokeRect(0.5, 0.5, 29, 29);
    }

    function drawMinimap(data) {
      if (!data || !data.minimap || !data.minimap.cells) return;
      const mm = data.minimap;
      const image = minimapCtx.createImageData(mm.w, mm.h);
      for (let i = 0; i < mm.cells.length; i++) {
        const ch = mm.cells.charCodeAt(i);
        const idx = ch <= 57 ? ch - 48 : ch - 87;
        const color = minimapColors[Math.max(0, Math.min(minimapColors.length - 1, idx))];
        image.data[i * 4 + 0] = parseInt(color.slice(1, 3), 16);
        image.data[i * 4 + 1] = parseInt(color.slice(3, 5), 16);
        image.data[i * 4 + 2] = parseInt(color.slice(5, 7), 16);
        image.data[i * 4 + 3] = 255;
      }

      const off = document.createElement("canvas");
      off.width = mm.w;
      off.height = mm.h;
      off.getContext("2d").putImageData(image, 0, 0);
      minimapCtx.imageSmoothingEnabled = false;
      minimapCtx.clearRect(0, 0, minimap.width, minimap.height);
      minimapCtx.drawImage(off, 0, 0, minimap.width, minimap.height);

      const map = data.map || { w: 1, h: 1 };
      const vp = data.viewport || { w: 1, h: 1 };
      const cam = data.camera || { x: 0, y: 0 };
      const x = cam.x / Math.max(1, map.w) * minimap.width;
      const y = cam.y / Math.max(1, map.h) * minimap.height;
      const w = Math.max(4, vp.w / Math.max(1, map.w) * minimap.width);
      const h = Math.max(4, vp.h / Math.max(1, map.h) * minimap.height);
      minimapCtx.strokeStyle = "#ffb000";
      minimapCtx.lineWidth = 1;
      minimapCtx.strokeRect(Math.round(x) + 0.5, Math.round(y) + 0.5, Math.round(w), Math.round(h));
    }

    function renderHud(data) {
      hud = data;
      document.getElementById("fortName").textContent = data.fort.name || "Fortress";
      document.getElementById("siteRank").textContent = `${data.fort.site || "Site"} / ${data.fort.rank || "Outpost"}`;
      document.getElementById("popTotal").textContent = data.population.total;
      document.getElementById("food").textContent = `~${data.stocks.food}`;
      document.getElementById("drink").textContent = `~${data.stocks.drink}`;
      document.getElementById("monthLine").textContent = `${ordinal(data.date.day)} ${data.date.monthName}`;
      document.getElementById("seasonLine").textContent = data.date.season;
      document.getElementById("yearLine").textContent = `Year ${data.date.year}`;

      const happiness = document.getElementById("happiness");
      happiness.innerHTML = "";
      data.happiness.forEach((count, idx) => {
        const dot = document.createElement("span");
        dot.className = "mood-dot";
        dot.style.background = moodColors[idx] || "#666";
        dot.title = String(count);
        happiness.appendChild(dot);
      });

      drawMoon(data.date.moonIcon);
      drawMinimap(data);
      setStatus(`${player} camera ${data.camera.x}, ${data.camera.y}, ${data.camera.z} / Zoom ${data.camera.zoom}% / Elevation ${data.elevation}`);
    }

)JS" +
R"JS(
    async function refreshHud() {
      const res = await fetch(`/hud?player=${encodeURIComponent(player)}`, { cache: "no-store" });
      if (!res.ok) throw new Error(await res.text());
      renderHud(await res.json());
    }

    function startStream() {
      streamToken += 1;
      frame.src = `/stream.mjpg?player=${encodeURIComponent(player)}&t=${streamToken}`;
    }

    async function postCamera(path, params) {
      const qs = new URLSearchParams({ player, ...params });
      const res = await fetch(`${path}?${qs.toString()}`, { method: "POST", cache: "no-store" });
      if (!res.ok) throw new Error(await res.text());
      const cam = await res.json();
      setStatus(`${player} camera ${cam.x}, ${cam.y}, ${cam.z}`);
      refreshHud().catch((err) => setStatus(err.message));
      return cam;
    }

    async function moveCamera(dx, dy, dz) {
      if (moving) return;
      moving = true;
      try { await postCamera("/camera/move", { dx, dy, dz }); }
      finally { moving = false; }
    }

    async function setCamera(x, y, z) {
      if (moving) return;
      moving = true;
      try { await postCamera("/camera/set", { x, y, z }); }
      finally { moving = false; }
    }

    async function zoomCamera(dir) {
      const qs = new URLSearchParams({ player, dir });
      const res = await fetch(`/zoom?${qs.toString()}`, { method: "POST", cache: "no-store" });
      if (!res.ok) throw new Error(await res.text());
      const cam = await res.json();
      setStatus(`${player} zoom ${cam.zoom}%`);
      refreshHud().catch((err) => setStatus(err.message));
      return cam;
    }

    for (let n = 1; n <= 7; n++) {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = String(n);
      button.className = n === designationPriority ? "active" : "";
      button.addEventListener("click", () => {
        designationPriority = n;
        priorityButtons.querySelectorAll("button").forEach((el) => {
          el.classList.toggle("active", Number(el.textContent) === n);
        });
        setStatus(`designation priority ${n}`);
      });
      priorityButtons.appendChild(button);
    }

    toolPalette.querySelectorAll(".tool").forEach((button) => {
      button.addEventListener("click", () => setActiveTool(button.dataset.tool));
    });

    frame.addEventListener("mousedown", (event) => {
      if (!activeTool) return;
      const point = eventToFramePixel(event);
      if (!point) return;
      event.preventDefault();
      dragStart = point;
      updateSelectionBox(point, point);
      sendPlacementCursor(point);
    });

    frame.addEventListener("mousemove", (event) => {
      if (!activeTool) return;
      const point = eventToFramePixel(event);
      if (!point) {
        sendPlacementCursor(null);
        if (!dragStart) selectionBox.style.display = "none";
        return;
      }
      if (dragStart) updateSelectionBox(dragStart, point);
      sendPlacementCursor(point);
    });

    frame.addEventListener("mouseleave", () => {
      if (!dragStart) {
        sendPlacementCursor(null);
        selectionBox.style.display = "none";
      }
    });

    window.addEventListener("mouseup", (event) => {
      if (!activeTool || !dragStart) return;
      const start = dragStart;
      dragStart = null;
      const end = eventToFramePixel(event) || start;
      updateSelectionBox(start, end);
      designateRectangle(start, end)
        .catch((err) => setStatus(err.message.trim ? err.message.trim() : String(err)))
        .finally(() => {
          setTimeout(() => {
            if (!dragStart) selectionBox.style.display = "none";
          }, 160);
          sendPlacementCursor(null);
        });
    });

    document.querySelectorAll("button[data-dx]").forEach((button) => {
      button.addEventListener("click", () => {
        const dx = Number(button.dataset.dx || 0);
        const dy = Number(button.dataset.dy || 0);
        const dz = Number(button.dataset.dz || 0);
        moveCamera(dx, dy, dz).catch((err) => setStatus(err.message));
      });
    });

    document.getElementById("homeBtn").addEventListener("click", () => {
      postCamera("/camera/home", {}).catch((err) => setStatus(err.message));
    });
    document.getElementById("surfaceBtn").addEventListener("click", () => {
      if (hud) setCamera(hud.camera.x, hud.camera.y, hud.minimap.surfaceZ).catch((err) => setStatus(err.message));
    });
    document.getElementById("deepBtn").addEventListener("click", () => {
      if (hud) setCamera(hud.camera.x, hud.camera.y, hud.minimap.deepestZ).catch((err) => setStatus(err.message));
    });
    document.getElementById("refreshBtn").addEventListener("click", startStream);
    document.getElementById("zoomInBtn").addEventListener("click", () => {
      zoomCamera("in").catch((err) => setStatus(err.message));
    });
    document.getElementById("zoomOutBtn").addEventListener("click", () => {
      zoomCamera("out").catch((err) => setStatus(err.message));
    });
    document.getElementById("zoomResetBtn").addEventListener("click", () => {
      zoomCamera("reset").catch((err) => setStatus(err.message));
    });
    document.getElementById("resetBtn").addEventListener("click", async () => {
      try {
        const cam = await postCamera("/reset", {});
        setStatus(`${player} reset to ${cam.x}, ${cam.y}, ${cam.z}`);
        startStream();
      } catch (err) {
        setStatus(err.message);
      }
    });

    minimap.addEventListener("click", (event) => {
      if (!hud || !hud.map) return;
      const rect = minimap.getBoundingClientRect();
      const px = (event.clientX - rect.left) / rect.width;
      const py = (event.clientY - rect.top) / rect.height;
      const x = Math.round(px * hud.map.w - (hud.viewport ? hud.viewport.w / 2 : 0));
      const y = Math.round(py * hud.map.h - (hud.viewport ? hud.viewport.h / 2 : 0));
      setCamera(x, y, hud.camera.z).catch((err) => setStatus(err.message));
    });

    window.addEventListener("keydown", (event) => {
      if (event.repeat) return;
      const step = event.shiftKey ? 20 : 10;
      const key = event.key.toLowerCase();
      if (key === "arrowup" || key === "w") {
        event.preventDefault();
        moveCamera(0, -step, 0).catch((err) => setStatus(err.message));
      } else if (key === "arrowdown" || key === "s") {
        event.preventDefault();
        moveCamera(0, step, 0).catch((err) => setStatus(err.message));
      } else if (key === "arrowleft" || key === "a") {
        event.preventDefault();
        moveCamera(-step, 0, 0).catch((err) => setStatus(err.message));
      } else if (key === "arrowright" || key === "d") {
        event.preventDefault();
        moveCamera(step, 0, 0).catch((err) => setStatus(err.message));
      } else if (key === "q") {
        event.preventDefault();
        moveCamera(0, 0, -1).catch((err) => setStatus(err.message));
      } else if (key === "e") {
        event.preventDefault();
        moveCamera(0, 0, 1).catch((err) => setStatus(err.message));
      } else if (key === "=" || key === "+") {
        event.preventDefault();
        zoomCamera("in").catch((err) => setStatus(err.message));
      } else if (key === "-" || key === "_") {
        event.preventDefault();
        zoomCamera("out").catch((err) => setStatus(err.message));
      } else if (key === "0") {
        event.preventDefault();
        zoomCamera("reset").catch((err) => setStatus(err.message));
      } else if (key === "escape") {
        if (activeTool) {
          event.preventDefault();
          setActiveTool(activeTool);
        }
      }
    });

    frame.addEventListener("load", () => refreshHud().catch(() => {}));
    frame.addEventListener("error", () => {
      setStatus("waiting for DF stream...");
      setTimeout(startStream, 1000);
    });
    startStream();
    refreshHud().catch((err) => setStatus(err.message));
    refreshNotifications().catch(() => {});
    refreshState().catch(() => {});
    setInterval(() => refreshHud().catch(() => {}), 2000);
    setInterval(() => refreshNotifications().catch(() => {}), 3000);
    setInterval(() => refreshState().catch(() => {}), 5000);
  </script>
</body>
</html>)JS";
    return html.c_str();
}

} // namespace dfcapture_public
