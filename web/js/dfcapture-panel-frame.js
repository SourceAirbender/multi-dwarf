// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

(function () {
  "use strict";

  const panelIds = ["selection", "clientPanel"];
  const keyPrefix = "dfcapture-panel-geometry:";

  function storageKey(panel) {
    return keyPrefix + panel.id + ":" + (typeof player === "string" ? player : "player");
  }

  function clamp(value, min, max) {
    return Math.max(min, Math.min(max, value));
  }

  function readGeometry(panel) {
    try { return JSON.parse(localStorage.getItem(storageKey(panel)) || "null"); }
    catch (_) { return null; }
  }

  function saveGeometry(panel) {
    const rect = panel.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    try {
      localStorage.setItem(storageKey(panel), JSON.stringify({
        left: Math.round(rect.left), top: Math.round(rect.top),
        width: Math.round(rect.width), height: Math.round(rect.height)
      }));
    } catch (_) {}
  }

  function applyGeometry(panel) {
    if (!panel.classList.contains("visible")) return;
    const geometry = readGeometry(panel);
    if (!geometry) return;
    const maxWidth = Math.max(280, innerWidth - 8);
    const maxHeight = Math.max(180, innerHeight - 8);
    const width = clamp(Number(geometry.width) || 360, 280, maxWidth);
    const height = clamp(Number(geometry.height) || 320, 180, maxHeight);
    panel.style.left = `${clamp(Number(geometry.left) || 4, 4, innerWidth - width - 4)}px`;
    panel.style.top = `${clamp(Number(geometry.top) || 4, 4, innerHeight - height - 4)}px`;
    panel.style.right = "auto";
    panel.style.bottom = "auto";
    panel.style.width = `${width}px`;
    panel.style.height = `${height}px`;
    panel.style.maxHeight = "none";
  }

  function resetPanelGeometry() {
    panelIds.forEach(id => {
      const panel = document.getElementById(id);
      if (!panel) return;
      try { localStorage.removeItem(storageKey(panel)); } catch (_) {}
      ["left", "top", "right", "bottom", "width", "height", "maxHeight"].forEach(
        property => panel.style.removeProperty(property));
    });
  }

  function installPanel(panel) {
    panel.classList.add("df-movable-panel");
    let action = null;
    panel.addEventListener("pointerdown", event => {
      if (event.button !== 0 || !panel.classList.contains("visible")) return;
      const rect = panel.getBoundingClientRect();
      const resize = event.clientX >= rect.right - 14 || event.clientY >= rect.bottom - 14;
      const handle = event.target.closest("[data-panel-drag-handle], .info-top-tabs, .unit-sheet-header, .build-head");
      if (!resize && !handle) return;
      if (!resize && event.target.closest("button,input,select,textarea,a")) return;
      action = {
        resize, pointerId: event.pointerId,
        startX: event.clientX, startY: event.clientY,
        left: rect.left, top: rect.top, width: rect.width, height: rect.height
      };
      panel.style.left = `${rect.left}px`;
      panel.style.top = `${rect.top}px`;
      panel.style.right = "auto";
      panel.style.bottom = "auto";
      panel.style.width = `${rect.width}px`;
      panel.style.height = `${rect.height}px`;
      panel.style.maxHeight = "none";
      try { panel.setPointerCapture(event.pointerId); } catch (_) {}
      event.preventDefault();
    });
    panel.addEventListener("pointermove", event => {
      if (!action || action.pointerId !== event.pointerId) return;
      const dx = event.clientX - action.startX;
      const dy = event.clientY - action.startY;
      if (action.resize) {
        panel.style.width = `${clamp(action.width + dx, 280, innerWidth - action.left - 4)}px`;
        panel.style.height = `${clamp(action.height + dy, 180, innerHeight - action.top - 4)}px`;
      } else {
        panel.style.left = `${clamp(action.left + dx, 4, innerWidth - action.width - 4)}px`;
        panel.style.top = `${clamp(action.top + dy, 4, innerHeight - action.height - 4)}px`;
      }
      event.preventDefault();
    });
    const finish = event => {
      if (!action || action.pointerId !== event.pointerId) return;
      try { panel.releasePointerCapture(event.pointerId); } catch (_) {}
      action = null;
      saveGeometry(panel);
    };
    panel.addEventListener("pointerup", finish);
    panel.addEventListener("pointercancel", finish);
    new MutationObserver(() => {
      if (!panel.classList.contains("df-movable-panel")) {
        panel.classList.add("df-movable-panel");
        return;
      }
      applyGeometry(panel);
    }).observe(panel, {
      attributes: true, attributeFilter: ["class"]
    });
  }

  addEventListener("DOMContentLoaded", () => {
    panelIds.map(id => document.getElementById(id)).filter(Boolean).forEach(installPanel);
    document.getElementById("resetPanelLayoutRow")?.addEventListener("click", resetPanelGeometry);
  });
  addEventListener("resize", () => panelIds.forEach(id => {
    const panel = document.getElementById(id);
    if (panel) applyGeometry(panel);
  }));
  window.resetDfcapturePanelLayout = resetPanelGeometry;
})();
