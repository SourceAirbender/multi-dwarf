// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

(function () {
  "use strict";

  const pointers = new Map();
  let single = null;
  let multi = null;
  const tapSlop = 10;

  function cellSize() {
    const grid = typeof captureTileGrid === "function" ? captureTileGrid() : null;
    if (grid) {
      const x = (grid.naturalX(1) - grid.naturalX(0)) * grid.rendered.scale;
      const y = (grid.naturalY(1) - grid.naturalY(0)) * grid.rendered.scale;
      if (x > 0 && y > 0) return { x, y };
    }
    return { x: 24, y: 24 };
  }

  function distance(a, b) { return Math.hypot(a.x - b.x, a.y - b.y); }
  function midpointY(a, b) { return (a.y + b.y) / 2; }

  function beginMulti() {
    const ids = Array.from(pointers.keys()).slice(0, 2);
    const a = pointers.get(ids[0]), b = pointers.get(ids[1]);
    multi = {
      ids, startDistance: distance(a, b), lastDistance: distance(a, b),
      lastMidY: midpointY(a, b), mode: null, zRemainder: 0
    };
    single = null;
  }

  function placementArmed() {
    try {
      if (window.DFPlacementArmed) return !!window.DFPlacementArmed();
      return typeof placementActive === "function" && !!placementActive();
    } catch (_) { return false; }
  }

  function bindTouch() {
    const map = document.getElementById("view");
    if (!map) return;
    map.style.touchAction = "none";

    map.addEventListener("pointerdown", event => {
      if (event.pointerType !== "touch") return;
      if (pointers.size === 0 && placementArmed()) return;
      pointers.set(event.pointerId, { x: event.clientX, y: event.clientY });
      if (pointers.size === 1) {
        single = {
          id: event.pointerId, downX: event.clientX, downY: event.clientY,
          anchorX: event.clientX, anchorY: event.clientY, moved: false
        };
      } else if (pointers.size === 2) {
        beginMulti();
      }
      try { map.setPointerCapture(event.pointerId); } catch (_) {}
    });

    map.addEventListener("pointermove", event => {
      if (event.pointerType !== "touch" || !pointers.has(event.pointerId)) return;
      const point = pointers.get(event.pointerId);
      point.x = event.clientX; point.y = event.clientY;
      if (multi) {
        const a = pointers.get(multi.ids[0]), b = pointers.get(multi.ids[1]);
        if (!a || !b) return;
        const dist = distance(a, b);
        const midY = midpointY(a, b);
        const pinchDelta = Math.abs(dist - multi.startDistance);
        const verticalDelta = Math.abs(midY - multi.lastMidY);
        if (!multi.mode && Math.max(pinchDelta, verticalDelta) > 24)
          multi.mode = pinchDelta >= verticalDelta * 1.25 ? "pinch" : "elevation";
        if (multi.mode === "pinch" && Math.abs(dist - multi.lastDistance) > 18) {
          sendZoom(dist > multi.lastDistance ? "in" : "out");
          multi.lastDistance = dist;
        } else if (multi.mode === "elevation") {
          multi.zRemainder += multi.lastMidY - midY;
          const steps = Math.trunc(multi.zRemainder / 52);
          if (steps) {
            queueMove(0, 0, steps);
            multi.zRemainder -= steps * 52;
          }
          multi.lastMidY = midY;
        }
        event.preventDefault();
        return;
      }
      if (!single || single.id !== event.pointerId) return;
      const size = cellSize();
      const dx = Math.round((single.anchorX - event.clientX) / size.x);
      const dy = Math.round((single.anchorY - event.clientY) / size.y);
      if (Math.hypot(event.clientX - single.downX, event.clientY - single.downY) > tapSlop)
        single.moved = true;
      if (dx || dy) {
        queueMove(dx, dy, 0);
        single.anchorX -= dx * size.x;
        single.anchorY -= dy * size.y;
      }
      if (single.moved) event.preventDefault();
    }, { passive: false });

    const finish = event => {
      if (event.pointerType !== "touch" || !pointers.has(event.pointerId)) return;
      pointers.delete(event.pointerId);
      if (multi?.ids.includes(event.pointerId)) {
        multi = null;
        const rest = Array.from(pointers.entries())[0];
        single = rest ? {
          id: rest[0], downX: rest[1].x, downY: rest[1].y,
          anchorX: rest[1].x, anchorY: rest[1].y, moved: true
        } : null;
      } else if (single?.id === event.pointerId) {
        single = null;
      }
      try { map.releasePointerCapture(event.pointerId); } catch (_) {}
    };
    map.addEventListener("pointerup", finish);
    map.addEventListener("pointercancel", finish);

    if (window.visualViewport) {
      const syncKeyboardInset = () => {
        const inset = Math.max(0, Math.round(
          innerHeight - visualViewport.height - visualViewport.offsetTop));
        document.documentElement.style.setProperty("--df-keyboard-inset", `${inset}px`);
      };
      visualViewport.addEventListener("resize", syncKeyboardInset);
      visualViewport.addEventListener("scroll", syncKeyboardInset);
      syncKeyboardInset();
    }
  }

  addEventListener("DOMContentLoaded", bindTouch);
})();
