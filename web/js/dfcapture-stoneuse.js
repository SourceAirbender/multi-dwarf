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

  // ---- Stone use: DF's Labor > "Stone use" screen -----------------------------------------------
  // GET /stone-use lists economic stones (name, magma-safe, what industries use them, whether
  // they're selected for ordinary jobs) plus the plain "other" stones. POST /stone-use toggles one.
  // Renders inside the Labor panel's main region, like the Kitchen tab.
  let stoneData = null;
  let stoneFilter = "";

  async function openStoneUseEmbedded() {
    const host = clientPanel.querySelector(".info-main");
    if (host) host.innerHTML = `<div class="info-message">Loading stone use...</div>`;
    await refreshStoneUse();
  }

  async function refreshStoneUse() {
    try {
      const r = await fetch(`/stone-use?player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
      const d = await r.json().catch(() => ({}));
      stoneData = (d && d.ok !== false) ? d : null;
    } catch (_) { stoneData = null; }
    renderStoneUse();
  }

  function renderStoneUse() {
    const host = clientPanel.querySelector(".info-main");
    if (!host) return;
    if (!stoneData) {
      host.innerHTML = `<div class="info-message">Stone use data unavailable.</div>`;
      return;
    }
    const f = stoneFilter.trim().toLowerCase();
    const economic = Array.isArray(stoneData.economic) ? stoneData.economic : [];
    const other = Array.isArray(stoneData.other) ? stoneData.other : [];
    const match = s => !f || String(s.name || "").toLowerCase().includes(f);

    const ecoRows = economic.filter(match).map(s => `
        <div class="stone-row">
          <span class="kit-name">${escapeHtml(s.name || "")}${s.magmaSafe ? ` <span class="stone-magma" title="Magma-safe">magma</span>` : ""}</span>
          <span class="stone-uses">${escapeHtml(Array.isArray(s.uses) ? s.uses.join(", ") : (s.uses || ""))}</span>
          <button class="kit-cell ${s.selected ? "yes" : "no"}" data-stone-mat="${s.matType}:${s.matIndex}" data-stone-val="${s.selected ? 0 : 1}"
            title="${s.selected ? "Available for ordinary jobs — click to reserve for its industry" : "Reserved for its industry — click to allow ordinary jobs"}">${s.selected ? "Use" : "Keep"}</button>
        </div>`).join("");

    const otherRows = other.filter(match).map(s =>
      `<span class="stone-chip">${escapeHtml(s.name || "")}${s.magmaSafe ? " *" : ""}</span>`).join("");

    host.innerHTML = `
      <div class="kit-embed-tabs">
        <input id="stoneSearch" class="sq-rename kit-search" type="text" placeholder="filter stones..." value="${escapeHtml(stoneFilter)}" spellcheck="false">
        <span id="stoneStatus" class="sq-status"></span>
      </div>
      <div class="sq-section-title">Economic stone (${economic.length})</div>
      <div class="stone-body">
        ${ecoRows || `<div class="sq-empty">No economic stone${f ? " matching the filter" : ""}.</div>`}
      </div>
      <div class="sq-section-title">Other stone (${other.length})</div>
      <div class="stone-chips">${otherRows || `<div class="sq-empty">None.</div>`}</div>
      <div class="sq-subtle kit-note">"Use" means masons and crafters may spend it on ordinary jobs; "Keep" reserves it for the industry that needs it. * = magma-safe.</div>`;

    const search = document.getElementById("stoneSearch");
    if (search) {
      search.addEventListener("click", ev => ev.stopPropagation());
      search.addEventListener("keydown", ev => ev.stopPropagation());
      search.addEventListener("input", () => {
        stoneFilter = search.value || ""; renderStoneUse();
        const s2 = document.getElementById("stoneSearch");
        if (s2) { s2.focus(); s2.setSelectionRange(s2.value.length, s2.value.length); }
      });
    }
    host.querySelectorAll("[data-stone-mat]").forEach(b => b.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      const st = document.getElementById("stoneStatus");
      try {
        const r = await fetch(`/stone-use?player=${encodeURIComponent(player)}&mat=${encodeURIComponent(b.dataset.stoneMat)}&value=${b.dataset.stoneVal}&t=${Date.now()}`,
                              { method: "POST", cache: "no-store" });
        const d = await r.json().catch(() => ({}));
        if (!r.ok || d.ok === false) throw new Error(d.error || "toggle failed");
        await refreshStoneUse();
        const st2 = document.getElementById("stoneStatus");
        if (st2) { st2.textContent = "Updated."; st2.className = "sq-status"; }
      } catch (err) {
        if (st) { st.textContent = err.message || "Could not update."; st.className = "sq-status err"; }
      }
    }));
  }
