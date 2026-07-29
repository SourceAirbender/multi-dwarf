// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

(function () {
  "use strict";

  const esc = value => typeof escapeHtml === "function"
    ? escapeHtml(String(value ?? ""))
    : String(value ?? "").replace(/[&<>"']/g, ch => ({
        "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
      }[ch]));

  async function jsonFetch(path, options) {
    const response = await fetch(path, { cache: "no-store", ...(options || {}) });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false)
      throw new Error(data.error || data.err || `${path} failed`);
    return data;
  }

  function openWindow(title, body, footer = "") {
    if (typeof clearBuildPlacement === "function") clearBuildPlacement(false);
    clientPanel.className = "visible info-panel domain-info-panel";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="domain-window-head" data-panel-drag-handle>
          <strong>${esc(title)}</strong>
          <button type="button" class="sq-btn tiny" data-domain-close title="Close">X</button>
        </div>
        <div class="info-body domain-window-body" style="grid-template-columns:1fr;">${body}</div>
        ${footer ? `<div class="info-footer">${esc(footer)}</div>` : ""}
      </div>`;
    clientPanel.querySelector("[data-domain-close]")?.addEventListener("click", event => {
      event.preventDefault();
      clientPanel.className = "";
      clientPanel.innerHTML = "";
      if (typeof focusPage === "function") focusPage();
    });
  }

  window.dfDomainUi = Object.freeze({ esc, jsonFetch, openWindow });
})();
