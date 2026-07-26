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

  // ---- Worldmap panel: GET /world-map -> sites / civilizations / missions over the DF world ----
  // Pure fetch + <canvas> plot. The map canvas is
  // deliberately NOT covered by the crisp-text filter (that thresholds alpha and would wreck it).
  let wmData = null;
  let wmView = "map";  // map | civs | missions

  async function openWorldmapPanel() {
    setActiveToolbar("worldmap");
    clearBuildPlacement(false);
    activeInfoPanel = "worldmap";
    clientPanel.className = "visible info-panel";
    if (!clientPanel.querySelector(".wm-body")) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading the known world...</div></div></div>`;
    }
    try {
      const r = await fetch(`/world-map?player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
      if (!r.ok) throw new Error("world-map failed");
      wmData = await r.json();
      renderWorldmap();
    } catch (_) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">World map unavailable.</div></div></div>`;
    }
  }

  function renderWorldmap() {
    const d = wmData || {};
    const sites = Array.isArray(d.sites) ? d.sites : [];
    const civs = Array.isArray(d.civs) ? d.civs : [];
    const missions = Array.isArray(d.missions) ? d.missions : [];
    const tabs = [["map", "Map"], ["civs", `Civilizations (${civs.length})`], ["missions", `Missions (${missions.length})`]]
      .map(([k, label]) => `<button class="info-tab${wmView === k ? " active" : ""}" data-wm-tab="${k}">${escapeHtml(label)}</button>`).join("");

    let bodyHtml;
    if (wmView === "civs") bodyHtml = wmRenderCivs(civs);
    else if (wmView === "missions") bodyHtml = wmRenderMissions(missions);
    else bodyHtml = `<canvas id="wmCanvas" class="wm-canvas" title="Click a site"></canvas><div id="wmSiteInfo" class="wm-siteinfo">${sites.length} sites known.</div>`;

    const footer = d.regionName ? `Home region: ${escapeHtml(d.regionName)}` : "The known world";
    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs">${tabs}</div>
        <div class="info-body" style="grid-template-columns:1fr;"><div class="wm-body">${bodyHtml}</div></div>
        <div class="info-footer"><div>${footer}</div></div>
      </div>`;

    clientPanel.querySelectorAll("[data-wm-tab]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      wmView = b.dataset.wmTab;
      if (wmView === "missions" && typeof window.openMissionsPanel === "function") {
        window.openMissionsPanel();
        return;
      }
      renderWorldmap();
    }));
    if (wmView === "map") requestAnimationFrame(() => wmDrawMap(sites, d));
  }

  function wmDrawMap(sites, d) {
    const canvas = document.getElementById("wmCanvas");
    if (!canvas) return;
    const W = Math.max(1, Number(d.width) || 1), H = Math.max(1, Number(d.height) || 1);
    const box = canvas.parentElement.getBoundingClientRect();
    const avail = Math.max(120, Math.min((box.width || 400) - 6, 560));
    const scale = avail / Math.max(W, H);
    canvas.width = Math.max(1, Math.round(W * scale));
    canvas.height = Math.max(1, Math.round(H * scale));
    const ctx = canvas.getContext("2d");
    ctx.fillStyle = "#0c1410";
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    let ownSite = null;
    for (const s of sites) {
      const x = (Number(s.x) + 0.5) * scale, y = (Number(s.y) + 0.5) * scale;
      ctx.fillStyle = s.own ? "#ffd25c" : "#7fa87f";
      ctx.beginPath();
      ctx.arc(x, y, s.own ? 4 : 2, 0, Math.PI * 2);
      ctx.fill();
      if (s.own) ownSite = { x, y };
    }
    if (ownSite) {
      ctx.strokeStyle = "#fff3b0";
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.arc(ownSite.x, ownSite.y, 7, 0, Math.PI * 2);
      ctx.stroke();
    }
    canvas.onclick = ev => {
      const rect = canvas.getBoundingClientRect();
      const cx = ev.clientX - rect.left, cy = ev.clientY - rect.top;
      let best = null, bd = 1e9;
      for (const s of sites) {
        const dx = (Number(s.x) + 0.5) * scale - cx, dy = (Number(s.y) + 0.5) * scale - cy;
        const dd = dx * dx + dy * dy;
        if (dd < bd) { bd = dd; best = s; }
      }
      const info = document.getElementById("wmSiteInfo");
      if (info && best && bd < 400) {
        info.innerHTML = `<b>${escapeHtml(best.name || "site")}</b> — ${escapeHtml(best.type || "")} ` +
          `<span class="wm-dim">(${best.x}, ${best.y})</span>${best.own ? ` <span class="wm-own">your fort</span>` : ""}`;
      }
    };
  }

  function wmRenderCivs(civs) {
    if (!civs.length) return `<div class="sq-empty">No civilizations known.</div>`;
    const rows = civs.slice().sort((a, b) => (Number(b.siteCount) || 0) - (Number(a.siteCount) || 0)).map(c => {
      const rel = String(c.relation || "").toLowerCase();
      return `<div class="wm-civ">
          <span class="wm-civ-name">${escapeHtml(c.name || "")}</span>
          <span class="wm-rel wm-rel-${escapeHtml(rel)}">${escapeHtml(c.relation || "—")}</span>
          <span class="wm-dim">${Number(c.siteCount) || 0} sites · pop ${Number(c.population) || 0}</span>
        </div>`;
    }).join("");
    return `<div class="wm-list">${rows}</div>`;
  }

  function wmRenderMissions(missions) {
    if (!missions.length) return `<div class="sq-empty">No active missions from your fort.</div>`;
    const rows = missions.map(m => `
        <div class="wm-mission">
          <div><span class="wm-civ-name">${escapeHtml(m.goal || "Mission")}</span>
          <span class="wm-dim">→ ${escapeHtml(m.targetSite || "?")} · ${(m.squadIds || []).length} squad(s)</span></div>
          ${m.reportTitle ? `<div class="wm-report">${escapeHtml(m.reportTitle)}</div>` : ""}
        </div>`).join("");
    return `<div class="wm-list">${rows}</div>`;
  }
