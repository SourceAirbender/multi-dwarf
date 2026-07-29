// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

(function () {
  "use strict";

  const { esc, jsonFetch } = window.dfDomainUi;
  const hotkeyModeKey = "dfcapture.cameraLocations.mode";
  let hotkeys = [];

  function useFortressHotkeys() {
    try { return localStorage.getItem(hotkeyModeKey) !== "local"; }
    catch (_) { return true; }
  }

  function localHotkeyKey() {
    return `dfcapture.cameraLocations.local.${String(player || "player")}`;
  }

  function loadLocalHotkeys() {
    try {
      const saved = JSON.parse(localStorage.getItem(localHotkeyKey()) || "[]");
      return Array.isArray(saved) ? saved.filter(entry =>
        entry && Number.isInteger(Number(entry.slot)) &&
        Number(entry.slot) >= 0 && Number(entry.slot) < 16) : [];
    } catch (_) {
      return [];
    }
  }

  function saveLocalHotkeys(entries) {
    try { localStorage.setItem(localHotkeyKey(), JSON.stringify(entries)); }
    catch (_) {}
  }

  async function reloadHotkeys() {
    if (!useFortressHotkeys()) {
      hotkeys = loadLocalHotkeys();
      return;
    }
    try {
      const data = await jsonFetch(`/hotkeys?t=${Date.now()}`);
      hotkeys = Array.isArray(data.hotkeys) ? data.hotkeys : [];
    } catch (_) {
      hotkeys = [];
    }
  }

  async function writeHotkey(slot, action, extra = {}) {
    if (!useFortressHotkeys()) {
      const entries = loadLocalHotkeys();
      const index = entries.findIndex(entry => Number(entry.slot) === slot);
      if (action === "set") {
        const prior = index >= 0 ? entries[index] : null;
        const entry = {
          slot, set: true, name: prior?.name || `Location ${slot + 1}`,
          x: Number(extra.x), y: Number(extra.y), z: Number(extra.z)
        };
        if (index >= 0) entries[index] = entry;
        else entries.push(entry);
      } else if (action === "clear" && index >= 0) {
        entries.splice(index, 1);
      } else if (action === "rename" && index >= 0) {
        entries[index].name = String(extra.name || `Location ${slot + 1}`);
      }
      saveLocalHotkeys(entries);
      hotkeys = entries;
      return;
    }
    const params = new URLSearchParams({ slot: String(slot), action, ...extra });
    await jsonFetch(`/hotkey-action?${params}`, { method: "POST" });
    await reloadHotkeys();
  }

  function renderFortressHotkeys(row) {
    const shared = useFortressHotkeys();
    row.title = shared
      ? "Shared fortress camera locations: click to save or jump; right-click clears"
      : "Private browser camera locations: click to save or jump; right-click clears";
    row.innerHTML = Array.from({ length: 16 }, (_, slot) => {
      const hotkey = hotkeys.find(entry => Number(entry.slot) === slot);
      const set = !!hotkey?.set;
      const name = hotkey?.name || `Location ${slot + 1}`;
      return `<button class="bm-slot${set ? " set" : ""}" data-fort-hotkey="${slot}"
        title="${set ? `Jump to ${esc(name)}; right-click clears; double-click renames`
          : `Save current camera as ${esc(name)}`}">${slot + 1}</button>`;
    }).join("");
    row.querySelectorAll("[data-fort-hotkey]").forEach(button => {
      const slot = Number(button.dataset.fortHotkey);
      button.addEventListener("click", async event => {
        event.preventDefault();
        event.stopPropagation();
        const hotkey = hotkeys.find(entry => Number(entry.slot) === slot);
        if (hotkey?.set) {
          cameraGoto(hotkey.x, hotkey.y, hotkey.z);
          return;
        }
        const camera = currentHud?.camera;
        if (!camera) return;
        await writeHotkey(slot, "set", {
          x: String(camera.x), y: String(camera.y), z: String(camera.z)
        }).catch(() => {});
        renderFortressHotkeys(row);
      });
      button.addEventListener("contextmenu", async event => {
        event.preventDefault();
        event.stopPropagation();
        await writeHotkey(slot, "clear").catch(() => {});
        renderFortressHotkeys(row);
      });
      button.addEventListener("dblclick", async event => {
        event.preventDefault();
        event.stopPropagation();
        const hotkey = hotkeys.find(entry => Number(entry.slot) === slot);
        if (!hotkey?.set) return;
        const name = prompt("Location name", hotkey.name || `Location ${slot + 1}`);
        if (name == null) return;
        await writeHotkey(slot, "rename", { name }).catch(() => {});
        renderFortressHotkeys(row);
      });
    });
  }

  async function installFortressHotkeys() {
    const oldRow = document.getElementById("bmRow");
    const trigger = document.getElementById("recenterLocationsBtn");
    if (!oldRow || !trigger) return;
    const row = oldRow.cloneNode(false);
    row.id = "bmRow";
    row.hidden = true;
    oldRow.replaceWith(row);
    await reloadHotkeys();
    renderFortressHotkeys(row);

    const cleanTrigger = trigger.cloneNode(true);
    trigger.replaceWith(cleanTrigger);
    cleanTrigger.addEventListener("click", event => {
      event.preventDefault();
      event.stopPropagation();
      row.hidden = !row.hidden;
      if (!row.hidden) reloadHotkeys().then(() => renderFortressHotkeys(row));
    });
    document.addEventListener("pointerdown", event => {
      if (!row.hidden && !event.target.closest("#bmRow, #recenterLocationsBtn")) row.hidden = true;
    });

    const modeRow = document.getElementById("setFortressHotkeys");
    const modeDescription = document.getElementById("hotkeyModeDescription");
    const syncModeSetting = () => {
      const shared = useFortressHotkeys();
      modeRow?.classList.toggle("on", shared);
      if (modeDescription) modeDescription.textContent = shared
        ? "Using the fort's 16 shared locations stored in this save."
        : "Using 16 private locations stored only for this player in this browser.";
    };
    syncModeSetting();
    modeRow?.addEventListener("click", async event => {
      event.preventDefault();
      try {
        localStorage.setItem(hotkeyModeKey, useFortressHotkeys() ? "local" : "fortress");
      } catch (_) {}
      syncModeSetting();
      await reloadHotkeys();
      renderFortressHotkeys(row);
    });
  }

  addEventListener("DOMContentLoaded", installFortressHotkeys);
})();
