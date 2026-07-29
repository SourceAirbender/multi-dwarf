// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

(function () {
  "use strict";

  const registry = new Map();
  // v3 deliberately abandons the old shared geometry keys. v2 could reuse a
  // building-control panel's tiny dimensions for the build menu.
  const storagePrefix = "dfcapture.panel.v3:";
  const edgeGrabPx = 12;
  let zCounter = 80;

  const profiles = {
    selection: {
      default:          { minWidth: 300, minHeight: 180, width: 380, height: 420, anchor: "left-top" },
      building:         { minWidth: 340, minHeight: 220, width: 380, height: 400, anchor: "left-top" },
      "building-control": { minWidth: 420, minHeight: 280, width: 500, height: 560, anchor: "left-top" },
      "lever-link":     { minWidth: 460, minHeight: 340, width: 560, height: 620, anchor: "left-top" },
      cage:             { minWidth: 420, minHeight: 320, width: 500, height: 600, anchor: "left-top" },
      coffin:           { minWidth: 400, minHeight: 300, width: 480, height: 520, anchor: "left-top" },
      farm:             { minWidth: 440, minHeight: 340, width: 520, height: 600, anchor: "left-top" },
      workshop:         { minWidth: 600, minHeight: 380, width: 680, height: 700, anchor: "left-top" },
      zone:             { minWidth: 340, minHeight: 300, width: 400, height: 580, anchor: "left-top" },
      "zone-wide":      { minWidth: 540, minHeight: 340, width: 640, height: 620, anchor: "left-top" },
      stockpile:        { minWidth: 480, minHeight: 360, width: 560, height: 700, anchor: "left-top" },
      "stock-item":     { minWidth: 440, minHeight: 300, width: 520, height: 600, anchor: "left-top" },
      "unit-sheet":     { minWidth: 440, minHeight: 360, width: 540, height: 700, anchor: "left-top" },
      occupants:        { minWidth: 380, minHeight: 280, width: 460, height: 560, anchor: "left-top" },
      engraving:        { minWidth: 380, minHeight: 260, width: 480, height: 520, anchor: "left-top" },
      location:         { minWidth: 460, minHeight: 360, width: 560, height: 650, anchor: "left-top" },
    },
    clientPanel: {
      default:          { minWidth: 420, minHeight: 300, width: 760, height: 640, anchor: "center-top" },
      build:            { minWidth: 620, minHeight: 420, width: 720, height: 620, anchor: "left-bottom" },
      info:             { minWidth: 620, minHeight: 420, width: 980, height: 720, anchor: "center-top" },
      alerts:           { minWidth: 620, minHeight: 420, width: 900, height: 680, anchor: "center-top" },
    },
  };

  function clamp(value, min, max) {
    return Math.max(min, Math.min(max, value));
  }

  function viewportRect() {
    const vv = window.visualViewport;
    return {
      left: Math.round(vv?.offsetLeft || 0),
      top: Math.round(vv?.offsetTop || 0),
      width: Math.round(vv?.width || innerWidth),
      height: Math.round(vv?.height || innerHeight),
    };
  }

  function stablePlayerKey() {
    return String(window.dfStablePlayerId || window.player || "player");
  }

  function elementFor(config) {
    return config.getElement?.() || config.element || document.getElementById(config.id);
  }

  function isOpen(config, panel) {
    return config.isOpen ? !!config.isOpen(panel) : panel.classList.contains("visible");
  }

  function variantFor(config, panel) {
    if (typeof config.variant === "function") return String(config.variant(panel) || "default");
    if (config.variant) return String(config.variant);
    if (config.id === "clientPanel") {
      if (panel.classList.contains("build-panel")) return "build";
      if (panel.classList.contains("alerts-window")) return "alerts";
      if (panel.classList.contains("info-panel") &&
          typeof activeInfoPanel === "string" && activeInfoPanel)
        return `info:${activeInfoPanel}`;
      if (panel.classList.contains("info-panel")) return "info";
      return "default";
    }
    if (panel.classList.contains("lever-panel")) return "lever-link";
    if (panel.classList.contains("cage-panel")) return "cage";
    if (panel.classList.contains("coffin-panel")) return "coffin";
    if (panel.classList.contains("workshop-panel")) return "workshop";
    if (panel.classList.contains("farm-panel")) return "farm";
    if (panel.classList.contains("location-panel")) return "location";
    if (panel.classList.contains("zone-wide")) return "zone-wide";
    if (panel.classList.contains("zone-panel")) return "zone";
    if (panel.classList.contains("stockpile-panel")) return "stockpile";
    if (panel.classList.contains("stock-item-panel")) return "stock-item";
    if (panel.classList.contains("unit-sheet-panel")) return "unit-sheet";
    if (panel.classList.contains("occupant-panel")) return "occupants";
    if (panel.classList.contains("engraving-panel")) return "engraving";
    if (panel.classList.contains("building-control-panel")) return "building-control";
    if (panel.classList.contains("building-panel")) return "building";
    return "default";
  }

  function storageKey(config, panel) {
    return `${storagePrefix}${stablePlayerKey()}:${config.id}:${variantFor(config, panel)}`;
  }

  function readGeometry(config, panel) {
    try { return JSON.parse(localStorage.getItem(storageKey(config, panel)) || "null"); }
    catch (_) { return null; }
  }

  function saveGeometry(config, panel) {
    if (!isOpen(config, panel)) return;
    const rect = panel.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    try {
      localStorage.setItem(storageKey(config, panel), JSON.stringify({
        left: Math.round(rect.left), top: Math.round(rect.top),
        width: Math.round(rect.width), height: Math.round(rect.height),
      }));
      if (config.persistOpen)
        localStorage.setItem(`${storageKey(config, panel)}:open`, "1");
    } catch (_) {}
  }

  function profileFor(config, panel) {
    const variant = variantFor(config, panel);
    const group = profiles[config.id] || {};
    const baseVariant = variant.startsWith("info:") ? "info" : variant;
    return group[baseVariant] || group.default || {
      minWidth: 280, minHeight: 180, width: 380, height: 440, anchor: "right-top"
    };
  }

  function panelScale(panel) {
    const rect = panel.getBoundingClientRect();
    const width = panel.offsetWidth;
    const height = panel.offsetHeight;
    return {
      x: width > 0 && rect.width > 0 ? rect.width / width : 1,
      y: height > 0 && rect.height > 0 ? rect.height / height : 1,
    };
  }

  function minSize(config, panel) {
    const profile = profileFor(config, panel);
    const override = config.minSize || {};
    const scale = panelScale(panel);
    return {
      width: (Number(override.width) || profile.minWidth || 280) * scale.x,
      height: (Number(override.height) || profile.minHeight || 180) * scale.y,
    };
  }

  function defaultGeometry(config, panel, view) {
    const profile = profileFor(config, panel);
    const scale = panelScale(panel);
    const min = minSize(config, panel);
    const width = clamp((config.defaultWidth || profile.width || 380) * scale.x,
      Math.min(min.width, view.width - 8), view.width - 8);
    const height = clamp((config.defaultHeight || profile.height || 440) * scale.y,
      Math.min(min.height, view.height - 8), view.height - 8);
    const anchor = config.defaultAnchor || profile.anchor || "right-top";
    const left = anchor.includes("left") ? view.left + 4 :
      anchor.includes("center") ? view.left + (view.width - width) / 2 :
      view.left + view.width - width - 4;
    const top = anchor.includes("bottom") ? view.top + view.height - height - 4 : view.top + 4;
    return { left, top, width, height };
  }

  function normalizedGeometry(config, panel, geometry) {
    const view = viewportRect();
    const min = minSize(config, panel);
    const narrow = view.width < 700;
    const fallback = defaultGeometry(config, panel, view);
    const source = geometry || fallback;
    const availableWidth = Math.max(240, view.width - 8);
    const effectiveMinWidth = Math.min(min.width, availableWidth);
    const width = narrow ? availableWidth :
      clamp(Number(source.width) || fallback.width, effectiveMinWidth, availableWidth);
    const height = clamp(Number(source.height) || fallback.height,
      Math.min(min.height, view.height - 8), Math.max(120, view.height - 8));
    const left = narrow ? view.left + 4 :
      clamp(Number(source.left), view.left + 4, view.left + view.width - width - 4);
    const top = clamp(Number(source.top), view.top + 4, view.top + view.height - height - 4);
    return { left, top, width, height };
  }

  function applyGeometry(config, panel, geometry = readGeometry(config, panel)) {
    if (!isOpen(config, panel)) return;
    const value = normalizedGeometry(config, panel, geometry);
    const scale = panelScale(panel);
    Object.assign(panel.style, {
      left: `${value.left / scale.x}px`, top: `${value.top / scale.y}px`,
      right: "auto", bottom: "auto",
      width: `${value.width / scale.x}px`, height: `${value.height / scale.y}px`,
      maxHeight: "none",
    });
  }

  function focusPanel(config, panel) {
    if (!isOpen(config, panel)) return;
    panel.style.zIndex = String(++zCounter);
    panel.dataset.panelFocused = "true";
    registry.forEach(other => {
      const element = elementFor(other);
      if (element && element !== panel) delete element.dataset.panelFocused;
    });
    config.onFocus?.(panel);
  }

  function resizeEdgeAt(event, panel) {
    const rect = panel.getBoundingClientRect();
    if (!rect.width || !rect.height) return "";
    const grab = Math.min(edgeGrabPx, rect.width / 4, rect.height / 4);
    const west = event.clientX <= rect.left + grab;
    const east = event.clientX >= rect.right - grab;
    const north = event.clientY <= rect.top + grab;
    const south = event.clientY >= rect.bottom - grab;
    return `${north ? "n" : south ? "s" : ""}${west ? "w" : east ? "e" : ""}`;
  }

  function setResizeCursor(panel, edge) {
    if (!edge) {
      delete panel.dataset.resizeEdge;
      panel.style.removeProperty("cursor");
      return;
    }
    panel.dataset.resizeEdge = edge;
    panel.style.cursor = edge.length === 2
      ? (edge === "ne" || edge === "sw" ? "nesw-resize" : "nwse-resize")
      : (edge === "n" || edge === "s" ? "ns-resize" : "ew-resize");
  }

  function interactiveOrScrollable(target) {
    return !!target.closest(
      "button,input,select,textarea,a,label,[contenteditable=true]," +
      ".info-body,.unit-sheet-body,.ann-log,.wo-list,.occupant-list"
    );
  }

  function installElement(config, panel) {
    if (panel.dataset.panelRegistryInstalled === "true") return;
    panel.dataset.panelRegistryInstalled = "true";
    panel.classList.add("df-movable-panel");
    panel.tabIndex = panel.tabIndex >= 0 ? panel.tabIndex : -1;
    let action = null;

    panel.addEventListener("pointerdown", event => {
      if (event.button !== 0 || !isOpen(config, panel)) return;
      focusPanel(config, panel);
      const edge = resizeEdgeAt(event, panel);
      const dragSelector = (config.dragHandles || [
        "[data-panel-drag-handle]", ".info-top-tabs", ".unit-sheet-header", ".bld-head", ".sp-header"
      ]).join(",");
      const drag = !edge && event.target.closest(dragSelector);
      if (!edge && (!drag || interactiveOrScrollable(event.target))) return;
      const rect = panel.getBoundingClientRect();
      action = {
        pointerId: event.pointerId, edge,
        startX: event.clientX, startY: event.clientY,
        left: rect.left, top: rect.top, width: rect.width, height: rect.height,
      };
      applyGeometry(config, panel, action);
      try { panel.setPointerCapture(event.pointerId); } catch (_) {}
      event.preventDefault();
      event.stopPropagation();
    }, { capture: true });

    panel.addEventListener("pointermove", event => {
      if (!action || action.pointerId !== event.pointerId) {
        setResizeCursor(panel, isOpen(config, panel) ? resizeEdgeAt(event, panel) : "");
        return;
      }
      const view = viewportRect();
      const min = minSize(config, panel);
      const dx = event.clientX - action.startX;
      const dy = event.clientY - action.startY;
      let { left, top, width, height } = action;
      if (!action.edge) {
        left += dx;
        top += dy;
      } else {
        if (action.edge.includes("e")) width += dx;
        if (action.edge.includes("s")) height += dy;
        if (action.edge.includes("w")) { left += dx; width -= dx; }
        if (action.edge.includes("n")) { top += dy; height -= dy; }
      }
      width = clamp(width, min.width, view.width - 8);
      height = clamp(height, Math.min(min.height, view.height - 8), view.height - 8);
      left = clamp(left, view.left + 4, view.left + view.width - width - 4);
      top = clamp(top, view.top + 4, view.top + view.height - height - 4);
      const scale = panelScale(panel);
      Object.assign(panel.style, {
        left: `${left / scale.x}px`, top: `${top / scale.y}px`,
        width: `${width / scale.x}px`, height: `${height / scale.y}px`,
      });
      event.preventDefault();
    }, { passive: false });
    panel.addEventListener("pointerleave", () => {
      if (!action) setResizeCursor(panel, "");
    });

    const finish = event => {
      if (!action || action.pointerId !== event.pointerId) return;
      try { panel.releasePointerCapture(event.pointerId); } catch (_) {}
      action = null;
      setResizeCursor(panel, resizeEdgeAt(event, panel));
      saveGeometry(config, panel);
    };
    panel.addEventListener("pointerup", finish);
    panel.addEventListener("pointercancel", finish);
    panel.addEventListener("focusin", () => focusPanel(config, panel));

    let previouslyOpen = isOpen(config, panel);
    let previousVariant = variantFor(config, panel);
    new MutationObserver(() => {
      // Most panel renderers replace className wholesale. Restore our marker
      // only when it is actually absent: Chromium reports classList writes as
      // attribute mutations, and an unconditional add here can self-trigger
      // this observer forever when a panel is opened.
      if (!panel.classList.contains("df-movable-panel"))
        panel.classList.add("df-movable-panel");
      const open = isOpen(config, panel);
      const variant = variantFor(config, panel);
      if (open) {
        if (!previouslyOpen || variant !== previousVariant)
          applyGeometry(config, panel, readGeometry(config, panel));
        if (!previouslyOpen) focusPanel(config, panel);
      }
      previouslyOpen = open;
      previousVariant = variant;
    }).observe(panel, { attributes: true, attributeFilter: ["class"], childList: true });
  }

  function registerPanel(input) {
    if (!input?.id) throw new Error("panel registration requires an id");
    const config = { ...input };
    registry.set(config.id, config);
    const panel = elementFor(config);
    if (panel) installElement(config, panel);
    return () => registry.delete(config.id);
  }

  function closePanel(config, panel) {
    const closeButton = panel.querySelector(
      "[data-domain-close],[data-bld-close],[data-stock-item-close],[data-sp-close]," +
      "[data-occupants-close],[data-engraving-close],.unit-close-button"
    );
    if (closeButton) {
      closeButton.click();
    } else if (config.close) {
      config.close(panel);
    } else {
      panel.classList.remove("visible");
      panel.replaceChildren();
    }
    if (config.persistOpen) {
      try { localStorage.setItem(`${storageKey(config, panel)}:open`, "0"); } catch (_) {}
    }
  }

  function closeTopPopover() {
    const visible = [...document.querySelectorAll(
      "[role=menu].visible,.tool-group.visible,.popover.visible,#bmRow:not([hidden])"
    )].filter(element => element.offsetParent !== null);
    const top = visible.at(-1);
    if (!top) return false;
    top.classList.remove("visible");
    if (top.id === "bmRow") top.hidden = true;
    top.setAttribute("aria-hidden", "true");
    return true;
  }

  function resetPanelGeometry() {
    registry.forEach(config => {
      const panel = elementFor(config);
      if (!panel) return;
      const prefix = `${storagePrefix}${stablePlayerKey()}:${config.id}:`;
      try {
        for (let i = localStorage.length - 1; i >= 0; --i) {
          const key = localStorage.key(i);
          if (key?.startsWith(prefix)) localStorage.removeItem(key);
        }
      } catch (_) {}
      ["left", "top", "right", "bottom", "width", "height", "maxHeight"].forEach(
        property => panel.style.removeProperty(property));
      if (isOpen(config, panel)) applyGeometry(config, panel, null);
    });
  }

  function recoverAll() {
    registry.forEach(config => {
      const panel = elementFor(config);
      if (!panel) return;
      installElement(config, panel);
      applyGeometry(config, panel);
    });
  }

  addEventListener("DOMContentLoaded", () => {
    registerPanel({ id: "selection" });
    registerPanel({ id: "clientPanel" });
    document.getElementById("resetPanelLayoutRow")?.addEventListener("click", resetPanelGeometry);
    recoverAll();
  });
  addEventListener("resize", recoverAll);
  window.visualViewport?.addEventListener("resize", recoverAll);
  window.visualViewport?.addEventListener("scroll", recoverAll);
  document.addEventListener("keydown", event => {
    if (event.key !== "Escape" || event.defaultPrevented) return;
    if (closeTopPopover()) {
      event.preventDefault();
      return;
    }
    const open = [...registry.values()].map(config => ({ config, panel: elementFor(config) }))
      .filter(entry => entry.panel && isOpen(entry.config, entry.panel))
      .sort((a, b) => Number(a.panel.style.zIndex || 0) - Number(b.panel.style.zIndex || 0));
    const top = open.at(-1);
    if (top) {
      closePanel(top.config, top.panel);
      event.preventDefault();
    }
  });

  window.registerDfcapturePanel = registerPanel;
  window.resetDfcapturePanelLayout = resetPanelGeometry;
  window.recoverDfcapturePanels = recoverAll;
})();
