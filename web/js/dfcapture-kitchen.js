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

  // ---- Kitchen panel: GET /kitchen (cookable stock + seed plants) + POST /kitchen-toggle -------
  // DF's kitchen screen in the browser: per-item Cook/Brew permission cells. Item rows address
  // the exclusion by type/mat/matIndex; plant rows by plant id (their cook cell is the SEED
  // toggle, matching the game).
  let kitData = null;
  let kitView = "items";   // items | plants
  let kitFilter = "";
  // Kitchen is a LABOR sub-screen in DF, so it normally renders inside the Labor panel's main
  // region (its "Kitchen" tab) rather than owning the window. Standalone mode is kept for a
  // direct openKitchenPanel() call.
  let kitEmbedded = false;

  async function openKitchenPanel() {
    kitEmbedded = false;
    setActiveToolbar("kitchen");
    clearBuildPlacement(false);
    activeInfoPanel = "kitchen";
    clientPanel.className = "visible info-panel";
    if (!clientPanel.querySelector(".kit-body")) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading kitchen...</div></div></div>`;
    }
    await refreshKitchen();
  }

  // Called by the Labor panel's "Kitchen" tab: keep Labor's own chrome (top tabs + section tabs)
  // and paint only its main region.
  async function openKitchenEmbedded() {
    kitEmbedded = true;
    const host = clientPanel.querySelector(".info-main");
    if (host) host.innerHTML = `<div class="info-message">Loading kitchen...</div>`;
    await refreshKitchen();
  }

  async function refreshKitchen() {
    try {
      const r = await fetch(`/kitchen?player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
      if (!r.ok) throw new Error("kitchen failed");
      kitData = await r.json();
      renderKitchen();
    } catch (_) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Kitchen data unavailable.</div></div></div>`;
    }
  }

  function kitToggleCell(kind, allowed, capable, attrs) {
    const verb = kind === "cook" ? "Cook" : "Brew";
    if (!capable) return `<span class="kit-cell off" title="Not ${kind === "cook" ? "cookable" : "brewable"}">—</span>`;
    const label = allowed ? verb : "No";
    const title = `${kind === "cook" ? "Cooking" : "Brewing"} ${allowed ? "permitted — click to forbid" : "forbidden — click to permit"}`;
    return `<button class="kit-cell ${allowed ? "yes" : "no"}" data-kit-toggle="${kind}" ${attrs} title="${title}">${label}</button>`;
  }

  function renderKitchen() {
    const d = kitData || {};
    const items = Array.isArray(d.items) ? d.items : [];
    const plants = Array.isArray(d.plants) ? d.plants : [];
    const f = kitFilter.trim().toLowerCase();
    const tabs = [["items", `Stock (${items.length})`], ["plants", `Seeds (${plants.length})`]]
      .map(([k, label]) => `<button class="info-tab${kitView === k ? " active" : ""}" data-kit-tab="${k}">${escapeHtml(label)}</button>`).join("");

    let rows;
    if (kitView === "plants") {
      const list = f ? plants.filter(p => (p.name || "").toLowerCase().includes(f)) : plants;
      rows = list.length ? list.map(p => `
        <div class="kit-row">
          <span class="kit-name">${escapeHtml(p.name || "")}</span>
          <span class="kit-cells">
            ${kitToggleCell("cook", p.seedCookAllowed, p.cookCapable, `data-kit-id="${p.id}"`)}
            ${kitToggleCell("brew", p.brewAllowed, p.brewCapable, `data-kit-id="${p.id}"`)}
          </span>
        </div>`).join("") : `<div class="sq-empty">No matching plants.</div>`;
    } else {
      const list = f ? items.filter(i => (i.name || "").toLowerCase().includes(f)) : items;
      rows = list.length ? list.map(i => `
        <div class="kit-row">
          <span class="kit-name">${escapeHtml(i.name || "")}</span>
          <span class="kit-count">${Number(i.count) || 0}</span>
          <span class="kit-cells">
            ${kitToggleCell("cook", i.cookAllowed, i.cookCapable, `data-kit-type="${i.type}" data-kit-mat="${i.mat}" data-kit-matindex="${i.matIndex}"`)}
            ${kitToggleCell("brew", i.brewAllowed, i.brewCapable, `data-kit-type="${i.type}" data-kit-mat="${i.mat}" data-kit-matindex="${i.matIndex}"`)}
          </span>
        </div>`).join("") : `<div class="sq-empty">No cookable stock${f ? " matching the filter" : ""}.</div>`;
    }

    const head = `<div class="kit-head"><span></span>${kitView === "items" ? "<span class=\"kit-count\">#</span>" : ""}<span class="kit-cells kit-cells-head"><span>Cook</span><span>Brew</span></span></div>`;
    const toolbar = `${tabs}
      <input id="kitSearch" class="sq-rename kit-search" type="text" placeholder="filter..." value="${escapeHtml(kitFilter)}" spellcheck="false">
      <span id="kitStatus" class="sq-status"></span>`;

    const host = kitEmbedded ? clientPanel.querySelector(".info-main") : null;
    if (host) {
      // Inside the Labor panel: its own tab chrome stays, we own only the main region.
      host.innerHTML = `<div class="kit-embed-tabs">${toolbar}</div>
        <div class="kit-body kit-embedded">${head}${rows}</div>
        <div class="sq-subtle kit-note">Cooking a seed or brewable plant consumes it — forbid what your farms and stills need.</div>`;
    } else {
      clientPanel.className = "visible info-panel";
      clientPanel.innerHTML = `
        <div class="info-window">
          <div class="info-top-tabs">${toolbar}</div>
          <div class="info-body" style="grid-template-columns:1fr;">
            <div class="kit-body">${head}${rows}</div>
          </div>
          <div class="info-footer"><div>Cooking a seed or brewable plant consumes it — forbid what your farms and stills need.</div></div>
        </div>`;
    }

    clientPanel.querySelectorAll("[data-kit-tab]").forEach(b => b.addEventListener("click", e => {
      e.preventDefault(); e.stopPropagation(); kitView = b.dataset.kitTab; renderKitchen();
    }));
    const search = document.getElementById("kitSearch");
    if (search) {
      search.addEventListener("click", ev => ev.stopPropagation());
      search.addEventListener("keydown", ev => ev.stopPropagation());
      search.addEventListener("input", () => { kitFilter = search.value || ""; renderKitchen(); const s2 = document.getElementById("kitSearch"); if (s2) { s2.focus(); s2.setSelectionRange(s2.value.length, s2.value.length); } });
    }
    clientPanel.querySelectorAll("[data-kit-toggle]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const mode = b.dataset.kitToggle;                  // cook | brew
      const wasAllowed = b.classList.contains("yes");
      const qs = new URLSearchParams();
      qs.set("player", player);
      qs.set("mode", mode);
      qs.set("on", wasAllowed ? "0" : "1");
      if (b.dataset.kitId != null) qs.set("id", b.dataset.kitId);
      else { qs.set("type", b.dataset.kitType); qs.set("mat", b.dataset.kitMat); qs.set("matIndex", b.dataset.kitMatindex); }
      qs.set("t", Date.now());
      try {
        const r = await fetch(`/kitchen-toggle?${qs.toString()}`, { method: "POST", cache: "no-store" });
        const res = await r.json().catch(() => ({}));
        if (!r.ok || res.ok === false) throw new Error(res.error || "toggle failed");
        await refreshKitchen();
        const st = document.getElementById("kitStatus");
        if (st) { st.textContent = `${mode === "brew" ? "Brewing" : "Cooking"} ${wasAllowed ? "forbidden" : "permitted"}.`; st.className = "sq-status"; }
      } catch (err) {
        const st = document.getElementById("kitStatus");
        if (st) { st.textContent = err.message || "Could not update."; st.className = "sq-status err"; }
      }
    }));
  }
