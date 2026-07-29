// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

(function () {
  "use strict";

  const idleState = () => ({ mode: "idle", points: {}, primary: null });
  const pointValues = state => Object.values(state.points);
  const distance = (a, b) => Math.hypot(a.x - b.x, a.y - b.y);
  const midpoint = (a, b) => ({ x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 });

  function reduceGesture(state, event, config = {}) {
    const next = {
      ...state,
      points: Object.fromEntries(Object.entries(state.points || {}).map(([id, point]) =>
        [id, { ...point }]))
    };
    const effects = [];
    const id = String(event.id);
    const tapSlop = Number(config.tapSlop) || 10;
    const cellX = Math.max(1, Number(config.cellX) || 24);
    const cellY = Math.max(1, Number(config.cellY) || 24);

    if (event.type === "down") {
      if (event.placement) return {
        state: { mode: "placement-pass-through", points: {}, primary: id }, effects
      };
      next.points[id] = { x: event.x, y: event.y, downX: event.x, downY: event.y,
        anchorX: event.x, anchorY: event.y };
      const points = pointValues(next);
      if (points.length === 1) {
        next.mode = "tap-candidate";
        next.primary = id;
      } else if (points.length === 2) {
        const [a, b] = points;
        next.mode = "pinch-preview";
        next.primary = null;
        next.startDistance = Math.max(1, distance(a, b));
        next.lastDistance = next.startDistance;
        next.startMid = midpoint(a, b);
        next.lastMid = next.startMid;
        next.intent = "undecided";
        next.zRemainder = 0;
      }
      return { state: next, effects };
    }

    if (state.mode === "placement-pass-through") {
      if (event.type === "up" || event.type === "cancel") return { state: idleState(), effects };
      return { state, effects };
    }

    if (event.type === "cancel") {
      effects.push({ type: "cancel" });
      return { state: { ...idleState(), mode: "cancel-recovery" }, effects };
    }

    if (event.type === "move" && next.points[id]) {
      const point = next.points[id];
      point.x = event.x;
      point.y = event.y;
      const points = pointValues(next);

      if ((next.mode === "pinch-preview" || next.mode === "elevation-swipe") && points.length >= 2) {
        const [a, b] = points;
        const dist = Math.max(1, distance(a, b));
        const mid = midpoint(a, b);
        const pinchDelta = Math.abs(dist - next.startDistance);
        const verticalDelta = Math.abs(mid.y - next.startMid.y);
        if (next.intent === "undecided") {
          if (pinchDelta > 20 && pinchDelta > verticalDelta * 1.2) next.intent = "pinch";
          else if (verticalDelta > 28 && verticalDelta > pinchDelta * 1.15) {
            next.intent = "elevation";
            next.mode = "elevation-swipe";
          }
        }
        if (next.intent === "pinch") {
          effects.push({ type: "pinch-preview", scale: dist / next.startDistance });
          next.lastDistance = dist;
        } else if (next.intent === "elevation") {
          next.zRemainder += next.lastMid.y - mid.y;
          const steps = Math.trunc(next.zRemainder / 52);
          if (steps) {
            effects.push({ type: "elevation", steps });
            next.zRemainder -= steps * 52;
          }
        }
        next.lastMid = mid;
        return { state: next, effects };
      }

      if ((next.mode === "tap-candidate" || next.mode === "pan" ||
           next.mode === "cancel-recovery") && next.primary === id) {
        const total = Math.hypot(point.x - point.downX, point.y - point.downY);
        if (next.mode !== "pan" && total > tapSlop) next.mode = "pan";
        if (next.mode === "cancel-recovery") next.mode = "pan";
        if (next.mode === "pan") {
          const dx = Math.trunc((point.anchorX - point.x) / cellX);
          const dy = Math.trunc((point.anchorY - point.y) / cellY);
          if (dx || dy) {
            effects.push({ type: "pan", dx, dy });
            point.anchorX -= dx * cellX;
            point.anchorY -= dy * cellY;
          }
        }
      }
      return { state: next, effects };
    }

    if (event.type === "up" && next.points[id]) {
      const priorMode = next.mode;
      const released = next.points[id];
      delete next.points[id];
      const remaining = Object.entries(next.points);
      if (priorMode === "pinch-preview" && next.intent === "pinch") {
        const scale = Math.max(.35, Math.min(3, next.lastDistance / next.startDistance));
        const steps = Math.max(-3, Math.min(3, Math.round(Math.log(scale) / Math.log(1.18))));
        if (steps) effects.push({ type: "zoom", steps });
        effects.push({ type: "pinch-end" });
      } else if (priorMode === "elevation-swipe") {
        effects.push({ type: "pinch-end" });
      } else if (priorMode === "tap-candidate") {
        const moved = Math.hypot(released.x - released.downX, released.y - released.downY);
        if (moved <= tapSlop) effects.push({ type: "tap", x: event.x, y: event.y });
      }
      if (remaining.length) {
        const [remainingId, point] = remaining[0];
        point.downX = point.anchorX = point.x;
        point.downY = point.anchorY = point.y;
        next.mode = "cancel-recovery";
        next.primary = remainingId;
        delete next.intent;
        return { state: next, effects };
      }
      return { state: idleState(), effects };
    }

    return { state: next, effects };
  }

  function cellSize() {
    const grid = typeof captureTileGrid === "function" ? captureTileGrid() : null;
    if (grid) {
      const x = (grid.naturalX(1) - grid.naturalX(0)) * grid.rendered.scale;
      const y = (grid.naturalY(1) - grid.naturalY(0)) * grid.rendered.scale;
      if (x > 0 && y > 0) return { x, y };
    }
    return { x: 24, y: 24 };
  }

  function placementArmed() {
    try {
      if (window.DFPlacementArmed) return !!window.DFPlacementArmed();
      return typeof placementActive === "function" && !!placementActive();
    } catch (_) { return false; }
  }

  function gestureHint() {
    let hint = document.getElementById("touchGestureHint");
    if (!hint) {
      hint = document.createElement("div");
      hint.id = "touchGestureHint";
      hint.setAttribute("aria-live", "polite");
      document.body.appendChild(hint);
    }
    return hint;
  }

  function showPinch(scale) {
    const hint = gestureHint();
    hint.textContent = `Zoom ${scale.toFixed(2)}x`;
    hint.classList.add("visible");
  }

  function hidePinch() {
    document.getElementById("touchGestureHint")?.classList.remove("visible");
  }

  function bindTouch() {
    const map = document.getElementById("view");
    if (!map) return;
    map.style.touchAction = "none";
    let gesture = idleState();

    const dispatch = (type, event) => {
      const size = cellSize();
      const result = reduceGesture(gesture, {
        type, id: event.pointerId, x: event.clientX, y: event.clientY,
        placement: type === "down" && placementArmed(),
      }, { cellX: size.x, cellY: size.y });
      gesture = result.state;
      result.effects.forEach(effect => {
        if (effect.type === "pan") queueMove(effect.dx, effect.dy, 0);
        else if (effect.type === "elevation") queueMove(0, 0, effect.steps);
        else if (effect.type === "pinch-preview") showPinch(effect.scale);
        else if (effect.type === "pinch-end" || effect.type === "cancel") hidePinch();
        else if (effect.type === "zoom") {
          const action = effect.steps > 0 ? "in" : "out";
          for (let i = 0; i < Math.abs(effect.steps); ++i) sendZoom(action);
        }
        // Tap is intentionally not synthesized: the map's ordinary pointerup path receives the same
        // real PointerEvent and performs its normal shared hit-test/inspect behavior.
      });
      return result;
    };

    map.addEventListener("pointerdown", event => {
      if (event.pointerType !== "touch") return;
      const result = dispatch("down", event);
      if (result.state.mode === "placement-pass-through") return;
      if (result.state.mode === "pinch-preview") {
        window.dfcCancelMapPointerGesture?.();
        event.stopPropagation();
      }
      try { map.setPointerCapture(event.pointerId); } catch (_) {}
    }, { capture: true });
    map.addEventListener("pointermove", event => {
      if (event.pointerType !== "touch" || !gesture.points?.[String(event.pointerId)]) return;
      const result = dispatch("move", event);
      if (["pan", "pinch-preview", "elevation-swipe"].includes(result.state.mode))
        event.preventDefault();
    }, { passive: false });
    map.addEventListener("pointerup", event => {
      if (event.pointerType !== "touch") return;
      dispatch("up", event);
      try { map.releasePointerCapture(event.pointerId); } catch (_) {}
    });
    map.addEventListener("pointercancel", event => {
      if (event.pointerType !== "touch") return;
      dispatch("cancel", event);
      try { map.releasePointerCapture(event.pointerId); } catch (_) {}
    });
    map.addEventListener("lostpointercapture", event => {
      if (event.pointerType === "touch" && gesture.points?.[String(event.pointerId)])
        dispatch("cancel", event);
    });

    if (window.visualViewport) {
      const syncKeyboardInset = () => {
        const inset = Math.max(0, Math.round(
          innerHeight - visualViewport.height - visualViewport.offsetTop));
        document.documentElement.style.setProperty("--df-keyboard-inset", `${inset}px`);
        window.recoverDfcapturePanels?.();
      };
      visualViewport.addEventListener("resize", syncKeyboardInset);
      visualViewport.addEventListener("scroll", syncKeyboardInset);
      syncKeyboardInset();
    }
    addEventListener("orientationchange", () => {
      gesture = idleState();
      hidePinch();
      setTimeout(() => window.recoverDfcapturePanels?.(), 50);
    });
  }

  window.dfTouchGesture = Object.freeze({ reduceGesture, idleState });
  addEventListener("DOMContentLoaded", bindTouch);
})();
