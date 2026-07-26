// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only
//
// Detailed fortress location management: occupations, dedications, guilds,
// rented rooms, occupants, and appointed positions.
(() => {
  "use strict";

  const state = {
    id: -1,
    data: null,
    picker: "",
    deityOpen: false,
    guildOpen: false,
    search: "",
    error: ""
  };

  const esc = value => escapeHtml(value == null ? "" : String(value));
  const list = value => Array.isArray(value) ? value.filter(Boolean) : [];

  function occupancyLabel(data) {
    const o = data?.occupancy;
    if (!o || !Number.isFinite(Number(o.inside))) return "Occupancy unavailable";
    const parts = [];
    [["citizens", "citizen"], ["residents", "resident"], ["visitors", "visitor"], ["others", "other"]]
      .forEach(([key, label]) => {
        const count = Number(o[key]) || 0;
        if (count) parts.push(`${count} ${label}${count === 1 ? "" : "s"}`);
      });
    return `${Number(o.inside)} inside${parts.length ? `: ${parts.join(", ")}` : ""}`;
  }

  function occupationRows(data) {
    return list(data?.occupations).map(row => {
      const id = Number(row.id);
      const existing = Number.isInteger(id) && id >= 0;
      const canAssign = existing || row.verified !== false || !!data.allowNewSlots;
      return {
        key: existing ? `id:${id}` : String(row.typeKey || ""),
        label: row.label || row.typeKey || "Occupation",
        holder: row.holder || "",
        assigned: !!row.assigned,
        unitId: Number(row.unitId),
        canAssign
      };
    });
  }

  function candidateList(slot) {
    const query = state.search.trim().toLowerCase();
    const candidates = list(state.data?.candidates).filter(candidate => {
      if (!query) return true;
      return `${candidate.name || ""} ${candidate.profession || ""}`.toLowerCase().includes(query);
    });
    return `
      <div class="location-picker">
        <input type="search" value="${esc(state.search)}" placeholder="Search citizens" data-location-search>
        <div class="location-picker-list">
          <button data-location-pick="-1" data-location-slot="${esc(slot)}">Vacant</button>
          ${candidates.map(candidate => `
            <button data-location-pick="${Number(candidate.unitId)}" data-location-slot="${esc(slot)}">
              <strong>${esc(candidate.name || `Unit ${candidate.unitId}`)}</strong>
              <span>${esc(candidate.profession || "")}</span>
              ${candidate.heldOccupation ? `<small>${esc(candidate.heldOccupation)}</small>` : ""}
            </button>`).join("")}
        </div>
      </div>`;
  }

  function occupationsMarkup(data) {
    const rows = occupationRows(data);
    return `
      <section class="location-section">
        <h3>Occupations</h3>
        ${rows.length ? rows.map(row => `
          <div class="location-row">
            <strong>${esc(row.label)}</strong>
            <span>${row.assigned ? esc(row.holder || "Assigned") : "Open"}</span>
            <button data-location-assign="${esc(row.key)}"${row.canAssign ? "" : " disabled"}>
              ${state.picker === row.key ? "Close" : row.assigned ? "Reassign" : "Assign"}
            </button>
          </div>
          ${state.picker === row.key ? candidateList(row.key) : ""}`).join("")
          : `<div class="location-note">This location has no staff positions.</div>`}
      </section>`;
  }

  function templeMarkup(data) {
    const temple = data?.temple;
    if (!temple) return "";
    if (temple.dedicated) {
      return `<section class="location-section"><h3>Dedication</h3>
        <div class="location-note">Dedicated to ${esc(temple.name || "an unknown power")}.</div></section>`;
    }
    const options = list(temple.options);
    return `<section class="location-section"><h3>Dedication</h3>
      <button data-location-toggle="deity">${state.deityOpen ? "Close" : "Dedicate temple"}</button>
      ${state.deityOpen ? `<div class="location-choice-list">${options.map(option => `
        <button data-location-deity="${esc(`${option.mode}:${Number(option.id)}`)}">
          <strong>${esc(option.name || "Unknown")}</strong>
          <span>${Number(option.worshippers) || 0} worshippers</span>
        </button>`).join("")}</div>` : ""}
    </section>`;
  }

  function guildMarkup(data) {
    const guild = data?.guild;
    if (!guild) return "";
    if (guild.dedicated) {
      return `<section class="location-section"><h3>Guild</h3>
        <div class="location-note">Serves the ${esc(guild.key || "assigned")} guild.</div></section>`;
    }
    const options = list(guild.options);
    return `<section class="location-section"><h3>Guild</h3>
      <button data-location-toggle="guild">${state.guildOpen ? "Close" : "Assign guild"}</button>
      ${state.guildOpen ? `<div class="location-choice-list">${options.map(option => `
        <button data-location-guild="${esc(option.key || "")}">
          <strong>${esc(option.name || option.key || "Guild")}</strong>
          <span>${Number(option.members) || 0} members</span>
        </button>`).join("")}</div>` : ""}
    </section>`;
  }

  function roomsMarkup(data) {
    if (!data?.rooms) return "";
    const rooms = list(data.rooms.rooms);
    return `<section class="location-section"><h3>Rented rooms</h3>
      ${rooms.length ? rooms.map(room => `<div class="location-row">
        <strong>${esc(room.label || room.zoneName || `Room ${room.id}`)}</strong>
        <span>${room.rented ? `${esc(room.renter || "Rented")}${Number(room.owed) > 0 ? `, owes ${Number(room.owed)}` : ""}` : "Vacant"}</span>
      </div>`).join("") : `<div class="location-note">No rentable rooms.</div>`}
      ${data.rooms.canWrite ? "" : `<div class="location-note subtle">Room records are read-only.</div>`}
    </section>`;
  }

  function positionsMarkup(data) {
    const positions = list(data?.positions);
    if (!positions.length) return "";
    return `<section class="location-section"><h3>Appointed positions</h3>
      ${positions.map(position => `<div class="location-row">
        <strong>${esc(position.name || "Position")}</strong>
        <span>${position.vacant ? "Vacant" : esc(position.holder || "Assigned")}</span>
      </div>`).join("")}
    </section>`;
  }

  function render() {
    selection.className = "visible building-panel location-panel";
    const data = state.data;
    selection.innerHTML = `<div class="location-sheet">
      <button class="unit-close-button" data-location-close title="Close">X</button>
      <header>
        <h2>${esc(data?.name || data?.label || "Location")}</h2>
        <div>${esc(data?.label || data?.kind || "")}${Number(data?.tier) > 0 ? `, tier ${Number(data.tier)}` : ""}</div>
      </header>
      ${state.error ? `<div class="location-error">${esc(state.error)}</div>` : ""}
      ${data?.ok === false ? `<div class="location-error">${esc(data.error || "Location unavailable")}</div>` : `
        <section class="location-section">
          <h3>Occupants</h3>
          <div class="location-note">${esc(occupancyLabel(data))}</div>
          <div class="location-note subtle">${list(data?.zones).length
            ? esc(list(data.zones).map(zone => zone.name || zone.type || `Zone ${zone.id}`).join(", "))
            : "No zones attached."}</div>
          <div class="location-note">Access: ${esc(data?.restriction || "Unavailable")}</div>
        </section>
        ${occupationsMarkup(data)}
        ${templeMarkup(data)}
        ${guildMarkup(data)}
        ${roomsMarkup(data)}
        ${positionsMarkup(data)}
      `}
    </div>`;
    wire();
  }

  async function post(action, kind = "", unit = -1) {
    state.error = "";
    const params = new URLSearchParams({
      id: String(state.id), action, kind, unit: String(unit)
    });
    const response = await fetch(`/location-action?${params}`, { method: "POST", cache: "no-store" });
    const result = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(result?.error || "Location action failed");
    await reload();
  }

  async function reload() {
    try {
      const response = await fetch(`/location-detail?id=${state.id}&t=${Date.now()}`, { cache: "no-store" });
      const result = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error(result?.error || "Location unavailable");
      state.data = result;
    } catch (error) {
      state.data = { ok: false, error: error.message };
    }
    render();
  }

  function wire() {
    selection.querySelector("[data-location-close]")?.addEventListener("click", () => {
      closeSelection();
      focusPage();
    });
    selection.querySelectorAll("[data-location-assign]").forEach(button => {
      button.addEventListener("click", () => {
        state.picker = state.picker === button.dataset.locationAssign ? "" : button.dataset.locationAssign;
        state.search = "";
        render();
      });
    });
    selection.querySelector("[data-location-search]")?.addEventListener("input", event => {
      state.search = event.target.value || "";
      render();
      const input = selection.querySelector("[data-location-search]");
      if (input) {
        input.focus();
        input.setSelectionRange(input.value.length, input.value.length);
      }
    });
    selection.querySelectorAll("[data-location-pick]").forEach(button => {
      button.addEventListener("click", async () => {
        try {
          await post("occupation-assign", button.dataset.locationSlot || "", Number(button.dataset.locationPick));
          state.picker = "";
        } catch (error) {
          state.error = error.message;
          render();
        }
      });
    });
    selection.querySelectorAll("[data-location-toggle]").forEach(button => {
      button.addEventListener("click", () => {
        if (button.dataset.locationToggle === "deity") state.deityOpen = !state.deityOpen;
        if (button.dataset.locationToggle === "guild") state.guildOpen = !state.guildOpen;
        render();
      });
    });
    selection.querySelectorAll("[data-location-deity]").forEach(button => {
      button.addEventListener("click", async () => {
        try { await post("deity", button.dataset.locationDeity || ""); }
        catch (error) { state.error = error.message; render(); }
      });
    });
    selection.querySelectorAll("[data-location-guild]").forEach(button => {
      button.addEventListener("click", async () => {
        try { await post("guild", button.dataset.locationGuild || ""); }
        catch (error) { state.error = error.message; render(); }
      });
    });
  }

  window.openLocationPanel = async locationId => {
    const id = Number(locationId);
    if (!Number.isInteger(id) || id < 0) return;
    Object.assign(state, {
      id, data: null, picker: "", deityOpen: false, guildOpen: false, search: "", error: ""
    });
    render();
    await reload();
  };
})();
