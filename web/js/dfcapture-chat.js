// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
//
// SPDX-License-Identifier: AGPL-3.0-only

(function () {
  "use strict";

  const MAX_LINES = 100;
  const MAX_TEXT = 500;
  const POLL_MS = 450;
  const LOCATION_RE = /\[\[loc:(-?\d{1,10}),(-?\d{1,10}),(-?\d{1,10})\]\]/g;
  const lines = new Map();
  let pollSeq = 0;
  let unread = 0;
  let opened = false;
  let picking = false;
  let polling = false;

  const toggle = document.createElement("button");
  toggle.id = "chatToggle";
  toggle.type = "button";
  toggle.innerHTML = '<span>Chat</span><span id="chatUnread"></span>';
  toggle.title = "Open multiplayer chat";

  const panel = document.createElement("section");
  panel.id = "chatPanel";
  panel.innerHTML = `
    <header id="chatHeader">
      <strong>Chat</strong>
      <button id="chatClose" type="button" title="Close chat">X</button>
    </header>
    <div id="chatLog" aria-live="polite"></div>
    <div id="chatStatus"></div>
    <footer id="chatComposer">
      <button id="chatPing" type="button" title="Ping a map location">Ping location</button>
      <input id="chatInput" maxlength="${MAX_TEXT}" autocomplete="off"
             placeholder="Message..." aria-label="Chat message">
      <button id="chatSend" type="button">Send</button>
    </footer>`;
  document.body.append(toggle, panel);

  const unreadEl = panel.ownerDocument.getElementById("chatUnread");
  const logEl = panel.ownerDocument.getElementById("chatLog");
  const statusEl = panel.ownerDocument.getElementById("chatStatus");
  const inputEl = panel.ownerDocument.getElementById("chatInput");
  const pingEl = panel.ownerDocument.getElementById("chatPing");

  function safeInt(value) {
    const n = Number(value);
    return Number.isInteger(n) && n >= -2147483648 && n <= 2147483647 ? n : null;
  }

  function mapPos(value) {
    if (!value) return null;
    const x = safeInt(value.x), y = safeInt(value.y), z = safeInt(value.z);
    return x == null || y == null || z == null ? null : { x, y, z };
  }

  function locationToken(pos) {
    const p = mapPos(pos);
    return p ? `[[loc:${p.x},${p.y},${p.z}]]` : "";
  }

  function setStatus(text, error) {
    statusEl.textContent = text || "";
    statusEl.classList.toggle("error", !!error);
  }

  function setPicking(value) {
    picking = !!value;
    pingEl.classList.toggle("active", picking);
    pingEl.textContent = picking ? "Click map..." : "Ping location";
    document.body.classList.toggle("chat-pick-active", picking);
    if (picking)
      setStatus("Click a location on the map. Esc cancels.", false);
    else if (statusEl.textContent.startsWith("Click a location"))
      setStatus("", false);
  }

  function setOpen(value) {
    opened = !!value;
    panel.classList.toggle("open", opened);
    toggle.hidden = opened;
    if (!opened)
      setPicking(false);
    if (opened) {
      unread = 0;
      unreadEl.textContent = "";
      requestAnimationFrame(() => {
        logEl.scrollTop = logEl.scrollHeight;
        inputEl.focus();
      });
    }
  }

  function colorFor(name) {
    try {
      if (typeof playerColor === "function")
        return playerColor(name);
    } catch (_) {}
    return "#8fd7ff";
  }

  function appendTextWithLocations(container, text) {
    let cursor = 0;
    LOCATION_RE.lastIndex = 0;
    for (let match; (match = LOCATION_RE.exec(text));) {
      if (match.index > cursor)
        container.append(document.createTextNode(text.slice(cursor, match.index)));
      const pos = mapPos({ x: match[1], y: match[2], z: match[3] });
      if (!pos) {
        container.append(document.createTextNode(match[0]));
      } else {
        const link = document.createElement("button");
        link.type = "button";
        link.className = "chat-location";
        link.textContent = `Location ${pos.x}, ${pos.y}, ${pos.z}`;
        link.title = "Center camera here";
        link.addEventListener("click", async () => {
          setOpen(false);
          if (typeof centerAndFlashMapPos === "function")
            await centerAndFlashMapPos(pos);
          else {
            if (typeof setCameraToMapPos === "function")
              await setCameraToMapPos(pos);
            if (typeof flashMapTile === "function")
              await flashMapTile(pos);
          }
        });
        container.append(link);
      }
      cursor = match.index + match[0].length;
    }
    if (cursor < text.length)
      container.append(document.createTextNode(text.slice(cursor)));
  }

  function renderLines(stickToBottom) {
    const ordered = [...lines.values()].sort((a, b) => Number(a.seq) - Number(b.seq));
    while (ordered.length > MAX_LINES) {
      const removed = ordered.shift();
      lines.delete(Number(removed.seq));
    }
    logEl.replaceChildren();
    if (!ordered.length) {
      const empty = document.createElement("div");
      empty.className = "chat-empty";
      empty.textContent = "No messages yet.";
      logEl.append(empty);
    }
    for (const line of ordered) {
      const row = document.createElement("div");
      row.className = "chat-line";
      const name = document.createElement("span");
      name.className = "chat-name";
      name.style.color = colorFor(line.from);
      name.textContent = `${line.from}: `;
      const body = document.createElement("span");
      body.className = "chat-text";
      appendTextWithLocations(body, String(line.text || ""));
      row.append(name, body);
      logEl.append(row);
    }
    if (stickToBottom)
      logEl.scrollTop = logEl.scrollHeight;
  }

  function applyLines(incoming) {
    const wasAtBottom = logEl.scrollHeight - logEl.scrollTop - logEl.clientHeight < 28;
    let changed = false;
    for (const raw of incoming || []) {
      const seq = Number(raw?.seq) || 0;
      if (!seq || lines.has(seq))
        continue;
      const line = {
        seq,
        from: String(raw.from || "player").slice(0, 32),
        text: String(raw.text || "").slice(0, MAX_TEXT),
        ts: Number(raw.ts) || 0
      };
      lines.set(seq, line);
      changed = true;
      if (!opened && line.from !== player)
        unread++;
    }
    if (!changed)
      return;
    unreadEl.textContent = unread ? String(Math.min(unread, 99)) : "";
    renderLines(opened && wasAtBottom);
  }

  async function pollChat() {
    if (polling)
      return;
    polling = true;
    try {
      const response = await fetch(`/chat?since=${pollSeq}&t=${Date.now()}`,
        { cache: "no-store" });
      if (!response.ok)
        return;
      const data = await response.json();
      applyLines(data.lines);
      pollSeq = Math.max(pollSeq, Number(data.latest) || 0);
      setStatus("", false);
    } catch (_) {
      // A missed chat poll is harmless; the next response returns every missed sequence.
    } finally {
      polling = false;
    }
  }

  async function postText(text) {
    text = String(text || "").trim().slice(0, MAX_TEXT);
    if (!text)
      return false;
    const query = new URLSearchParams({ player, name: player, text });
    try {
      const response = await fetch(`/chat?${query.toString()}`,
        { method: "POST", cache: "no-store" });
      const data = await response.json().catch(() => ({}));
      if (!response.ok) {
        setStatus(data.error || "Message was not sent.", true);
        return false;
      }
      if (data.line)
        applyLines([data.line]);
      inputEl.value = "";
      return true;
    } catch (_) {
      setStatus("Chat is temporarily unavailable.", true);
      return false;
    }
  }

  async function sendLocation(pos) {
    const p = mapPos(pos);
    if (!p)
      return false;
    const sent = await postText(locationToken(p));
    const query = new URLSearchParams({
      player, name: player, color: (typeof myColor === "string" ? myColor : colorFor(player)),
      x: String(p.x), y: String(p.y), z: String(p.z)
    });
    fetch(`/ping?${query.toString()}`, { method: "POST", cache: "no-store" }).catch(() => {});
    return sent;
  }

  function consumeMapPick(pos) {
    if (!picking)
      return false;
    setPicking(false);
    sendLocation(pos);
    return true;
  }

  async function sendCurrent() {
    await postText(inputEl.value);
  }

  toggle.addEventListener("click", () => setOpen(true));
  panel.querySelector("#chatClose").addEventListener("click", () => setOpen(false));
  panel.querySelector("#chatSend").addEventListener("click", sendCurrent);
  pingEl.addEventListener("click", () => setPicking(!picking));
  inputEl.addEventListener("keydown", event => {
    if (event.key === "Enter" && !event.shiftKey) {
      event.preventDefault();
      sendCurrent();
    } else if (event.key === "Escape") {
      event.preventDefault();
      if (picking)
        setPicking(false);
      else
        setOpen(false);
    }
  });
  addEventListener("keydown", event => {
    if (event.key === "Escape" && picking) {
      event.preventDefault();
      event.stopImmediatePropagation();
      setPicking(false);
    }
  }, { capture: true });

  window.DFCaptureChat = {
    isPicking: () => picking,
    consumeMapPick,
    sendLocation,
    open: () => setOpen(true)
  };

  pollChat();
  setInterval(pollChat, POLL_MS);
})();
