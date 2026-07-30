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

  // ---- Help panel: searchable reference over the baked help corpus ---------------------------
  // Data lives in dfcapture-help-corpus.js and dfcapture-help-curated.js. The "hotkeys" surface is
  // replaced with this UI's actual keybindings, and unavailable feature surfaces are hidden.
  const HELP_HIDDEN_SURFACES = new Set(["audio", "chat", "analytics", "vote", "lobby", "obligations"]);
  const HELP_LOCAL_HOTKEYS = {
    id: "hotkeys", label: "Keyboard & mouse", kind: "hotkeys",
    entries: [
      { control: "Arrows / WASD / HJKL", text: "Pan the camera", group: "Camera" },
      { control: "[ or = / ] or -", text: "Zoom in / out", group: "Camera" },
      { control: "PageUp / PageDown", text: "Z-level up / down", group: "Camera" },
      { control: "e / q", text: "Z-level up / down (alternate)", group: "Camera" },
      { control: "Home or r", text: "Reset this camera to the host's", group: "Camera" },
      { control: "Wheel / Ctrl+Wheel", text: "Elevation and map zoom; assignments are configurable under Settings > Controls", group: "Mouse" },
      { control: "Click a tile", text: "Inspect what's there (unit, building, item, zone)", group: "Mouse" },
      { control: "Drag with a tool", text: "Paint the selection (dig, zone, stockpile, build)", group: "Mouse" },
      { control: "Configurable click", text: "Ping that spot for every player; choose the shortcut under Settings > Controls", group: "Multiplayer" },
      { control: "Esc", text: "Close the open panel or cancel targeting", group: "Panels" },
    ],
  };
  let helpQuery = "";
  let helpSurfaceId = "hotkeys";

  function helpSurfaces() {
    const corpus = (typeof DFHelpCorpus !== "undefined" && DFHelpCorpus && Array.isArray(DFHelpCorpus.surfaces))
      ? DFHelpCorpus.surfaces : [];
    const out = [HELP_LOCAL_HOTKEYS];
    for (const s of corpus) {
      if (!s || s.id === "hotkeys" || HELP_HIDDEN_SURFACES.has(s.id)) continue;
      out.push(s);
    }
    return out;
  }

  function helpCuratedNote(surfaceId, text) {
    try {
      const n = DFHelpCurated && DFHelpCurated.notes && DFHelpCurated.notes[surfaceId];
      return n ? (n[text] || null) : null;
    } catch (_) { return null; }
  }

  function openHelpPanel() {
    setActiveToolbar("help");
    clearBuildPlacement(false);
    activeInfoPanel = "help";
    renderHelp();
  }

  function renderHelp() {
    const surfaces = helpSurfaces();
    const q = helpQuery.trim().toLowerCase();
    let active = surfaces.find(s => s.id === helpSurfaceId) || surfaces[0];

    let rows;
    if (q) {
      // Global search across every visible surface.
      const hits = [];
      for (const s of surfaces) {
        for (const e of (s.entries || [])) {
          const hay = ((e.control || "") + " " + (e.text || "") + " " + (e.group || "")).toLowerCase();
          if (hay.includes(q)) hits.push({ s, e });
          if (hits.length >= 80) break;
        }
        if (hits.length >= 80) break;
      }
      rows = hits.length ? hits.map(({ s, e }) => helpEntryRow(s.id, e, s.label)).join("")
                         : `<div class="sq-empty">Nothing matches "${escapeHtml(helpQuery)}".</div>`;
    } else if (active) {
      const groups = new Map();
      for (const e of (active.entries || [])) {
        const g = e.group || "";
        if (!groups.has(g)) groups.set(g, []);
        groups.get(g).push(e);
      }
      rows = [...groups.entries()].map(([g, list]) =>
        (g ? `<div class="sq-section-title">${escapeHtml(g)}</div>` : "") +
        list.map(e => helpEntryRow(active.id, e, null)).join("")).join("");
      if (!rows) rows = `<div class="sq-empty">No entries.</div>`;
    } else {
      rows = `<div class="sq-empty">Help corpus unavailable.</div>`;
    }

    const surfaceList = surfaces.map(s =>
      `<div class="sq-item${(!q && active && s.id === active.id) ? " selected" : ""}" data-help-surface="${escapeHtml(s.id)}">
         <span class="sq-item-name">${escapeHtml(s.label || s.id)}</span>
         <span class="sq-item-count">${(s.entries || []).length}</span>
       </div>`).join("");

    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs"><span class="info-tab active">Help</span>
          <input id="helpSearch" class="sq-rename kit-search" type="text" placeholder="search everything..." value="${escapeHtml(helpQuery)}" spellcheck="false">
        </div>
        <div class="info-body" style="grid-template-columns:1fr;">
          <div class="sq-cols">
            <div class="sq-left"><div class="sq-list">${surfaceList}</div></div>
            <div class="sq-right">${rows}</div>
          </div>
        </div>
        <div class="info-footer"><div>Reference for this browser client. DF itself has deeper in-game guides.</div></div>
      </div>`;

    clientPanel.querySelectorAll("[data-help-surface]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation();
      helpSurfaceId = b.dataset.helpSurface; helpQuery = ""; renderHelp();
    }));
    const search = document.getElementById("helpSearch");
    if (search) {
      search.addEventListener("click", ev => ev.stopPropagation());
      search.addEventListener("keydown", ev => ev.stopPropagation());
      search.addEventListener("input", () => { helpQuery = search.value || ""; renderHelp(); const s2 = document.getElementById("helpSearch"); if (s2) { s2.focus(); s2.setSelectionRange(s2.value.length, s2.value.length); } });
    }
  }

  function helpEntryRow(surfaceId, e, surfaceLabel) {
    const note = helpCuratedNote(surfaceId, e.text || "");
    return `<div class="help-row">
        ${e.control ? `<span class="help-key">${escapeHtml(e.control)}</span>` : ""}
        <span class="help-text">${escapeHtml(e.text || "")}${surfaceLabel ? ` <span class="wm-dim">— ${escapeHtml(surfaceLabel)}</span>` : ""}
          ${note ? `<span class="help-note">${escapeHtml(note)}</span>` : ""}</span>
      </div>`;
  }
