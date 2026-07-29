// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

(function () {
  "use strict";

  const { esc, jsonFetch } = window.dfDomainUi;
  const maxUnseen = 160;

  function classify(report) {
    const text = String(report?.text || "");
    const key = String(report?.typeKey || "").toLowerCase();
    const section = String(report?.section || "misc");
    if (section === "deaths" || /\b(died|slain|deceased|has been found dead)\b/i.test(text)) return "Deaths";
    if (section === "artifacts" || /artifact/i.test(key + " " + text)) return "Artifacts";
    if (section === "combat" || section === "sieges") return "Conflict";
    if (section === "trade") return "Trade";
    if (section === "nobles" || /mandate|petition|appointed/i.test(text)) return "Fortress affairs";
    if (/migrant|arriv|visitor|caravan/i.test(text)) return "Arrivals";
    if (/strange mood|possessed|fey mood|fell mood|macabre mood/i.test(text)) return "Moods";
    return "Other";
  }

  function aggregateReports(reports, overflow = false) {
    const messages = (Array.isArray(reports) ? reports : []).filter(report => !report?.continuation);
    const counts = new Map();
    messages.forEach(report => {
      const group = classify(report);
      counts.set(group, (counts.get(group) || 0) + 1);
    });
    const summary = [...counts.entries()]
      .sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]))
      .slice(0, 6)
      .map(([label, count]) => `${count} ${label.toLowerCase()}`);
    const headlines = messages.slice(-4).reverse().map(report => String(report.text || "").trim())
      .filter(Boolean);
    return { count: messages.length, summary, headlines, overflow: !!overflow };
  }

  function cursorKey(world) {
    return `dfcapture.sinceYouLeft.v1.${encodeURIComponent(String(world || "unknown"))}.` +
      encodeURIComponent(String(window.player || "player"));
  }

  function readCursor(key) {
    try {
      const value = Number(localStorage.getItem(key));
      return Number.isInteger(value) && value >= -1 ? value : null;
    } catch (_) {
      return null;
    }
  }

  function writeCursor(key, value) {
    try { localStorage.setItem(key, String(Math.max(-1, Number(value) || 0))); }
    catch (_) {}
  }

  function renderDigest(digest) {
    document.getElementById("sinceYouLeftDigest")?.remove();
    const host = document.createElement("section");
    host.id = "sinceYouLeftDigest";
    host.setAttribute("aria-live", "polite");
    host.innerHTML = `
      <div class="digest-head"><strong>Since you left</strong>
        <button type="button" data-digest-close aria-label="Dismiss">X</button></div>
      <div class="digest-summary">${esc(digest.summary.join(" · ") ||
        `${digest.count} new fortress report${digest.count === 1 ? "" : "s"}`)}</div>
      ${digest.headlines.length ? `<ul>${digest.headlines.map(line => `<li>${esc(line)}</li>`).join("")}</ul>` : ""}
      ${digest.overflow ? `<div class="digest-overflow">There were more events than this bounded digest could include. Open Reports for the full history.</div>` : ""}`;
    host.querySelector("[data-digest-close]")?.addEventListener("click", () => host.remove());
    document.body.appendChild(host);
  }

  async function checkSinceYouLeft() {
    try {
      const attribution = await jsonFetch(`/attrib?t=${Date.now()}`);
      const world = attribution.world || "unknown";
      const key = cursorKey(world);
      const latest = await jsonFetch(`/reports?section=all&max=1&t=${Date.now()}`);
      const latestId = Math.max(-1, Number(latest.nextReportId) - 1);
      const prior = readCursor(key);
      if (prior == null) {
        writeCursor(key, latestId);
        return;
      }
      if (latestId <= prior) return;
      const page = await jsonFetch(
        `/reports?section=all&since=${prior}&max=${maxUnseen}&t=${Date.now()}`
      );
      const digest = aggregateReports(page.reports,
        page.truncated || page.budgetExhausted || Number(page.nextReportId) - 1 > latestId);
      writeCursor(key, Math.max(prior, Number(page.nextReportId) - 1));
      if (digest.count) renderDigest(digest);
    } catch (_) {
      // A digest is optional polish. A failed read never advances the cursor or blocks the fort.
    }
  }

  window.dfSinceYouLeft = Object.freeze({ aggregateReports, classify });
  addEventListener("DOMContentLoaded", () => setTimeout(checkSinceYouLeft, 800));
})();
