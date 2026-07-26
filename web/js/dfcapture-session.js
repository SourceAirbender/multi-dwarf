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

  function cookieCredential(password) {
    document.cookie = `dfcap_auth=${encodeURIComponent(password)}; Path=/; SameSite=Strict`;
  }

  function clearCredential() {
    document.cookie = "dfcap_auth=; Path=/; Max-Age=0; SameSite=Strict";
  }

  function showJoinGate() {
    if (document.getElementById("sessionJoinGate")) return;
    const gate = document.createElement("div");
    gate.id = "sessionJoinGate";
    gate.innerHTML = `
      <form class="session-join-card">
        <h2>Join fortress</h2>
        <label>Display name<input name="name" maxlength="32" required value="${esc(displayName())}"></label>
        <label>Join password<input name="password" type="password" autocomplete="current-password" required></label>
        <div class="session-join-error" aria-live="polite"></div>
        <button type="submit">Join</button>
      </form>`;
    document.body.appendChild(gate);
    const form = gate.querySelector("form");
    form.addEventListener("submit", async event => {
      event.preventDefault();
      const error = gate.querySelector(".session-join-error");
      const data = new FormData(form);
      const name = String(data.get("name") || "").trim();
      const password = String(data.get("password") || "");
      if (!name || !password) return;
      error.textContent = "Joining...";
      try {
        const response = await fetch("/join", {
          method: "POST",
          headers: { "Content-Type": "application/x-www-form-urlencoded" },
          body: `password=${encodeURIComponent(password)}`,
          cache: "no-store"
        });
        if (!response.ok) {
          error.textContent = "Wrong password. Ask the host for the current passphrase.";
          return;
        }
        setDisplayName(name);
        cookieCredential(password);
        location.reload();
      } catch (_) {
        error.textContent = "The fortress server is unavailable.";
      }
    });
    gate.querySelector('input[name="name"]')?.focus();
  }

  async function api(path, options) {
    const response = await fetch(path, Object.assign({ cache: "no-store" }, options || {}));
    let body = null;
    try { body = await response.json(); } catch (_) {}
    if (response.status === 401) showJoinGate();
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
    const hostBox = document.getElementById("sessionHostSettings");
    if (hostBox) hostBox.hidden = !host;
    const state = document.getElementById("sessionState");
    if (state) {
      const players = Array.isArray(config.players) ? config.players : [];
      state.innerHTML = `<b>${players.length} connected</b>` +
        (players.length ? `<span>${players.map(p => esc(p.name || p.player)).join(", ")}</span>` : "");
    }
    const name = document.getElementById("sessionDisplayName");
    if (name && document.activeElement !== name) name.value = displayName();
  }

  async function refresh() {
    const result = await api("/session");
    if (result.response.ok && result.body) {
      config = result.body;
      renderSettings();
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
        <b>Allow remote save</b><span>Let authenticated friends queue a fortress save.</span>
      </div></div>
      <div class="set-row" id="setRemoteAudio"><div class="set-toggle"></div><div class="set-label">
        <b>Stream audio to friends</b><span>Serve this installation's Dwarf Fortress music to authenticated remote players.</span>
      </div></div>
      <div class="session-actions">
        <button type="button" id="sessionSaveBtn">Save fortress</button>
      </div>
      <div id="sessionHostSettings" class="session-host" hidden>
        <label>Join password<input id="sessionPassword" type="password" autocomplete="new-password"></label>
        <button type="button" id="sessionPasswordSet">Set password</button>
        <button type="button" id="sessionPasswordOff">Turn off password</button>
      </div>
      <div id="sessionMessage" class="session-message" aria-live="polite"></div>`;
    menu.appendChild(section);

    const message = text => {
      const el = document.getElementById("sessionMessage");
      if (el) el.textContent = text;
    };
    document.getElementById("sessionDisplayName")?.addEventListener("change", event => {
      setDisplayName(event.target.value);
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
    document.getElementById("sessionPasswordSet")?.addEventListener("click", async () => {
      const input = document.getElementById("sessionPassword");
      const password = String(input?.value || "");
      if (!password) { message("Enter a password first."); return; }
      try {
        await updateConfig({ password });
        cookieCredential(password);
        input.value = "";
        message("Join password updated.");
      } catch (error) { message(error.message); }
    });
    document.getElementById("sessionPasswordOff")?.addEventListener("click", async () => {
      try {
        await updateConfig({ passwordOff: "true" });
        clearCredential();
        message("Join password disabled.");
      } catch (error) { message(error.message); }
    });
  }

  async function boot() {
    installSettings();
    try {
      const version = await fetch("/version", { cache: "no-store" });
      const info = await version.json();
      if (info.authRequired) {
        const session = await fetch("/session", { cache: "no-store" });
        if (session.status === 401) {
          showJoinGate();
          return;
        }
      }
      await refresh();
      setInterval(refresh, 3000);
    } catch (_) {}
  }

  window.DFCaptureSession = { displayName, refresh, showJoinGate };
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot);
  else boot();
})();
