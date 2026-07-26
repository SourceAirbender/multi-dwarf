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

  // ---- Minimap extras: a z-scrollbar and camera bookmarks -------------------------------------
  // Both are pure frontend against the existing /camera route and the z-bounds already in /hud
  // (camera.z, map.z). Liquid-numeral and ramp-arrow toggles require a client-side tile renderer
  // and cannot change a server-rendered pixel frame.
  // Peer-follow is already handled by the presence roster (click a name to follow / unfollow).
  const BM_SLOTS = 5;
  const BM_STORE = "dfcap-camera-bookmarks";

  function bmLoad() { try { return JSON.parse(localStorage.getItem(BM_STORE) || "[]"); } catch (_) { return []; } }
  function bmStore(arr) { try { localStorage.setItem(BM_STORE, JSON.stringify(arr)); } catch (_) {} }

  function cameraGoto(x, y, z) {
    if (typeof resetPanPrediction === "function") resetPanPrediction();
    fetch(`/camera?player=${encodeURIComponent(player)}&x=${x}&y=${y}&z=${z}`, { method: "POST", cache: "no-store" })
      .then(() => { if (typeof loadHud === "function") loadHud(); }).catch(() => {});
  }

  function setCameraZ(z) {
    const cam = currentHud && currentHud.camera; if (!cam) return;
    const mz = (currentHud.map && currentHud.map.z) || 1;
    cameraGoto(cam.x, cam.y, Math.max(0, Math.min(mz - 1, Math.round(z))));
  }

  function renderBookmarks() {
    const row = document.getElementById("bmRow"); if (!row) return;
    const arr = bmLoad();
    row.innerHTML = Array.from({ length: BM_SLOTS }, (_, i) => {
      const bm = arr[i];
      return `<button class="bm-slot${bm ? " set" : ""}" data-bm="${i}" title="${bm
        ? `Jump to bookmark ${i + 1} — right-click clears`
        : `Save the current view to slot ${i + 1}`}">${i + 1}</button>`;
    }).join("");
    row.querySelectorAll("[data-bm]").forEach(b => {
      const i = Number(b.dataset.bm);
      b.addEventListener("click", e => {
        e.preventDefault(); e.stopPropagation();
        const a = bmLoad();
        if (a[i]) { cameraGoto(a[i].x, a[i].y, a[i].z); }
        else { const cam = currentHud && currentHud.camera; if (cam) { a[i] = { x: cam.x, y: cam.y, z: cam.z }; bmStore(a); renderBookmarks(); } }
        if (typeof focusPage === "function") focusPage();
      });
      b.addEventListener("contextmenu", e => {
        e.preventDefault(); e.stopPropagation();
        const a = bmLoad(); a[i] = null; bmStore(a); renderBookmarks();
      });
    });
  }

  let elevationBandKey = "";

  function zTopPercent(z, maxZ) {
    if (maxZ <= 0) return 0;
    return (1 - Math.max(0, Math.min(maxZ, Number(z) || 0)) / maxZ) * 100;
  }

  // DF's elevation bar is one native SCROLLBAR_* slice per world z-level. Rebuild the
  // terrain stack only when the world/surface geometry changes; camera and peer markers
  // can then move independently without churning the level DOM.
  function renderElevationBands() {
    const bands = document.getElementById("zLevelBands");
    if (!bands || !currentHud) return;
    const levelCount = Math.max(1, Number(currentHud.map && currentHud.map.z) || 1);
    const cameraZ = Number(currentHud.camera && currentHud.camera.z) || 0;
    const rawSurface = Number(currentHud.minimap && currentHud.minimap.surfaceZ);
    const surfaceZ = Math.max(0, Math.min(levelCount - 1,
      Number.isFinite(rawSurface) ? Math.round(rawSurface) : cameraZ));
    const key = `${levelCount}:${surfaceZ}`;
    if (key === elevationBandKey) return;
    elevationBandKey = key;

    const frag = document.createDocumentFragment();
    for (let z = levelCount - 1; z >= 0; --z) {
      const slice = document.createElement("div");
      slice.className = "z-level-slice " +
        (z > surfaceZ ? "sky" : (z === surfaceZ ? "ground" : "underground"));
      slice.dataset.z = String(z);
      frag.appendChild(slice);
    }
    bands.replaceChildren(frag);
  }

  function renderPeerElevations() {
    const host = document.getElementById("zPeerMarkers");
    const track = document.getElementById("zScrollTrack");
    if (!host || !track || !currentHud) return;
    const maxZ = Math.max(0, (Number(currentHud.map && currentHud.map.z) || 1) - 1);
    const peers = typeof window.dfPresencePeers === "function"
      ? window.dfPresencePeers().filter(p => p && p.hasCam)
      : [];
    peers.sort((a, b) => String(a.name || a.id).localeCompare(String(b.name || b.id)));

    const trackHeight = track.getBoundingClientRect().height || 1;
    const placed = [];
    const frag = document.createDocumentFragment();
    for (const peer of peers) {
      const top = zTopPercent(peer.cz, maxZ);
      const y = top * trackHeight / 100;
      let collisions = 0;
      for (const prior of placed) if (Math.abs(prior - y) < 7) collisions++;
      placed.push(y);

      const marker = document.createElement("button");
      marker.type = "button";
      marker.className = "z-peer-marker";
      marker.style.top = `${top.toFixed(2)}%`;
      marker.style.left = `${-8 - collisions * 4}px`;
      marker.style.borderLeftColor = peer.color || "#8fd7ff";
      marker.style.color = peer.color || "#8fd7ff";
      marker.title = `${peer.name || peer.id} - elevation ${peer.cz}. Click to view their camera.`;
      marker.setAttribute("aria-label", marker.title);
      marker.addEventListener("pointerdown", event => event.stopPropagation());
      marker.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        cameraGoto(peer.cx, peer.cy, peer.cz);
        if (typeof focusPage === "function") focusPage();
      });
      frag.appendChild(marker);
    }
    host.replaceChildren(frag);
  }

  // Called from the HUD render so the native marker tracks this camera's z live.
  function syncMinimapZ() {
    const marker = document.getElementById("zScrollMarker");
    if (!marker || !currentHud || !currentHud.camera) return;
    const mz = (currentHud.map && currentHud.map.z) || 1;
    marker.style.top = `${zTopPercent(currentHud.camera.z, mz - 1).toFixed(2)}%`;
  }

  function syncMinimapControls() {
    renderElevationBands();
    syncMinimapZ();
    renderPeerElevations();
    const follow = document.getElementById("followBtn");
    if (follow) {
      const state = typeof window.dfcFollowState === "function" ? window.dfcFollowState() : null;
      follow.hidden = !(state && state.following);
      follow.title = state && state.following
        ? "Stop following " + state.following
        : "Stop following player";
    }
  }
  window.syncMinimapControls = syncMinimapControls;
  window.syncPresenceElevationMarkers = renderPeerElevations;

  function zTrackSetFromY(clientY) {
    const track = document.getElementById("zScrollTrack");
    if (!track || !currentHud) return;
    const r = track.getBoundingClientRect();
    const frac = 1 - Math.max(0, Math.min(1, (clientY - r.top) / r.height));  // top of track = high z
    const mz = (currentHud.map && currentHud.map.z) || 1;
    setCameraZ(Math.round(frac * (mz - 1)));
  }

  (function initMinimapExtras() {
    renderBookmarks();
    const bookmarkButton = document.getElementById("recenterLocationsBtn");
    const bookmarkRow = document.getElementById("bmRow");
    if (bookmarkButton && bookmarkRow) {
      bookmarkButton.addEventListener("click", e => {
        e.preventDefault();
        e.stopPropagation();
        bookmarkRow.hidden = !bookmarkRow.hidden;
        if (!bookmarkRow.hidden) renderBookmarks();
      });
      document.addEventListener("pointerdown", e => {
        if (bookmarkRow.hidden || e.target.closest("#bmRow, #recenterLocationsBtn")) return;
        bookmarkRow.hidden = true;
      });
    }
    const followButton = document.getElementById("followBtn");
    if (followButton) followButton.addEventListener("click", e => {
      e.preventDefault();
      e.stopPropagation();
      if (typeof window.dfcStopFollowing === "function") window.dfcStopFollowing();
      syncMinimapControls();
      if (typeof focusPage === "function") focusPage();
    });
    const track = document.getElementById("zScrollTrack");
    if (track) {
      let dragging = false;
      track.addEventListener("pointerdown", e => { e.preventDefault(); e.stopPropagation(); dragging = true; try { track.setPointerCapture(e.pointerId); } catch (_) {} zTrackSetFromY(e.clientY); });
      track.addEventListener("pointermove", e => { if (dragging) zTrackSetFromY(e.clientY); });
      track.addEventListener("pointerup", e => { dragging = false; try { track.releasePointerCapture(e.pointerId); } catch (_) {} });
      track.addEventListener("pointercancel", e => { dragging = false; try { track.releasePointerCapture(e.pointerId); } catch (_) {} });
      track.addEventListener("wheel", e => { e.preventDefault(); e.stopPropagation(); const cam = currentHud && currentHud.camera; if (cam) setCameraZ(cam.z + (e.deltaY < 0 ? 1 : -1)); }, { passive: false });
    }
    document.querySelectorAll("[data-z-meter-step]").forEach(button => {
      button.addEventListener("click", e => {
        e.preventDefault();
        e.stopPropagation();
        const cam = currentHud && currentHud.camera;
        if (cam) setCameraZ(cam.z + Number(button.dataset.zMeterStep || 0));
        if (typeof focusPage === "function") focusPage();
      });
    });
    syncMinimapControls();
  })();
