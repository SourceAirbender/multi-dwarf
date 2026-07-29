// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

(function () {
  "use strict";

  const esc = value => String(value ?? "").replace(/[&<>"']/g, ch => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
  }[ch]));

  let config = null;

  function displayName() {
    try { return localStorage.getItem("dfcapture.displayName") || ""; } catch (_) { return ""; }
  }

  function setDisplayName(name) {
    const clean = String(name || "").trim().slice(0, 32);
    try {
      if (clean) localStorage.setItem("dfcapture.displayName", clean);
      else localStorage.removeItem("dfcapture.displayName");
    } catch (_) {}
  }

  async function ensureIdentity(candidate) {
    const name = displayName();
    const query = new URLSearchParams();
    if (candidate) query.set("candidate", String(candidate));
    if (name) query.set("name", name);
    const response = await fetch(`/identity?${query.toString()}`, {
      method: "POST", cache: "no-store"
    });
    if (!response.ok) throw new Error("Player identity is unavailable");
    const identity = await response.json();
    if (!identity?.playerId) throw new Error("Player identity response is invalid");
    try { localStorage.setItem("dfcapture.playerId", identity.playerId); } catch (_) {}
    return identity;
  }

  async function api(path, options) {
    const response = await fetch(path, Object.assign({ cache: "no-store" }, options || {}));
    let body = null;
    try { body = await response.json(); } catch (_) {}
    return { response, body };
  }

  function toggleRow(id, value, enabled) {
    const row = document.getElementById(id);
    if (!row) return;
    row.classList.toggle("on", !!value);
    row.classList.toggle("disabled", enabled === false);
    row.setAttribute("aria-disabled", enabled === false ? "true" : "false");
  }

  function renderSettings() {
    if (!config) return;
    const host = !!config.host;
    toggleRow("setDisconnectPause", config.disconnectPause, host);
    toggleRow("setHostUnpause", config.hostUnpauseOnly, host);
    toggleRow("setRemoteSave", config.remoteSave, host);
    toggleRow("setRemoteAudio", config.remoteAudio, host);
    const state = document.getElementById("sessionState");
    if (state) {
      const players = Array.isArray(config.players) ? config.players : [];
      state.innerHTML = `<b>${players.length} connected</b>` +
        (players.length ? `<span>${players.map(p => esc(p.name || p.player)).join(", ")}</span>` : "");
    }
    const name = document.getElementById("sessionDisplayName");
    if (name && document.activeElement !== name) name.value = displayName();
  }

  // Saves block every browser view, including a localhost tab that the
  // server correctly recognizes as host-authorized. The host distinction controls authority,
  // not whether the browser is temporarily unable to interact with DF's unsafe object graph.
  // Other-player/disconnect pauses still use the compact pill.
  function reflectBusyState(s) {
    if (!s) return;
    let el = document.getElementById("dfcBusy");
    if (!el) { el = document.createElement("div"); el.id = "dfcBusy"; document.body.appendChild(el); }
    let saveNotice = document.getElementById("dfcSaveNotice");
    if (!saveNotice) {
      saveNotice = document.createElement("div");
      saveNotice.id = "dfcSaveNotice";
      saveNotice.setAttribute("role", "status");
      saveNotice.setAttribute("aria-live", "assertive");
      saveNotice.innerHTML = `<div class="dfc-save-card">
        <strong>REMOTE GAME PAUSED</strong>
        <span>Awaiting host save to finish</span>
        <small>Interaction will resume automatically when the fortress is ready.</small>
      </div>`;
      document.body.appendChild(saveNotice);
    }
    const me = (typeof player !== "undefined") ? player : "";
    const saveBlocking = !!s.saving;
    let text = "";
    if (!saveBlocking) {
      if (s.paused && s.pauseReason === "disconnect") text = "Paused — everyone disconnected";
      else if (s.paused && s.pauseActor && s.pauseActor !== me &&
               (s.pauseReason === "player" || s.pauseReason === "host"))
        text = "Paused by " + s.pauseActor;
    }
    el.textContent = text;
    el.classList.toggle("visible", !!text);
    el.classList.remove("saving");
    saveNotice.classList.toggle("visible", saveBlocking);
    saveNotice.setAttribute("aria-hidden", saveBlocking ? "false" : "true");
    const playBtn = document.querySelector('#pauseRow [data-action="play"]');
    if (playBtn) {
      const blocked = s.canUnpause === false;
      playBtn.classList.toggle("blocked", blocked);
      playBtn.title = blocked ? "Only the host may unpause (session policy)" : "Play";
    }
  }

  async function refresh() {
    const result = await api("/session");
    if (result.response.ok && result.body) {
      config = result.body;
      renderSettings();
      reflectBusyState(config);
    } else if (result.response.status === 503 && result.body?.busy) {
      // The global save barrier intentionally rejects /session while DF's object graph is unsafe.
      // Preserve the last known host/remote identity and turn that authoritative 503 into the
      // temporary saving state. The next successful poll clears the notice automatically.
      reflectBusyState(Object.assign({}, config || {}, { saving: true, busyReason: "saving" }));
    }
    return config;
  }

  async function updateConfig(params) {
    const result = await api("/session-config", {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: new URLSearchParams(params).toString()
    });
    if (!result.response.ok) throw new Error(result.body?.error || "Session update failed");
    config = result.body;
    renderSettings();
  }

  function installSettings() {
    const menu = document.getElementById("settingsMenu");
    if (!menu || document.getElementById("sessionSettings")) return;
    const section = document.createElement("div");
    section.id = "sessionSettings";
    section.innerHTML = `
      <h3>Session</h3>
      <div class="session-name-row">
        <label>Display name<input id="sessionDisplayName" maxlength="32" value="${esc(displayName())}"></label>
      </div>
      <div id="sessionState" class="session-state"></div>
      <div class="set-row" id="setDisconnectPause"><div class="set-toggle"></div><div class="set-label">
        <b>Pause after everyone leaves</b><span>Pause after the last browser player disconnects.</span>
      </div></div>
      <div class="set-row" id="setHostUnpause"><div class="set-toggle"></div><div class="set-label">
        <b>Only host may unpause</b><span>Friends can pause, but only the host browser can resume.</span>
      </div></div>
      <div class="set-row" id="setRemoteSave"><div class="set-toggle"></div><div class="set-label">
        <b>Allow remote save</b><span>Let connected friends queue a fortress save.</span>
      </div></div>
      <div class="set-row" id="setRemoteAudio"><div class="set-toggle"></div><div class="set-label">
        <b>Stream audio to friends</b><span>Serve this installation's Dwarf Fortress music to connected remote players.</span>
      </div></div>
      <div class="session-actions">
        <button type="button" id="sessionSaveBtn">Save fortress</button>
      </div>
      <div id="sessionMessage" class="session-message" aria-live="polite"></div>`;
    menu.appendChild(section);

    const message = text => {
      const el = document.getElementById("sessionMessage");
      if (el) el.textContent = text;
    };
    document.getElementById("sessionDisplayName")?.addEventListener("change", event => {
      setDisplayName(event.target.value);
      ensureIdentity(window.dfStablePlayerId || "").catch(() => {});
      message("Display name updated.");
    });
    [["setDisconnectPause", "disconnectPause"], ["setHostUnpause", "hostUnpauseOnly"],
     ["setRemoteSave", "remoteSave"], ["setRemoteAudio", "remoteAudio"]].forEach(([id, key]) => {
      document.getElementById(id)?.addEventListener("click", async () => {
        if (!config?.host) return;
        try { await updateConfig({ [key]: config[key] ? "false" : "true" }); }
        catch (error) { message(error.message); }
      });
    });
    document.getElementById("sessionSaveBtn")?.addEventListener("click", async () => {
      try {
        const result = await api("/save", { method: "POST" });
        message(result.response.ok ? "Fortress save queued." : (result.body?.error || "Save failed."));
      } catch (_) { message("Save request failed."); }
    });
  }

  // Blocking mismatch screen: API-schema mismatches always block. Strict release deployments also
  // block when plugin and web version/source receipts differ. Development deployments permit an
  // exact-revision mismatch when the schemas remain compatible (web-only iteration).
  function showCompatGate(info) {
    const esc = s => String(s == null ? "" : s).replace(/[&<>"]/g,
      c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
    const web = info && info.web;
    const el = document.createElement("div");
    el.id = "dfcCompatGate";
    el.innerHTML = `<div class="dfc-compat-card">
      <h2>Update mismatch</h2>
      <p>${esc((info && info.reason) || "The plugin and web assets are from different builds.")}</p>
      <div class="dfc-compat-ids">
        <div>Plugin <b>v${esc(info && info.version)}</b>, schema <b>${Number(info && info.schema)}</b></div>
        <div>Web <b>v${esc(web && web.version)}</b>, schema <b>${web ? Number(web.schema) : "?"}</b></div>
        <div>Plugin source <b>${esc(info && info.sourceCommit)}</b></div>
        <div>Web source <b>${esc(web && web.sourceCommit)}</b></div>
      </div>
      <p class="dfc-compat-fix">Redeploy both halves with tools/deploy.ps1. If the DLL is locked, close Dwarf Fortress first, reopen, then reload this page.</p>
      <button id="dfcCompatReload">Reload</button></div>`;
    document.body.appendChild(el);
    const btn = document.getElementById("dfcCompatReload");
    if (btn) btn.addEventListener("click", () => location.reload());
  }

  async function boot() {
    installSettings();
    try {
      const version = await fetch("/version", { cache: "no-store" });
      const info = await version.json();
      if (info.compatible === false) { showCompatGate(info); return; }
      if (info.warning) console.warn("DFCapture build identity:", info.warning, info);
      await refresh();
      setInterval(refresh, 3000);
    } catch (_) {}
  }

  window.DFCaptureSession = { displayName, refresh, ensureIdentity };
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot);
  else boot();
})();
