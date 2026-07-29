// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

(function () {
  "use strict";

  const { esc, jsonFetch, openWindow } = window.dfDomainUi;
  const plural = { building: "buildings", stockpile: "stockpiles", zone: "zones", order: "orders" };
  let cache = null;
  let fetchedAt = 0;
  let pending = null;

  function deterministicColor(name) {
    let hash = 2166136261;
    for (const ch of String(name || "unknown")) {
      hash ^= ch.charCodeAt(0);
      hash = Math.imul(hash, 16777619);
    }
    return `hsl(${Math.abs(hash) % 360} 68% 62%)`;
  }

  function playerColor(name) {
    if (String(name) === String(window.player) && typeof myColor === "string") return myColor;
    const peers = typeof window.dfPresencePeers === "function" ? window.dfPresencePeers() : [];
    return peers.find(peer => String(peer.player || peer.name) === String(name))?.color ||
      deterministicColor(name);
  }

  async function load(force = false) {
    const now = Date.now();
    if (!force && cache && now - fetchedAt < 1500) return cache;
    if (!force && pending) return pending;
    pending = jsonFetch(`/attrib?t=${now}`).then(data => {
      cache = data || {};
      fetchedAt = Date.now();
      return cache;
    }).finally(() => { pending = null; });
    return pending;
  }

  function ownerFor(data, kind, id) {
    return data?.[plural[kind]]?.[String(id)] || "";
  }

  function chip(owner, label = owner) {
    if (!owner) return "";
    return `<span class="attrib-chip" style="--attrib-color:${esc(playerColor(owner))}"
      title="Browser-created by ${esc(label)}">${esc(label)}</span>`;
  }

  async function decorate(root = document, force = false) {
    let data;
    try { data = await load(force); } catch (_) { return; }
    const rows = [];
    if (root.matches?.("[data-attrib-kind][data-attrib-id]")) rows.push(root);
    root.querySelectorAll?.("[data-attrib-kind][data-attrib-id]").forEach(row => rows.push(row));
    rows.forEach(row => {
      const owner = ownerFor(data, row.dataset.attribKind, row.dataset.attribId);
      row.querySelector(":scope > .attrib-chip")?.remove();
      row.querySelector("[data-attrib-slot]")?.replaceChildren();
      if (!owner) return;
      const template = document.createElement("template");
      template.innerHTML = chip(owner, data?.players?.[owner] || owner).trim();
      const node = template.content.firstElementChild;
      const slot = row.querySelector("[data-attrib-slot]");
      if (slot) slot.appendChild(node);
      else row.prepend(node);
    });
  }

  function invalidate(root) {
    cache = null;
    fetchedAt = 0;
    return decorate(root || document, true);
  }

  function attributionRows(data) {
    const players = new Map();
    Object.values(plural).forEach(kind => {
      Object.entries(data?.[kind] || {}).forEach(([id, owner]) => {
        const name = String(owner || "unknown");
        if (!players.has(name)) players.set(name, { total: 0, ids: {} });
        const row = players.get(name);
        row.total++;
        (row.ids[kind] ||= []).push(id);
      });
    });
    return Array.from(players.entries()).sort((a, b) => b[1].total - a[1].total);
  }

  function eventRows(data) {
    const events = Array.isArray(data?.events) ? data.events.slice().reverse() : [];
    if (!events.length) return `<div class="info-message">No session activity events yet.</div>`;
    return events.slice(0, 30).map(event => `
      <div class="activity-event">
        ${chip(event.actorPlayerId, event.actorDisplayName || event.actorPlayerId)}
        <span>${esc(event.action || "changed")} ${esc(event.objectKind || "object")}
          ${Number(event.objectId) >= 0 ? `#${Number(event.objectId)}` : ""}</span>
      </div>`).join("");
  }

  function ownershipAnalytics(data) {
    if (!data) return "";
    const analytics = data.analytics || {};
    const names = new Map((data.players || []).map(row => [String(row.playerId), row.name]));
    const counts = Object.entries(analytics.ownedByPlayer || {})
      .sort((a, b) => Number(b[1]) - Number(a[1]))
      .map(([id, count]) => `
        <div class="domain-data-row">
          <strong>${window.dfOwnership?.chip({ playerId: id, playerName: names.get(id) }) || esc(names.get(id) || id)}</strong>
          <span>${Number(count)} owned dwarf${Number(count) === 1 ? "" : "s"}</span>
        </div>`).join("");
    return `
      <h3 class="domain-section-title">Dwarf ownership and active work</h3>
      <div class="ownership-summary">
        <span>${Number(analytics.activeCitizens) || 0} citizens</span>
        <span>${Number(analytics.unownedCitizens) || 0} unowned</span>
        <span>${Number(analytics.alignedActiveOrders) || 0} owner-aligned orders</span>
        <span>${Number(analytics.mismatchedActiveOrders) || 0} cross-player orders</span>
        <span>${Number(analytics.untrackedActiveOrders) || 0} native/untracked orders</span>
      </div>
      ${counts || `<div class="info-message">No player-owned dwarves yet.</div>`}`;
  }

  async function openAttributionPanel() {
    openWindow("Player activity", `<div class="info-message">Loading...</div>`);
    try {
      const [data, ownership] = await Promise.all([
        load(true),
        window.dfOwnership?.load?.().catch(() => null) || Promise.resolve(null)
      ]);
      const rows = attributionRows(data);
      const totals = rows.length ? rows.map(([name, stats]) => `
        <div class="domain-data-row">
          <strong>${chip(name, data?.players?.[name] || name)} ${stats.total} attributed object${stats.total === 1 ? "" : "s"}</strong>
          <span class="domain-detail">${Object.values(plural)
            .filter(kind => stats.ids[kind]?.length)
            .map(kind => `${kind}: ${stats.ids[kind].length}`).join(" | ")}</span>
        </div>`).join("") : `<div class="info-message">No attributed browser objects yet.</div>`;
      openWindow("Player activity", `
        <div class="activity-scope">Object activity is session-scoped; dwarf ownership persists with this fortress save.</div>
        <h3 class="domain-section-title">Attributed objects</h3>${totals}
        ${ownershipAnalytics(ownership)}
        <h3 class="domain-section-title">Recent actions</h3>${eventRows(data)}`,
        "Unknown attribution means native or untracked work; it never implies host ownership.");
    } catch (error) {
      openWindow("Player activity", `<div class="info-message">${esc(error.message)}</div>`);
    }
  }

  window.dfAttribution = Object.freeze({ load, decorate, invalidate, chip, ownerFor });
  window.openAttributionPanel = openAttributionPanel;
  addEventListener("DOMContentLoaded", () => {
    document.getElementById("openAnalyticsRow")?.addEventListener("click", openAttributionPanel);
  });
})();
