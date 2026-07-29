// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-only

(function () {
  "use strict";

  const { esc, jsonFetch } = window.dfDomainUi;
  let popupSignature = "";
  let popupBusy = false;

  function popupHost() {
    let host = document.getElementById("nativePopupSync");
    if (!host) {
      host = document.createElement("div");
      host.id = "nativePopupSync";
      host.setAttribute("aria-live", "assertive");
      document.body.appendChild(host);
    }
    return host;
  }

  function renderNativePopup(data) {
    const host = popupHost();
    const popup = Array.isArray(data?.popups) ? data.popups[0] : null;
    if (!popup) {
      host.className = "";
      host.replaceChildren();
      popupSignature = "";
      return;
    }
    const signature = `${popup.id}:${JSON.stringify(popup.text || [])}`;
    if (signature === popupSignature && host.classList.contains("visible")) return;
    popupSignature = signature;
    host.className = "visible";
    host.innerHTML = `
      <div class="native-popup-window" role="alertdialog" aria-modal="true">
        <div class="native-popup-title">Dwarf Fortress</div>
        <div class="native-popup-copy">${(popup.text || []).map(line =>
          `<div>${esc(line)}</div>`).join("")}</div>
        <button type="button" data-popup-dismiss="${Number(popup.id)}">Dismiss</button>
      </div>`;
    host.querySelector("[data-popup-dismiss]")?.addEventListener("click", async event => {
      event.preventDefault();
      const id = Number(event.currentTarget.dataset.popupDismiss);
      if (!Number.isFinite(id) || popupBusy) return;
      popupBusy = true;
      try {
        await jsonFetch(`/popup/dismiss?id=${id}`, { method: "POST" });
        popupSignature = "";
        await pollNativePopup();
      } catch (_) {
        popupSignature = "";
      } finally {
        popupBusy = false;
      }
    });
  }

  async function pollNativePopup() {
    try { renderNativePopup(await jsonFetch(`/popup?t=${Date.now()}`)); }
    catch (_) {}
  }

  addEventListener("DOMContentLoaded", () => {
    pollNativePopup();
    setInterval(pollNativePopup, 600);
  });
})();
