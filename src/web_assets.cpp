#include "web_assets.h"

namespace dfcapture_public {

const char* index_html() {
    return R"HTML(<!doctype html>
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
    }

    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      background: var(--bg);
      color: var(--text);
      font: 14px/1.4 Consolas, "Liberation Mono", monospace;
      overflow: hidden;
    }

    .topbar {
      height: 48px;
      display: flex;
      align-items: center;
      gap: 16px;
      padding: 0 14px;
      border-bottom: 2px solid var(--line);
      background: #090909;
    }

    .brand {
      color: #ffd15a;
      font-weight: 700;
    }

    .stage {
      height: calc(100vh - 48px);
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
      grid-template-columns: repeat(3, 36px);
      gap: 6px;
      padding: 8px;
      border: 2px solid var(--line);
      background: rgba(12, 12, 12, 0.92);
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
      color: var(--muted);
    }
  </style>
</head>
<body>
  <header class="topbar">
    <div class="brand">dfcapture</div>
    <div class="status" id="status">connecting...</div>
  </header>
  <main class="stage">
    <img class="frame" id="frame" alt="">
    <div class="controls" aria-label="camera controls">
      <button data-dx="0" data-dy="-10">N</button>
      <button data-dx="0" data-dy="0" data-dz="1">Up</button>
      <button data-dx="10" data-dy="0">E</button>
      <button data-dx="-10" data-dy="0">W</button>
      <button data-dx="0" data-dy="0" data-dz="-1">Dn</button>
      <button data-dx="0" data-dy="10">S</button>
    </div>
  </main>
  <script>
    const params = new URLSearchParams(location.search);
    const player = params.get("player") || "default";
    const frame = document.getElementById("frame");
    const status = document.getElementById("status");

    async function refreshCamera() {
      const res = await fetch(`/camera?player=${encodeURIComponent(player)}`, { cache: "no-store" });
      if (!res.ok) throw new Error(await res.text());
      const cam = await res.json();
      status.textContent = `${player} camera ${cam.x}, ${cam.y}, ${cam.z}`;
    }

    function refreshFrame() {
      frame.src = `/frame.jpg?player=${encodeURIComponent(player)}&t=${Date.now()}`;
    }

    async function moveCamera(dx, dy, dz) {
      const qs = new URLSearchParams({ player, dx, dy, dz });
      const res = await fetch(`/camera/move?${qs.toString()}`, { method: "POST", cache: "no-store" });
      if (!res.ok) throw new Error(await res.text());
      const cam = await res.json();
      status.textContent = `${player} camera ${cam.x}, ${cam.y}, ${cam.z}`;
      refreshFrame();
    }

    document.querySelectorAll("button[data-dx]").forEach((button) => {
      button.addEventListener("click", () => {
        const dx = Number(button.dataset.dx || 0);
        const dy = Number(button.dataset.dy || 0);
        const dz = Number(button.dataset.dz || 0);
        moveCamera(dx, dy, dz).catch((err) => { status.textContent = err.message; });
      });
    });

    frame.addEventListener("load", () => refreshCamera().catch(() => {}));
    frame.addEventListener("error", () => { status.textContent = "waiting for DF frame..."; });
    refreshFrame();
    setInterval(refreshFrame, 250);
    refreshCamera().catch((err) => { status.textContent = err.message; });
  </script>
</body>
</html>)HTML";
}

} // namespace dfcapture_public
