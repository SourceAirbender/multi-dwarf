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

  // ---- Trade Depot panel: /depots + /depot-info + /depot-goods + /depot-mark + /depot-broker ----
  // See the caravan at your depot, request the broker, and mark goods for trade. The barter screen
  // transaction remains native-only; goods marked here still reach the depot.
  let tdDepots = [];
  let tdSelId = null;
  let tdInfo = null;
  let tdGoods = null;
  let tdFilter = "";
  let tdShowAll = false;

  async function openTradeDepotPanel() {
    setActiveToolbar("tradedepot");
    clearBuildPlacement(false);
    activeInfoPanel = "tradedepot";
    clientPanel.className = "visible info-panel";
    if (!clientPanel.querySelector(".td-body")) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading trade depot...</div></div></div>`;
    }
    try {
      const r = await fetch(`/depots?player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
      const d = await r.json();
      tdDepots = Array.isArray(d.depots) ? d.depots : [];
      if (tdSelId == null || !tdDepots.some(x => Number(x.id) === Number(tdSelId)))
        tdSelId = tdDepots.length ? Number(tdDepots[0].id) : null;
      await refreshTradeDepot();
    } catch (_) {
      clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Trade depot data unavailable.</div></div></div>`;
    }
  }

  async function refreshTradeDepot() {
    tdInfo = null; tdGoods = null;
    if (tdSelId != null) {
      try {
        const [ri, rg] = await Promise.all([
          fetch(`/depot-info?player=${encodeURIComponent(player)}&id=${tdSelId}&t=${Date.now()}`, { cache: "no-store" }),
          fetch(`/depot-goods?player=${encodeURIComponent(player)}&id=${tdSelId}${tdShowAll ? "&all=1" : ""}&t=${Date.now()}`, { cache: "no-store" }),
        ]);
        tdInfo = await ri.json().catch(() => null);
        tdGoods = await rg.json().catch(() => null);
      } catch (_) {}
    }
    renderTradeDepot();
  }

  function tdStatus(msg, isErr) {
    const el = document.getElementById("tdStatus");
    if (el) { el.textContent = msg || ""; el.className = "sq-status" + (isErr ? " err" : ""); }
  }

  function renderTradeDepot() {
    const picker = tdDepots.length > 1
      ? `<select id="tdPick" class="sq-select">${tdDepots.map(x => `<option value="${x.id}"${Number(x.id) === Number(tdSelId) ? " selected" : ""}>${escapeHtml(x.name)}</option>`).join("")}</select>`
      : (tdDepots.length === 1 ? `<span class="hosp-title">${escapeHtml(tdDepots[0].name)}</span>` : "");

    let body;
    if (!tdDepots.length) {
      body = `<div class="sq-hint">No trade depot yet. Build one (b -> Trade Depot) near your fort entrance so caravans can reach it.</div>`;
    } else if (!tdInfo || tdInfo.ok === false) {
      body = `<div class="info-message">Depot unavailable.</div>`;
    } else {
      const caravans = Array.isArray(tdInfo.caravans) ? tdInfo.caravans : [];
      const activeCars = caravans.filter(c => c.active);
      const carRows = activeCars.length ? activeCars.map(c => `
          <div class="td-caravan">
            <span class="wm-civ-name">${escapeHtml(c.origin || "caravan")}</span>
            <span class="wm-dim">${escapeHtml(c.state || "")}${c.atDepot ? " · at depot" : ""}${Number(c.daysRemaining) > 0 ? ` · ~${c.daysRemaining}d left` : ""} · ${Number(c.goodsCount) || 0} goods</span>
          </div>`).join("") : `<div class="sq-subtle">No caravan on the map right now.</div>`;

      const b = tdInfo.broker || {};
      const brokerLine = b.found
        ? `Broker: <b>${escapeHtml(b.name || "")}</b> <span class="wm-dim">(${escapeHtml(b.position || "broker")})</span>`
        : `<span class="wm-dim">No broker appointed (Nobles screen).</span>`;

      const goods = (tdGoods && Array.isArray(tdGoods.goods)) ? tdGoods.goods : [];
      const f = tdFilter.trim().toLowerCase();
      const list = f ? goods.filter(g => (g.desc || "").toLowerCase().includes(f)) : goods;
      const goodRows = list.length ? list.map(g => `
          <div class="td-good${g.pending || g.atDepot ? " marked" : ""}">
            <span class="kit-name">${escapeHtml(g.desc || "")}</span>
            <span class="td-val">${Number(g.value) || 0}&#9788;</span>
            <span class="td-state">${g.atDepot ? "at depot" : (g.pending ? "hauling" : "")}</span>
            <button class="sq-btn tiny" data-td-mark="${g.id}" data-td-on="${g.pending || g.atDepot ? 0 : 1}">${g.pending || g.atDepot ? "Unmark" : "Trade"}</button>
          </div>`).join("") : `<div class="sq-empty">No tradeable goods${f ? " matching the filter" : ""}.</div>`;
      const truncNote = tdGoods && tdGoods.truncated && !tdShowAll
        ? `<div class="sq-form-row"><button class="sq-btn" data-td-all>Show all (capped at ${Number(tdGoods.cap) || 400})</button></div>` : "";

      body = `
        <div class="sq-section-title">Caravans</div>
        ${carRows}
        <div class="sq-section-title">Depot</div>
        <div class="td-flags">
          <span>${brokerLine}</span>
          <label class="sq-check"><input type="checkbox" id="tdReq"${tdInfo.traderRequested ? " checked" : ""}> Trader requested</label>
          <label class="sq-check"><input type="checkbox" id="tdAnyone"${tdInfo.anyoneCanTrade ? " checked" : ""}> Anyone can trade</label>
          <span class="wm-dim">${Number(tdInfo.goodsAtDepot) || 0} at depot · ${Number(tdInfo.pendingCount) || 0} hauling</span>
        </div>
        <div class="sq-section-title">Goods</div>
        <div class="td-goods">${goodRows}</div>
        ${truncNote}`;
    }

    clientPanel.className = "visible info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-top-tabs"><span class="info-tab active">Trade Depot</span>${picker}
          <input id="tdSearch" class="sq-rename kit-search" type="text" placeholder="filter goods..." value="${escapeHtml(tdFilter)}" spellcheck="false">
          <span id="tdStatus" class="sq-status"></span>
        </div>
        <div class="info-body" style="grid-template-columns:1fr;"><div class="td-body">${body}</div></div>
        <div class="info-footer"><div>Mark goods here and dwarves haul them to the depot. Complete barter transactions in Dwarf Fortress.</div></div>
      </div>`;

    const pick = document.getElementById("tdPick");
    if (pick) pick.addEventListener("change", () => { tdSelId = Number(pick.value); refreshTradeDepot(); });
    const search = document.getElementById("tdSearch");
    if (search) {
      search.addEventListener("click", ev => ev.stopPropagation());
      search.addEventListener("keydown", ev => ev.stopPropagation());
      search.addEventListener("input", () => { tdFilter = search.value || ""; renderTradeDepot(); const s2 = document.getElementById("tdSearch"); if (s2) { s2.focus(); s2.setSelectionRange(s2.value.length, s2.value.length); } });
    }
    const allBtn = clientPanel.querySelector("[data-td-all]");
    if (allBtn) allBtn.addEventListener("click", e => { e.preventDefault(); e.stopPropagation(); tdShowAll = true; refreshTradeDepot(); });
    const req = document.getElementById("tdReq");
    if (req) req.addEventListener("change", async () => {
      try { await fetch(`/depot-broker?player=${encodeURIComponent(player)}&id=${tdSelId}&request=${req.checked ? 1 : 0}&t=${Date.now()}`, { method: "POST", cache: "no-store" }); tdStatus("Depot flag updated.", false); }
      catch (_) { tdStatus("Could not update.", true); }
    });
    const anyone = document.getElementById("tdAnyone");
    if (anyone) anyone.addEventListener("change", async () => {
      try { await fetch(`/depot-broker?player=${encodeURIComponent(player)}&id=${tdSelId}&anyone=${anyone.checked ? 1 : 0}&t=${Date.now()}`, { method: "POST", cache: "no-store" }); tdStatus("Depot flag updated.", false); }
      catch (_) { tdStatus("Could not update.", true); }
    });
    clientPanel.querySelectorAll("[data-td-mark]").forEach(btn => btn.addEventListener("click", async e => {
      e.preventDefault(); e.stopPropagation();
      try {
        const r = await fetch(`/depot-mark?player=${encodeURIComponent(player)}&id=${tdSelId}&item=${btn.dataset.tdMark}&on=${btn.dataset.tdOn}&t=${Date.now()}`, { method: "POST", cache: "no-store" });
        const res = await r.json().catch(() => ({}));
        if (!r.ok || res.ok === false) throw new Error(res.error || "mark failed");
        await refreshTradeDepot();
        tdStatus(btn.dataset.tdOn === "1" ? "Marked for trade." : "Unmarked.", false);
      } catch (err) { tdStatus(err.message || "Could not update.", true); }
    }));
  }
