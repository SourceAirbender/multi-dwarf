// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

(function () {
  "use strict";

  const STORAGE_VOLUME = "dfcapture.audio.volume";
  const STORAGE_MUTED = "dfcapture.audio.muted";
  const state = {
    available: false,
    allowed: false,
    host: false,
    unlocked: false,
    muted: false,
    volume: 0.55,
    audio: null,
    track: "",
    elapsedMs: 0,
    tracks: [],
    pollTimer: 0
  };

  function readLocal() {
    try {
      const volume = Number(localStorage.getItem(STORAGE_VOLUME));
      if (Number.isFinite(volume)) state.volume = Math.max(0, Math.min(1, volume));
      state.muted = localStorage.getItem(STORAGE_MUTED) === "true";
    } catch (_) {}
  }

  function writeLocal() {
    try {
      localStorage.setItem(STORAGE_VOLUME, String(state.volume));
      localStorage.setItem(STORAGE_MUTED, String(state.muted));
    } catch (_) {}
  }

  function soundUrl(path) {
    return "/sound/" + String(path || "").split("/").map(encodeURIComponent).join("/");
  }

  function trackInfo(key) {
    return state.tracks.find(track => track.key === key) || null;
  }

  function currentSeason() {
    const hud = typeof currentHud !== "undefined" ? currentHud : null;
    const label = String(hud?.date?.season || "").toLowerCase();
    if (label.includes("spring")) return 0;
    if (label.includes("summer")) return 1;
    if (label.includes("autumn")) return 2;
    if (label.includes("winter")) return 3;
    return -1;
  }

  function circularDrift(actual, expected, duration) {
    if (!Number.isFinite(duration) || duration <= 0) return Math.abs(actual - expected);
    const raw = Math.abs(actual - expected) % duration;
    return Math.min(raw, duration - raw);
  }

  function syncPosition(force) {
    const audio = state.audio;
    if (!audio || !Number.isFinite(audio.duration) || audio.duration <= 0) return;
    const expected = (Math.max(0, state.elapsedMs) / 1000) % audio.duration;
    if (force || circularDrift(audio.currentTime, expected, audio.duration) > 1.5) {
      try { audio.currentTime = expected; } catch (_) {}
    }
  }

  function applyPlayback() {
    const audio = state.audio;
    if (!audio) return;
    audio.volume = state.volume;
    audio.muted = state.muted;
    if (!state.available || !state.allowed || state.muted || !state.unlocked || !state.track) {
      try { audio.pause(); } catch (_) {}
      return;
    }
    audio.play().catch(() => {});
  }

  function ensureAudio() {
    if (state.audio) return state.audio;
    const audio = new Audio();
    audio.preload = "auto";
    audio.loop = true;
    audio.addEventListener("loadedmetadata", () => {
      syncPosition(true);
      applyPlayback();
    });
    state.audio = audio;
    return audio;
  }

  function setCanonicalMusic(music) {
    if (!music || typeof music.track !== "string") return;
    const info = trackInfo(music.track);
    state.elapsedMs = Number.isFinite(Number(music.elapsedMs)) ? Number(music.elapsedMs) : 0;
    if (!info) {
      state.track = "";
      render();
      return;
    }
    const audio = ensureAudio();
    if (state.track !== music.track) {
      state.track = music.track;
      try {
        audio.pause();
        audio.src = soundUrl(info.path);
        audio.load();
      } catch (_) {}
    } else {
      syncPosition(false);
    }
    applyPlayback();
    render();
  }

  async function probe() {
    try {
      const response = await fetch("/sound-info", { cache: "no-store" });
      if (response.status === 401) return;
      const info = response.ok ? await response.json() : {};
      state.available = info.audio === true;
      state.allowed = info.allowed === true;
      state.host = info.host === true;
    } catch (_) {
      state.available = false;
      state.allowed = false;
    }
    render();
  }

  async function pollMusic() {
    if (!state.available || !state.allowed) return;
    try {
      const season = currentSeason();
      const response = await fetch(`/music?season=${season}`, { cache: "no-store" });
      if (!response.ok) return;
      const music = await response.json();
      state.host = music.host === true;
      state.tracks = Array.isArray(music.tracks) ? music.tracks : [];
      setCanonicalMusic(music);
    } catch (_) {}
  }

  async function chooseMusic(parameters) {
    const response = await fetch("/music", {
      method: "POST",
      cache: "no-store",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: new URLSearchParams(parameters).toString()
    });
    if (!response.ok) throw new Error("Music control failed");
    setCanonicalMusic(await response.json());
  }

  function statusText() {
    if (!state.available) return "Installed Dwarf Fortress audio was not found.";
    if (!state.allowed) return "The host has disabled audio streaming for friends.";
    const track = trackInfo(state.track);
    if (!track) return state.unlocked ? "Waiting for fortress music." : "Click anywhere to enable audio.";
    return `${state.unlocked ? "Now playing" : "Ready"}: ${track.label}`;
  }

  function render() {
    const section = document.getElementById("audioSettings");
    if (!section) return;
    const status = section.querySelector(".audio-status");
    if (status) status.textContent = statusText();
    const mute = document.getElementById("setAudioMuted");
    if (mute) mute.classList.toggle("on", !state.muted);
    const volume = document.getElementById("audioVolume");
    if (volume && document.activeElement !== volume) volume.value = String(state.volume);
    const select = document.getElementById("audioTrack");
    if (select) {
      const signature = state.tracks.map(track => track.key).join("|");
      if (select.dataset.signature !== signature) {
        select.dataset.signature = signature;
        select.replaceChildren(...state.tracks.map(track => {
          const option = document.createElement("option");
          option.value = track.key;
          option.textContent = track.label;
          return option;
        }));
      }
      if (state.track) select.value = state.track;
    }
    const hostControls = document.getElementById("audioHostControls");
    if (hostControls) hostControls.hidden = !state.host;
  }

  function installUi() {
    const menu = document.getElementById("settingsMenu");
    if (!menu || document.getElementById("audioSettings")) return;
    const section = document.createElement("div");
    section.id = "audioSettings";
    section.innerHTML = `
      <h3>Audio</h3>
      <div class="audio-status">Checking installed audio...</div>
      <div class="set-row on" id="setAudioMuted">
        <div class="set-toggle"></div>
        <div class="set-label"><b>Browser audio</b><span>Play synchronized music from this Dwarf Fortress installation.</span></div>
      </div>
      <label class="audio-volume-row">Volume<input id="audioVolume" type="range" min="0" max="1" step="0.01"></label>
      <div id="audioHostControls" class="audio-host-controls" hidden>
        <label>Fortress track<select id="audioTrack"></select></label>
        <div><button type="button" id="audioTrackPlay">Play selected</button><button type="button" id="audioTrackAuto">Auto</button></div>
      </div>`;
    menu.appendChild(section);

    document.getElementById("setAudioMuted")?.addEventListener("click", () => {
      state.muted = !state.muted;
      writeLocal();
      applyPlayback();
      render();
    });
    document.getElementById("audioVolume")?.addEventListener("input", event => {
      state.volume = Math.max(0, Math.min(1, Number(event.target.value)));
      writeLocal();
      applyPlayback();
    });
    document.getElementById("audioTrackPlay")?.addEventListener("click", async () => {
      const track = document.getElementById("audioTrack")?.value;
      if (track) {
        try { await chooseMusic({ track }); } catch (_) {}
      }
    });
    document.getElementById("audioTrackAuto")?.addEventListener("click", async () => {
      try { await chooseMusic({ auto: "true", season: currentSeason() }); } catch (_) {}
    });
    render();
  }

  function unlock() {
    state.unlocked = true;
    applyPlayback();
    render();
    document.removeEventListener("pointerdown", unlock, true);
    document.removeEventListener("keydown", unlock, true);
  }

  async function boot() {
    readLocal();
    installUi();
    document.addEventListener("pointerdown", unlock, true);
    document.addEventListener("keydown", unlock, true);
    await probe();
    await pollMusic();
    state.pollTimer = window.setInterval(async () => {
      await probe();
      await pollMusic();
    }, 2000);
  }

  window.DFCaptureAudio = { refresh: pollMusic };
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot);
  else boot();
})();
