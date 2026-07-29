// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

(function () {
  "use strict";

  const { esc, jsonFetch, openWindow } = window.dfDomainUi;
  let snapshot = null;
  let search = "";
  let ownerFilter = "all";
  let activeSave = "";
  let favoriteIds = new Set();
  let favoriteDetails = new Map();
  let favoriteHudEnabled = false;
  let favoriteRefreshPending = false;
  const MAX_FAVORITES = 5;

  function storagePlayer() {
    return String(window.dfStablePlayerId || window.player || "player");
  }

  function favoritesKey(save = activeSave) {
    return `dfcapture.favoriteDwarves.${storagePlayer()}.${save || "unknown"}`;
  }

  function hudKey() {
    return `dfcapture.favoriteHud.${storagePlayer()}`;
  }

  function readStoredFavorites(save) {
    try {
      const values = JSON.parse(localStorage.getItem(favoritesKey(save)) || "[]");
      return new Set((Array.isArray(values) ? values : [])
        .map(Number).filter(Number.isFinite).slice(0, MAX_FAVORITES));
    } catch (_) { return new Set(); }
  }

  function storeFavorites() {
    try { localStorage.setItem(favoritesKey(), JSON.stringify([...favoriteIds])); } catch (_) {}
  }

  function storeHudPreference() {
    try { localStorage.setItem(hudKey(), favoriteHudEnabled ? "1" : "0"); } catch (_) {}
  }

  function colorFor(id) {
    let hash = 2166136261;
    for (const ch of String(id || "unowned")) {
      hash ^= ch.charCodeAt(0);
      hash = Math.imul(hash, 16777619);
    }
    return `hsl(${Math.abs(hash) % 360} 68% 62%)`;
  }

  function playerName(id, fallback = "") {
    return snapshot?.players?.find(row => String(row.playerId) === String(id))?.name ||
      fallback || id || "Unowned";
  }

  function chip(owner, options = {}) {
    const id = owner?.playerId || owner?.player_id || "";
    if (!id) return `<span class="ownership-chip unowned">Unowned</span>`;
    const name = playerName(id, owner?.playerName || owner?.player_name);
    const presence = owner?.online ?? snapshot?.players?.find(row => row.playerId === id)?.online;
    const state = presence === true ? "online" : (presence === false ? "offline" : "");
    const title = options.title ||
      `Dwarf owned by ${name}${state ? `; ${state}` : ""} (advisory multiplayer ownership)`;
    return `<span class="ownership-chip" style="--owner-color:${esc(colorFor(id))}"
      title="${esc(title)}">${state ? `<i class="${state}"></i>` : ""}${esc(name)}</span>`;
  }

  function unitChip(unit) {
    return chip(unit?.ownership || null);
  }

  async function load() {
    snapshot = await jsonFetch(`/ownership?t=${Date.now()}`);
    const nextSave = String(snapshot.saveDir || "");
    if (nextSave !== activeSave) {
      activeSave = nextSave;
      favoriteIds = readStoredFavorites(activeSave);
      favoriteDetails.clear();
    }
    return snapshot;
  }

  function starButton(unitId, label) {
    const favorite = favoriteIds.has(Number(unitId));
    return `<button type="button" class="ownership-star${favorite ? " active" : ""}"
      data-owner-star="${Number(unitId)}" aria-pressed="${favorite ? "true" : "false"}"
      title="${favorite ? "Remove from favorite HUDs" : `Favorite ${esc(label || "dwarf")}`}">
      ${favorite ? "&#9733;" : "&#9734;"}
    </button>`;
  }

  function toggleFavorite(unitId) {
    const id = Number(unitId);
    if (!Number.isFinite(id)) return false;
    if (favoriteIds.has(id)) {
      favoriteIds.delete(id);
      favoriteDetails.delete(id);
    } else {
      if (favoriteIds.size >= MAX_FAVORITES) {
        alert(`You can favorite up to ${MAX_FAVORITES} dwarves.`);
        return false;
      }
      favoriteIds.add(id);
    }
    storeFavorites();
    renderFavoriteHud();
    refreshFavoriteHud();
    return true;
  }

  function healthView(unit) {
    const lines = Array.isArray(unit?.statusLines) ? unit.statusLines.filter(Boolean) : [];
    const problems = lines.filter(line => String(line).toLowerCase() !== "healthy");
    const healthy = !problems.length &&
      (!unit?.bodySummary || /no health problems/i.test(unit.bodySummary));
    const percent = healthy ? 100 : Math.max(15, 100 - problems.length * 22);
    return {
      percent,
      label: healthy ? "Healthy" : (problems[0] || unit?.bodySummary || "Health issue")
    };
  }

  function portrait(unit) {
    return typeof unitPortraitMarkup === "function"
      ? unitPortraitMarkup(unit, "favorite-unit-portrait")
      : `<div class="favorite-unit-portrait"><div class="portrait-glyph">${esc(String(unit?.name || "?").slice(0, 1))}</div></div>`;
  }

  function renderFavoriteHud() {
    let root = document.getElementById("favoriteDwarfHud");
    if (!root) {
      root = document.createElement("div");
      root.id = "favoriteDwarfHud";
      document.body.appendChild(root);
    }
    if (!favoriteHudEnabled || !favoriteIds.size) {
      root.classList.remove("visible");
      root.innerHTML = "";
      return;
    }
    root.classList.add("visible");
    root.innerHTML = [...favoriteIds].map(id => {
      const data = favoriteDetails.get(id);
      const known = snapshot?.units?.find(unit => Number(unit.unitId) === id);
      const unit = data?.unit || known || { id, name: `Dwarf #${id}` };
      const health = data?.unit ? healthView(unit) : { percent: 0, label: "Loading..." };
      const job = typeof unit.currentJob === "string"
        ? unit.currentJob : (unit.currentJob?.name || known?.currentJob?.name || "No current job");
      return `<div class="favorite-dwarf-card" data-favorite-open="${id}">
        ${portrait(unit)}
        <div class="favorite-dwarf-body">
          <div class="favorite-dwarf-name">${esc(unit.name || `Dwarf #${id}`)}</div>
          <div class="favorite-health-track" title="${esc(health.label)}">
            <span style="width:${health.percent}%"></span>
          </div>
          <div class="favorite-dwarf-status">${esc(health.label)}</div>
          <div class="favorite-dwarf-job">${esc(job)}</div>
        </div>
        ${starButton(id, unit.name)}
      </div>`;
    }).join("");
    root.querySelectorAll("[data-owner-star]").forEach(button =>
      button.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        toggleFavorite(button.dataset.ownerStar);
      }));
    root.querySelectorAll("[data-favorite-open]").forEach(card =>
      card.addEventListener("click", async () => {
        const id = Number(card.dataset.favoriteOpen);
        try {
          const data = await jsonFetch(`/unit?id=${id}&t=${Date.now()}`);
          if (typeof showUnitSheet === "function") showUnitSheet(data);
        } catch (_) {}
      }));
  }

  async function refreshFavoriteHud() {
    if (!favoriteHudEnabled || !favoriteIds.size || favoriteRefreshPending) return;
    favoriteRefreshPending = true;
    try {
      for (const id of favoriteIds) {
        try {
          const data = await jsonFetch(`/unit?id=${id}&t=${Date.now()}`);
          favoriteDetails.set(id, data);
          renderFavoriteHud();
        } catch (_) {
          favoriteDetails.delete(id);
        }
      }
    } finally {
      favoriteRefreshPending = false;
    }
  }

  function playerOptions(selected) {
    return (snapshot?.players || [])
      .slice()
      .sort((a, b) => String(a.name).localeCompare(String(b.name)))
      .map(row => `<option value="${esc(row.playerId)}"${row.playerId === selected ? " selected" : ""}>
        ${esc(row.name || row.playerId)}${row.online ? " (online)" : ""}
      </option>`).join("");
  }

  function visibleUnits() {
    const needle = search.trim().toLowerCase();
    return (snapshot?.units || []).filter(unit => {
      const owner = unit.owner?.playerId || "";
      if (ownerFilter === "unowned" && owner) return false;
      if (ownerFilter !== "all" && ownerFilter !== "unowned" && owner !== ownerFilter) return false;
      if (!needle) return true;
      return [unit.name, unit.profession, unit.currentJob?.name, playerName(owner)]
        .some(value => String(value || "").toLowerCase().includes(needle));
    });
  }

  function schedulerMarkup() {
    const scheduler = snapshot?.scheduler || {};
    const enabled = scheduler.enabled === true;
    const decisions = Array.isArray(scheduler.recentDecisions)
      ? scheduler.recentDecisions.slice(0, 12) : [];
    const decisionRows = decisions.map(row => {
      const who = row.unitName || (row.unitId >= 0 ? `Dwarf #${row.unitId}` : "");
      const job = row.jobName || (row.jobId >= 0 ? `Job #${row.jobId}` : "");
      const player = row.playerName || row.playerId || "";
      const subject = [who, job, player ? `for ${player}` : ""].filter(Boolean).join(" · ");
      const timestamp = Number(row.timestamp)
        ? new Date(Number(row.timestamp)).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })
        : "";
      return `<div class="ownership-scheduler-decision ${esc(row.outcome || "skipped")}">
        <div><strong>${esc(row.outcome || "checked")}</strong>
          ${subject ? `<span>${esc(subject)}</span>` : ""}
          ${timestamp ? `<time>${esc(timestamp)}</time>` : ""}
        </div>
        <small>${esc(row.reason || "")}</small>
      </div>`;
    }).join("");
    const autoDisabled = scheduler.autoDisabled
      ? `<div class="ownership-scheduler-warning">
          Preference was switched off after ${Number(scheduler.consecutiveFailures) || 0}
          consecutive early cancellations. Review the entries below before enabling it again.
        </div>` : "";
    return `<section class="ownership-scheduler">
      <div class="ownership-scheduler-heading">
        <div>
          <strong>Owned-dwarf task preference</strong>
          <span class="${enabled ? "on" : "off"}">${enabled ? "On" : "Off"}</span>
        </div>
        <div class="ownership-scheduler-actions">
          <button type="button" id="ownershipSchedulerRefresh">Refresh</button>
          ${snapshot.host ? `<button type="button" id="ownershipSchedulerToggle"
            class="${enabled ? "on" : ""}">
            ${enabled ? "Disable preference" : "Prefer owned dwarves for their player's tasks"}
          </button>` : ""}
        </div>
      </div>
      <div class="ownership-scheduler-status">${esc(scheduler.status || "Off")}</div>
      <small class="ownership-scheduler-scope">${esc(scheduler.scope || "")}</small>
      ${autoDisabled}
      <details class="ownership-scheduler-log"${decisions.length ? " open" : ""}>
        <summary>Recent assignment decisions${decisions.length ? ` (${decisions.length})` : ""}</summary>
        ${decisionRows || `<div class="info-message">No assignment decisions yet.</div>`}
      </details>
    </section>`;
  }

  function render() {
    if (!snapshot) return;
    const counts = snapshot.analytics?.ownedByPlayer || {};
    const playerCards = (snapshot.players || []).map(row => `
      <div class="ownership-player">
        ${chip({ playerId: row.playerId, playerName: row.name })}
        <span>${Number(counts[row.playerId]) || 0} dwarf${Number(counts[row.playerId]) === 1 ? "" : "s"}</span>
        <small>${row.online ? "online" : "offline"}</small>
      </div>`).join("") || `<div class="info-message">No browser players have checked in yet.</div>`;
    const filterOptions = (snapshot.players || []).map(row =>
      `<option value="${esc(row.playerId)}"${ownerFilter === row.playerId ? " selected" : ""}>${esc(row.name)}</option>`
    ).join("");
    const rows = visibleUnits().map(unit => {
      const owner = unit.owner?.playerId || "";
      const orderActor = unit.currentJob?.orderActorPlayerId || "";
      const alignment = orderActor && owner
        ? (unit.currentJob.ownerAligned
          ? `<span class="ownership-alignment aligned">order requested by owner</span>`
          : `<span class="ownership-alignment mismatch">order requested by ${esc(playerName(orderActor))}</span>`)
        : "";
      const controls = snapshot.host ? `
        <div class="ownership-controls">
          <select data-owner-select="${unit.unitId}" aria-label="Owner for ${esc(unit.name)}">
            <option value="">Unowned</option>${playerOptions(owner)}
          </select>
          <input data-owner-notes="${unit.unitId}" maxlength="128"
            value="${esc(unit.owner?.notes || "")}" placeholder="Role or note (optional)">
          <button type="button" data-owner-save="${unit.unitId}">Save</button>
        </div>` : "";
      return `<div class="ownership-unit-row" data-owner-unit="${unit.unitId}">
        <div class="ownership-unit-main">
          ${starButton(unit.unitId, unit.name)}
          <strong>${esc(unit.name)}</strong> ${chip(unit.owner)}
          <span>${esc(unit.profession || "")}</span>
        </div>
        <div class="ownership-job">${esc(unit.currentJob?.name || "No current job")} ${alignment}</div>
        ${controls}
      </div>`;
    }).join("") || `<div class="info-message">No citizens match these filters.</div>`;
    const historical = (snapshot.historical || []).length
      ? `<details class="ownership-history"><summary>${snapshot.historical.length} missing or deceased owned dwarf${snapshot.historical.length === 1 ? "" : "s"} retained for history</summary>
          ${(snapshot.historical || []).map(row => `<div>${esc(`Unit #${row.unitId}`)} ${chip(row)}</div>`).join("")}
        </details>` : "";
    const a = snapshot.analytics || {};
    openWindow("Player-owned dwarves", `
      ${schedulerMarkup()}
      <div class="ownership-players">${playerCards}</div>
      <div class="ownership-summary">
        <span>${Number(a.activeCitizens) || 0} citizens</span>
        <span>${Number(a.unownedCitizens) || 0} unowned</span>
        <span>${Number(a.alignedActiveOrders) || 0} owner-aligned active orders</span>
        <span>${Number(a.mismatchedActiveOrders) || 0} cross-player active orders</span>
      </div>
      <div class="ownership-favorite-controls">
        <button type="button" id="ownershipHudToggle" class="${favoriteHudEnabled ? "on" : ""}">
          Favorite HUDs: ${favoriteHudEnabled ? "On" : "Off"}
        </button>
        <span>${favoriteIds.size}/${MAX_FAVORITES} favorites</span>
      </div>
      <div class="ownership-filters">
        <input id="ownershipSearch" value="${esc(search)}" placeholder="Search dwarves, jobs, players">
        <select id="ownershipFilter">
          <option value="all"${ownerFilter === "all" ? " selected" : ""}>All owners</option>
          <option value="unowned"${ownerFilter === "unowned" ? " selected" : ""}>Unowned</option>
          ${filterOptions}
        </select>
      </div>
      <div class="ownership-unit-list">${rows}</div>
      ${historical}`,
      snapshot.host
        ? "Assignments persist with this fortress save."
        : "Only the host can assign, transfer, or clear dwarf ownership.");
    bind();
  }

  function bind() {
    document.getElementById("ownershipSchedulerRefresh")?.addEventListener("click", async event => {
      const button = event.currentTarget;
      button.disabled = true;
      try {
        await load();
        render();
      } catch (error) {
        button.disabled = false;
        alert(error.message);
      }
    });
    document.getElementById("ownershipSchedulerToggle")?.addEventListener("click", async event => {
      const button = event.currentTarget;
      const enabled = snapshot?.scheduler?.enabled === true;
      button.disabled = true;
      try {
        const params = new URLSearchParams({
          action: "scheduler-toggle",
          enabled: enabled ? "false" : "true"
        });
        await jsonFetch("/ownership-action", {
          method: "POST",
          headers: { "Content-Type": "application/x-www-form-urlencoded" },
          body: params.toString()
        });
        await load();
        render();
      } catch (error) {
        button.disabled = false;
        alert(error.message);
      }
    });
    document.getElementById("ownershipHudToggle")?.addEventListener("click", () => {
      favoriteHudEnabled = !favoriteHudEnabled;
      storeHudPreference();
      render();
      renderFavoriteHud();
      refreshFavoriteHud();
    });
    clientPanel.querySelectorAll("[data-owner-star]").forEach(button => {
      button.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        if (toggleFavorite(button.dataset.ownerStar)) render();
      });
    });
    document.getElementById("ownershipSearch")?.addEventListener("input", event => {
      search = event.target.value || "";
      render();
      const input = document.getElementById("ownershipSearch");
      input?.focus();
      input?.setSelectionRange(search.length, search.length);
    });
    document.getElementById("ownershipFilter")?.addEventListener("change", event => {
      ownerFilter = event.target.value || "all";
      render();
    });
    clientPanel.querySelectorAll("[data-owner-save]").forEach(button => {
      button.addEventListener("click", async () => {
        const unitId = Number(button.dataset.ownerSave);
        const select = clientPanel.querySelector(`[data-owner-select="${unitId}"]`);
        const notes = clientPanel.querySelector(`[data-owner-notes="${unitId}"]`);
        const owner = String(select?.value || "");
        const previous = snapshot.units.find(unit => Number(unit.unitId) === unitId)?.owner?.playerId || "";
        if (!owner && !previous) return;
        const params = new URLSearchParams({
          action: owner ? (previous && previous !== owner ? "transfer" : "assign") : "clear",
          unit: String(unitId),
          notes: String(notes?.value || "")
        });
        if (owner) {
          params.set("owner", owner);
          params.set("ownerName", playerName(owner));
        }
        button.disabled = true;
        try {
          await jsonFetch("/ownership-action", {
            method: "POST",
            headers: { "Content-Type": "application/x-www-form-urlencoded" },
            body: params.toString()
          });
          await load();
          render();
          window.dfAttribution?.invalidate?.(document);
        } catch (error) {
          button.disabled = false;
          button.title = error.message;
          alert(error.message);
        }
      });
    });
  }

  async function openOwnershipPanel() {
    openWindow("Player-owned dwarves", `<div class="info-message">Loading...</div>`);
    try {
      await load();
      render();
    } catch (error) {
      openWindow("Player-owned dwarves", `<div class="info-message">${esc(error.message)}</div>`);
    }
  }

  window.dfOwnership = Object.freeze({ load, open: openOwnershipPanel, chip, unitChip });
  window.openOwnershipPanel = openOwnershipPanel;
  addEventListener("DOMContentLoaded", () => {
    document.getElementById("ownershipTopBtn")?.addEventListener("click", openOwnershipPanel);
    try { favoriteHudEnabled = localStorage.getItem(hudKey()) === "1"; } catch (_) {}
    if (favoriteHudEnabled) {
      window.setTimeout(async () => {
        try {
          await load();
          renderFavoriteHud();
          refreshFavoriteHud();
        } catch (_) {}
      }, 1200);
    }
  });
  window.setInterval(refreshFavoriteHud, 5000);
})();
