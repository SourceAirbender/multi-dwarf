// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios
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

  function closeClientPanel() {
    clientPanel.className = "";
    clientPanel.innerHTML = "";
    activeInfoPanel = null;
    activeInfoSection = null;
    activeInfoDetail = null;
  }

  function notificationsPanelIsOpen() {
    return activeInfoPanel === "alerts" &&
      clientPanel.classList.contains("visible") &&
      clientPanel.classList.contains("alerts-window");
  }

  window.dfcPortraitLoad = function(img) {
    if (!img || !img.parentElement) return;
    img.parentElement.classList.add("has-native-portrait");
  };

  window.dfcPortraitError = function(img) {
    if (!img) return;
    const tries = Number(img.dataset.tries || 0);
    const max = Number(img.dataset.maxTries || 0);
    if (tries < max && img.dataset.srcBase) {
      const next = tries + 1;
      img.dataset.tries = String(next);
      const delay = 120 + next * 110;
      window.setTimeout(() => {
        img.src = `${img.dataset.srcBase}&try=${next}&_=${Date.now()}`;
      }, delay);
      return;
    }
    if (img.dataset.iconSrc && !img.dataset.iconTried) {
      img.dataset.iconTried = "1";
      img.dataset.tries = "0";
      img.dataset.maxTries = "2";
      img.dataset.srcBase = img.dataset.iconSrc;
      window.setTimeout(() => {
        img.src = `${img.dataset.iconSrc}&try=0&_=${Date.now()}`;
      }, 80);
      return;
    }
    img.remove();
  };

  function unitPortraitMarkup(unit, className = "unit-portrait") {
    const source = unit || {};
    const id = Number(source.id ?? source.unitId ?? -1);
    const texpos = Number(source.portraitTexpos ?? -1);
    const sheetTexpos = Number(source.sheetIconTexpos ?? -1);
    const glyphSource = source.race || source.name || source.category || "?";
    const glyph = String(glyphSource).trim().slice(0, 1).toUpperCase() || "?";
    const fallback = `<div class="portrait-glyph">${escapeHtml(glyph)}</div>`;
    if (!unitImagesEnabled)
      return `<div class="${className}" data-unit-portrait-box="${escapeHtml(id)}">${fallback}</div>`;
    if (id >= 0) {
      const small = String(className || "").includes("info-portrait-small");
      if (!small && texpos <= 0)
        return `<div class="${className}" data-unit-portrait-box="${escapeHtml(id)}">${fallback}</div>`;
      const base = `/unit-portrait?id=${encodeURIComponent(id)}&mode=portrait&tex=${encodeURIComponent(texpos)}&sheet=${encodeURIComponent(sheetTexpos)}`;
      const icon = `/unit-portrait?id=${encodeURIComponent(id)}&mode=icon&tex=${encodeURIComponent(texpos)}&sheet=${encodeURIComponent(sheetTexpos)}`;
      const primary = small ? icon : base;
      const maxTries = 1;
      const src = `${primary}&try=0&_=${Date.now()}`;
      return `<div class="${className}" data-unit-portrait-box="${escapeHtml(id)}"><img class="native-portrait-img" src="${escapeHtml(src)}" data-src-base="${escapeHtml(primary)}" data-tries="0" data-max-tries="${maxTries}" alt="" draggable="false" decoding="async" onload="window.dfcPortraitLoad(this)" onerror="window.dfcPortraitError(this)">${fallback}</div>`;
    }
    return `<div class="${className}">${fallback}</div>`;
  }

  function generateUnitPortrait(unit, button) {
    const source = unit || {};
    const id = Number(source.id ?? source.unitId ?? -1);
    if (id < 0) return;
    if (!unitImagesEnabled) {
      if (button) {
        button.disabled = true;
        button.textContent = "Off";
        window.setTimeout(() => {
          button.disabled = false;
          button.innerHTML = "&#128444;";
        }, 750);
      }
      return;
    }
    const box = selection.querySelector(`[data-unit-portrait-box="${id}"]`);
    if (!box) return;
    const texpos = Number(source.portraitTexpos ?? -1);
    const sheetTexpos = Number(source.sheetIconTexpos ?? -1);
    const src = `/unit-portrait?id=${encodeURIComponent(id)}&mode=portrait&generate=1&tex=${encodeURIComponent(texpos)}&sheet=${encodeURIComponent(sheetTexpos)}&_=${Date.now()}`;
    if (button) {
      button.disabled = true;
      button.textContent = "...";
    }
    box.classList.remove("has-native-portrait");
    box.querySelectorAll(".native-portrait-img").forEach(img => img.remove());
    const img = document.createElement("img");
    img.className = "native-portrait-img";
    img.alt = "";
    img.draggable = false;
    img.decoding = "async";
    img.onload = () => {
      source.portraitTexpos = 1;
      window.dfcPortraitLoad(img);
      if (button) {
        button.disabled = false;
        button.innerHTML = "&#128444;";
      }
    };
    img.onerror = () => {
      img.remove();
      box.classList.remove("has-native-portrait");
      if (button) {
        button.disabled = false;
        button.innerHTML = "&#128444;";
      }
    };
    box.prepend(img);
    img.src = src;
  }

  function unitOverviewLines(unit, key, fallback = []) {
    const value = unit && Array.isArray(unit[key]) ? unit[key] : [];
    return value.length ? value : fallback;
  }

  // --- DF-style color coding for unit text (emotions / skills / needs) ---
  const EMOTION_POS = new Set(["love","fondness","tenderness","affection","caring","joy","happiness",
    "euphoria","euphoric","ecstasy","elation","delight","satisfaction","pride","hope","hopeful",
    "eagerness","gratitude","relief","content","contentment","interest","amusement","wonder","awe",
    "bliss","jubilation","rapture","glee","cheer","optimism","empathy","confidence","calm","serenity",
    "pleasure","excitement","excited"]);
  const EMOTION_NEG = new Set(["anger","angry","annoyed","annoyance","fear","afraid","grief","sadness",
    "despair","anguish","misery","dread","terror","rage","fury","disgust","contempt","hatred","jealousy",
    "envy","shame","guilt","embarrassment","humiliation","anxiety","distress","frustration","bitterness",
    "gloom","alarm","panic","panicked","irritation","irritated","boredom","bored","nervousness","worry",
    "grumpiness","unhappy","sorrow","despondent","tormented","isolation","loneliness","disappointment"]);
  const EMOTION_NEU = new Set(["remembering","nostalgia","longing","acceptance","resignation","indifference"]);
  const SKILL_LEVELS = [["grand master",14],["high master",13],["dabbling",0],["novice",1],
    ["adequate",2],["competent",3],["skilled",4],["proficient",5],["talented",6],["adept",7],
    ["expert",8],["professional",9],["accomplished",10],["great",11],["master",12],["legendary",15]];

  function colorizeEmotionLine(line) {
    return escapeHtml(line).replace(/[A-Za-z]+/g, w => {
      const lw = w.toLowerCase();
      if (EMOTION_POS.has(lw)) return `<span class="em-pos">${w}</span>`;
      if (EMOTION_NEG.has(lw)) return `<span class="em-neg">${w}</span>`;
      if (EMOTION_NEU.has(lw)) return `<span class="em-neu">${w}</span>`;
      return w;
    });
  }
  function colorizeSkillLine(line) {
    const lower = String(line).toLowerCase();
    for (const [name, lvl] of SKILL_LEVELS) {
      if (lower.startsWith(name + " ")) return `<span class="sk-${lvl}">${escapeHtml(line)}</span>`;
    }
    return escapeHtml(line);
  }
  function colorizeUnitLine(line, tab, detail) {
    if (tab === "Thoughts") return colorizeEmotionLine(line);
    if (tab === "Skills") return colorizeSkillLine(line);
    if (detail === "Needs" || /^Unmet need:/.test(line))
      return `<span class="unit-need-line">${escapeHtml(line)}</span>`;
    return escapeHtml(line);
  }

  function renderUnitOverviewLines(unit, lines, tab = "Overview", detail = "") {
    const list = Array.isArray(lines) ? lines : [];
    if (!list.length) return "";
    return list.map(line => `<div class="unit-cell-line${classForUnitLine(tab, detail, line)}">${colorizeUnitLine(line, tab, detail)}</div>`).join("");
  }

  function showUnitSheet(data) {
    selectedUnitData = data;
    activeUnitTab = "Overview";
    activeUnitDetailTab = null;
    renderUnitSheet();
  }

  function renderUnitSheet() {
    const data = selectedUnitData || {};
    const unit = data.unit || {};
    const actions = Array.isArray(unit.actions) ? unit.actions : [];
    const flags = Array.isArray(unit.flags) ? unit.flags : [];
    const statusLines = Array.isArray(unit.statusLines) && unit.statusLines.length
      ? unit.statusLines : [unit.status || "Healthy"];
    const sexSymbol = unit.sex === "female" ? "&#9792;" : (unit.sex === "male" ? "&#9794;" : "?");
    const tabRow = ["Relations", "Groups", "Military", "Thoughts", "Personality"];
    const subTabRow = ["Overview", "Items", "Health", "Skills", "Rooms", "Labor"];
    const detailTabs = unitDetailTabs(activeUnitTab);
    if (detailTabs.length && !detailTabs.includes(activeUnitDetailTab))
      activeUnitDetailTab = detailTabs[0];
    if (!detailTabs.length)
      activeUnitDetailTab = null;
    const tabButton = label => `<button class="unit-tab${activeUnitTab === label ? " active" : ""}" data-unit-tab="${escapeHtml(label)}">${escapeHtml(label)}</button>`;
    const detailButton = label => `<button class="unit-tab${activeUnitDetailTab === label ? " active" : ""}" data-unit-detail-tab="${escapeHtml(label)}">${escapeHtml(label)}</button>`;
    const flagHtml = flags.length
      ? `<div class="unit-flags">${flags.map(flag => `<span class="unit-flag">${escapeHtml(flag)}</span>`).join("")}</div>`
      : "";
    const actionHtml = actions.map(action => `
      <div class="unit-action-row${action.available ? "" : " muted"}">
        <span><b>${escapeHtml(action.hotkey || "")}</b>: ${escapeHtml(action.label || "")}</span>
        <strong>${escapeHtml(action.value || "")}</strong>
      </div>
    `).join("");
    const training = unit.training ? `<div class="subtle">${escapeHtml(unit.training)}</div>` : "";
    const statusHtml = statusLines.map(line =>
      `<div class="${line === "Healthy" ? "healthy" : "condition"}">${escapeHtml(line)}</div>`
    ).join("");
    const relationLines = unitOverviewLines(unit, "overviewRelationLines");
    const traitLines = unitOverviewLines(unit, "overviewTraitLines", statusLines);
    const positionLines = unitOverviewLines(unit, "overviewPositionLines", flags);
    const squadLines = unitOverviewLines(unit, "overviewSquadLines");
    const skillLines = unitOverviewLines(unit, "overviewSkillLines");
    const needLines = unitOverviewLines(unit, "overviewNeedLines");
    const memoryLines = unitOverviewLines(unit, "overviewMemoryLines", unit.thoughtLines || []);
    const overviewGrid = `
      <div class="unit-grid">
        <div class="unit-cell">
          <div>${escapeHtml(unit.age || "Age unknown")}, ${sexSymbol}</div>
          ${training}
          ${renderUnitOverviewLines(unit, relationLines, "Relations")}
        </div>
        <div class="unit-cell status">${renderUnitOverviewLines(unit, traitLines, "Personality", "Traits") || statusHtml}</div>
        <div class="unit-cell">
          <div class="unit-cell-title">Health</div>
          <div class="unit-cell-line">${escapeHtml(unit.bodySummary || "No health problems")}</div>
        </div>
        <div class="unit-cell">${renderUnitOverviewLines(unit, positionLines, "Groups")}</div>
        <div class="unit-cell"></div>
        <div class="unit-cell">${renderUnitOverviewLines(unit, squadLines, "Military", "Squad")}</div>
        <div class="unit-cell">${renderUnitOverviewLines(unit, skillLines, "Skills")}</div>
        <div class="unit-cell">
          ${renderUnitOverviewLines(unit, needLines, "Personality", "Needs")}
          <div class="unit-actions-box">${actionHtml}</div>
        </div>
        <div class="unit-cell wide">${renderUnitOverviewLines(unit, memoryLines, "Thoughts") || `<div class="unit-cell-line subtle">No recent thoughts recorded.</div>`}</div>
      </div>
    `;
    const detailHtml = detailTabs.length
      ? `<div class="unit-detail-tabs">${detailTabs.map(detailButton).join("")}</div>`
      : "";
    const bodyHtml = activeUnitTab === "Overview" ? overviewGrid : renderUnitTabBody(unit, activeUnitTab, activeUnitDetailTab);
    selection.className = "visible unit-sheet-panel";
    selection.innerHTML = `
      <div class="unit-sheet">
        <button class="unit-close-button" data-unit-close title="Close">X</button>
        <div class="unit-sheet-header">
          ${unitPortraitMarkup(unit)}
          <div>
            <div class="unit-name-line">${escapeHtml(unit.name || data.title || "Unit")}</div>
            <div class="unit-meta-line">${escapeHtml(unit.profession || "")}${unit.race ? `, ${escapeHtml(unit.race)}` : ""}</div>
            <div class="unit-job-line">${escapeHtml(unit.currentJob || "")}</div>
            ${flagHtml}
          </div>
          <div class="unit-header-actions">
            <button class="unit-icon-button" data-unit-follow title="Move camera to this unit">&#127909;</button>
            <button class="unit-icon-button" data-unit-generate-portrait title="Generate portrait">&#128444;</button>
          </div>
        </div>
        <div class="unit-tabs">
          ${tabRow.map(tabButton).join("")}
        </div>
        <div class="unit-subtabs">
          ${subTabRow.map(tabButton).join("")}
        </div>
        ${detailHtml}
        ${bodyHtml}
        <div class="unit-sheet-footer">Unit id: ${escapeHtml(unit.id ?? "")} | Tile: ${escapeHtml(data.tile?.x ?? "")}, ${escapeHtml(data.tile?.y ?? "")}, ${escapeHtml(data.tile?.z ?? "")}</div>
      </div>
    `;
    selection.querySelectorAll("[data-unit-tab]").forEach(button => {
      button.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        activeUnitTab = button.dataset.unitTab || "Overview";
        activeUnitDetailTab = null;
        renderUnitSheet();
        focusPage();
      });
    });
    selection.querySelectorAll("[data-unit-detail-tab]").forEach(button => {
      button.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        activeUnitDetailTab = button.dataset.unitDetailTab || null;
        renderUnitSheet();
        focusPage();
      });
    });
    const follow = selection.querySelector("[data-unit-follow]");
    if (follow) {
      follow.addEventListener("click", async event => {
        event.preventDefault();
        event.stopPropagation();
        const tile = data.tile || {};
        const pos = {
          x: Number(tile.x),
          y: Number(tile.y),
          z: Number(tile.z)
        };
        if (Number.isFinite(pos.x) && Number.isFinite(pos.y) && Number.isFinite(pos.z))
          await centerAndFlashMapPos(pos);
        else
          focusPage();
      });
    }
    const portraitButton = selection.querySelector("[data-unit-generate-portrait]");
    if (portraitButton) {
      portraitButton.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        generateUnitPortrait(unit, portraitButton);
      });
    }
    const close = selection.querySelector("[data-unit-close]");
    if (close) {
      close.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        closeSelection();
        focusPage();
      });
    }
  }

  function unitDetailTabs(tab) {
    return ({
      Personality: ["Traits", "Values", "Preferences", "Needs"],
      Labor: ["Work details", "Workshops", "Locations", "Work animals"],
      Health: ["Status", "Wounds", "Treatment", "History", "Description"],
      Military: ["Squad", "Uniform", "Kills"]
    }[tab] || []);
  }

  function classForUnitLine(tab, detail, line) {
    const lower = String(line || "").toLowerCase();
    if (tab === "Health" && !["healthy", "no health problems", "no wounds.", "no treatment scheduled.", "no health history."].includes(lower))
      return " condition";
    if ((tab === "Personality" || tab === "Health") && /poor|weak|meager|no natural|discouraged|worry|anger|quick to tire|susceptible|clumsy|fragile|slow to heal/.test(lower))
      return " condition";
    if ((tab === "Personality" || tab === "Health") && /great|good|strong|agile|quick to heal|resistant|friendly|curious|vivid|empathy/.test(lower))
      return " positive";
    return "";
  }

  function renderUnitTextLines(unit, tab, detail, lines) {
    const rendered = lines.length ? lines.map(line =>
      `<p class="unit-text-line${classForUnitLine(tab, detail, line)}">${colorizeUnitLine(line, tab, detail)}</p>`
    ).join("") : `<p class="unit-text-line unit-list-empty">No entries.</p>`;
    return `<div class="unit-text-block">${rendered}</div>`;
  }

  function renderUnitTabBody(unit, tab, detail) {
    const map = {
      Relations: unit.relationLines,
      Groups: unit.groupLines,
      Thoughts: unit.thoughtLines,
      Items: unit.inventoryLines,
      Skills: unit.skillLines,
      Rooms: detail === "Assigned" ? unit.roomAssignmentLines : unit.roomLines,
      Health: {
        Status: unit.healthStatusLines,
        Wounds: unit.healthWoundLines,
        Treatment: unit.healthTreatmentLines,
        History: unit.healthHistoryLines,
        Description: unit.healthDescriptionLines
      }[detail] || unit.healthLines,
      Labor: {
        "Work details": unit.laborWorkDetailLines,
        Workshops: unit.laborWorkshopLines,
        Locations: unit.laborLocationLines,
        "Work animals": unit.laborWorkAnimalLines
      }[detail] || unit.laborLines,
      Military: {
        Squad: unit.militarySquadLines,
        Uniform: unit.militaryUniformLines,
        Kills: unit.militaryKillLines
      }[detail] || unit.militaryLines,
      Personality: {
        Traits: unit.personalityTraitLines,
        Values: unit.personalityValueLines,
        Preferences: unit.personalityPreferenceLines,
        Needs: unit.personalityNeedLines
      }[detail] || unit.personalityLines
    };
    const lines = Array.isArray(map[tab]) ? map[tab] : [];
    if ((tab === "Personality" && detail === "Traits") || (tab === "Health" && detail === "Description"))
      return renderUnitTextLines(unit, tab, detail, lines);
    const rendered = lines.length ? lines.map(line => {
      const cls = classForUnitLine(tab, detail, line);
      return `<div class="unit-list-row${cls}">${colorizeUnitLine(line, tab, detail)}</div>`;
    }).join("") : `<div class="unit-list-row unit-list-empty">No entries.</div>`;
    return `<div class="unit-list-grid">${rendered}</div>`;
  }

  function escapeHtml(value) {
    return String(value).replace(/[&<>"']/g, ch => ({
      "&": "&amp;",
      "<": "&lt;",
      ">": "&gt;",
      "\"": "&quot;",
      "'": "&#39;"
    }[ch]));
  }

  function ordinal(n) {
    if (n % 100 >= 11 && n % 100 <= 13) return `${n}th`;
    switch (n % 10) {
      case 1: return `${n}st`;
      case 2: return `${n}nd`;
      case 3: return `${n}rd`;
      default: return `${n}th`;
    }
  }

  async function loadHud() {
    try {
      const response = await fetch(`/hud?player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
      if (!response.ok) throw new Error("hud failed");
      currentHud = await response.json();
      renderHud(currentHud);
      renderZoneOverlay();
    } catch (_) {}
  }

  // While the host is placing dig/build/chop orders, the plugin holds the last
  // independent frame (it can't safely re-render the map mid-interaction). Show a
  // clear banner so remote players know why their view paused, instead of a silent freeze.
  function showHostBusyBanner(show) {
    let el = document.getElementById("hostBusyBanner");
    if (!el) {
      el = document.createElement("div");
      el.id = "hostBusyBanner";
      el.style.cssText = "position:fixed;top:14px;left:50%;transform:translateX(-50%);z-index:9999;" +
        "display:none;align-items:center;gap:9px;padding:9px 18px;color:#ffe9b0;background:rgba(21,21,21,0.94);" +
        "box-shadow:0 0 0 2px #d89b27 inset,0 4px 18px rgba(0,0,0,0.5);font:700 14px ui-monospace,Consolas,monospace;" +
        "letter-spacing:0.2px;pointer-events:none;";
      el.innerHTML = '<span style="width:9px;height:9px;border-radius:50%;background:#ffb12e;' +
        'box-shadow:0 0 7px #ffb12e;animation:hbbPulse 1s ease-in-out infinite;"></span>' +
        'Paused: Host is setting dig orders &mdash; please wait';
      document.body.appendChild(el);
      if (!document.getElementById("hbbStyle")) {
        const st = document.createElement("style");
        st.id = "hbbStyle";
        st.textContent = "@keyframes hbbPulse{0%,100%{opacity:1}50%{opacity:0.3}}";
        document.head.appendChild(st);
      }
    }
    el.style.display = show ? "flex" : "none";
  }

  const moodCounts = Array.from(document.querySelectorAll("#moods .mood-n"));
  function renderHud(hud) {
    showHostBusyBanner(!!hud.hostInteracting);
    // Shade whichever of pause (Ã¢ÂÅ¡Ã¢ÂÅ¡) / play (Ã¢â€“Â¶) matches the current game state, so it's obvious.
    const isPaused = !!hud.paused;
    const pauseBtn = document.querySelector('#pauseRow [data-action="pause"]');
    const playBtn = document.querySelector('#pauseRow [data-action="play"]');
    if (pauseBtn) pauseBtn.classList.toggle("sb-active", isPaused);
    if (playBtn) playBtn.classList.toggle("sb-active", !isPaused);
    hudEls.fortName.textContent = hud.fort?.name || "Fortress";
    hudEls.siteName.textContent = hud.fort?.site || "Site";
    hudEls.rankName.textContent = hud.fort?.rank || "Outpost";
    hudEls.population.textContent = hud.population?.total ?? 0;
    const happ = Array.isArray(hud.happiness) ? hud.happiness : [];
    moodCounts.forEach((el, i) => {
      const n = happ[i] || 0;
      el.textContent = n;
      el.parentElement.style.opacity = n ? "1" : "0.3";
    });
    hudEls.food.textContent = `~${hud.stocks?.food ?? 0}`;
    hudEls.drink.textContent = `~${hud.stocks?.drink ?? 0}`;
    hudEls.dateDay.textContent = ordinal(hud.date?.day || 1);
    hudEls.dateMonth.textContent = hud.date?.monthName || "Granite";
    hudEls.dateSeason.textContent = hud.date?.season || "Early Spring";
    hudEls.dateYear.textContent = hud.date?.year ?? 0;
    hudEls.elevation.textContent = `Elevation ${hud.elevation ?? 0}`;
    const moonIcon = Math.max(0, Math.min(9, Number(hud.date?.moonIcon ?? 0)));
    hudEls.moon.style.backgroundPosition = `-${moonIcon * 32}px 0`;
    renderMinimap(hud);
  }

  // Minimap category colors (indices 0..14 match the backend minimap_color_for_tile buckets:
  // 0 soil 1 sand 2 rockFloor 3 darkFloor 4 stoneWall 5 conFloor 6 built 7 water 8 magma
  // 9 grass 10 trees 11 dryGrass 12 ice 13 mountain 14 sky).
  const MM_COLORS = ["#7a5a32","#c8803c","#6b6b6b","#3a3a3a","#4a4640","#8a8270","#b0a080",
    "#3b6fd4","#d8401a","#4f9a3a","#2f6b27","#9b8b3a","#e8f0f8","#9a948c","#14171c"];
  function mmDecode(ch) {
    if (ch >= 48 && ch <= 57) return ch - 48;      // '0'..'9'
    if (ch >= 97 && ch <= 101) return 10 + ch - 97; // 'a'..'e'
    return 14;
  }
  function renderMinimap(hud) {
    const mm = hud.minimap || {};
    const W = Math.max(1, mm.w | 0), H = Math.max(1, mm.h | 0);
    const cells = typeof mm.cells === "string" ? mm.cells : "";
    const cv = hudEls.minimap;
    if (!cv || !cv.getContext) return;
    // Aspect-correct: fixed display width, height follows the map aspect.
    const dispW = 164, dispH = Math.max(24, Math.round(dispW * H / W));
    if (cv.width !== dispW || cv.height !== dispH) { cv.width = dispW; cv.height = dispH; }
    cv.style.height = dispH + "px";
    const ctx = cv.getContext("2d");
    const cw = dispW / W, ch = dispH / H;
    ctx.fillStyle = "#1b1b1b";
    ctx.fillRect(0, 0, dispW, dispH);
    for (let gy = 0; gy < H; gy++) {
      for (let gx = 0; gx < W; gx++) {
        const c = mmDecode(cells.charCodeAt(gy * W + gx));
        ctx.fillStyle = MM_COLORS[c] || "#1b1b1b";
        ctx.fillRect(Math.floor(gx * cw), Math.floor(gy * ch), Math.ceil(cw), Math.ceil(ch));
      }
    }
    // Viewport box: where this player's camera is looking, on the whole-map minimap.
    const cam = hud.camera || { x: 0, y: 0 };
    const map = hud.map || { w: 1, h: 1 };
    const vp = hud.viewport || { w: 1, h: 1 };
    const bx = (cam.x / Math.max(1, map.w)) * dispW;
    const by = (cam.y / Math.max(1, map.h)) * dispH;
    const bw = Math.max(3, (vp.w / Math.max(1, map.w)) * dispW);
    const bh = Math.max(3, (vp.h / Math.max(1, map.h)) * dispH);
    ctx.lineWidth = 1;
    ctx.strokeStyle = "rgba(0,0,0,0.7)";
    ctx.strokeRect(bx + 0.5, by + 0.5, bw, bh);
    ctx.strokeStyle = "#ffdf4d";
    ctx.strokeRect(bx + 1.5, by + 1.5, bw - 2, bh - 2);
  }

  const ALERT_NAMES = [
    "General", "Era Change", "Underground", "Migrants", "Monster", "Ambush",
    "Trade", "Noble", "Animal", "Birth", "Mood", "Labor Change", "Military",
    "Marriage", "Berserk", "Martial Trance", "Emotion", "Stress",
    "Art Defacement", "Masterpiece", "Job Failed", "Death", "Ghost",
    "Undead Attack", "Weather", "Vermin", "Curious Guzzler",
    "Research Breakthrough", "Guest Arrival", "Holdings", "Rumor",
    "Agreement", "Crime", "Deity Curse", "Combat", "Sparring", "Hunting"
  ];
  const DF_COLORS = [
    ["#000000", "#555555"],
    ["#0000aa", "#5555ff"],
    ["#00aa00", "#55ff55"],
    ["#00aaaa", "#55ffff"],
    ["#aa0000", "#ff5555"],
    ["#aa00aa", "#ff55ff"],
    ["#aa5500", "#ffff55"],
    ["#aaaaaa", "#ffffff"]
  ];
  function alertName(alert) {
    const i = Number(alert?.type);
    if (Number.isFinite(i) && ALERT_NAMES[i]) return ALERT_NAMES[i];
    return String(alert?.typeKey || "Announcement").replace(/_/g, " ").toLowerCase()
      .replace(/\b\w/g, ch => ch.toUpperCase());
  }
  function alertIconStyle(iconIndex) {
    const i = Math.max(0, Math.min(36, Number(iconIndex) || 0));
    return `background-position:0 -${i * 32}px`;
  }
  function dfTextColor(report) {
    const row = DF_COLORS[Math.max(0, Math.min(7, Number(report?.color) || 0))] || DF_COLORS[7];
    return row[report?.bright ? 1 : 0];
  }
  function reportText(report) {
    if (!report || !report.text) return "";
    const suffix = Number(report.repeatCount) > 0 ? ` x${Number(report.repeatCount) + 1}` : "";
    return `${report.text}${suffix}`;
  }
  function alertDisplayLines(alert) {
    const lines = [];
    (Array.isArray(alert?.reports) ? alert.reports : []).forEach(report => {
      const text = reportText(report);
      if (text) lines.push({ text, color: dfTextColor(report), report });
    });
    (Array.isArray(alert?.unitReports) ? alert.unitReports : []).forEach(ref => {
      const hasLines = Array.isArray(ref.reports) && ref.reports.some(r => r?.text);
      if (!hasLines && ref.unitName)
        lines.push({ text: `${ref.unitName} (${String(ref.categoryKey || "report").toLowerCase()})`, unit: true });
    });
    return lines;
  }
  async function loadNotifications() {
    try {
      const response = await fetch(`/notifications?player=${encodeURIComponent(player)}&t=${Date.now()}`, { cache: "no-store" });
      if (!response.ok) throw new Error("notifications failed");
      notificationState = await response.json();
      renderAlertStack();
      if (notificationsPanelIsOpen())
        renderNotificationsPanel(notificationFilterType, { preserveScroll: true, skipIfSame: true });
    } catch (_) {}
  }
  function renderAlertStack() {
    const alerts = Array.isArray(notificationState?.alerts) ? notificationState.alerts : [];
    alertStack.innerHTML = alerts.map(alert => `
      <button class="alert-button${pinnedAlertKey === alert.dismissKey ? " pinned" : ""}"
        data-alert-key="${escapeHtml(alert.dismissKey || "")}"
        title="${escapeHtml(alertName(alert))}"
        style="${alertIconStyle(alert.iconIndex)}"></button>`).join("");
    alertStack.style.display = alerts.length ? "flex" : "none";
    alertStack.querySelectorAll(".alert-button").forEach(button => {
      const alert = alerts.find(a => a.dismissKey === button.dataset.alertKey);
      if (!alert) return;
      button.addEventListener("mouseenter", () => showAlertPopup(alert, button, false));
      button.addEventListener("mouseleave", () => {
        if (pinnedAlertKey !== alert.dismissKey) hideAlertPopup();
      });
      button.addEventListener("click", event => {
        event.preventDefault();
        event.stopPropagation();
        recenterOnAlert(alert);
        pinnedAlertKey = alert.dismissKey || null;
        showAlertPopup(alert, button, true);
        renderAlertStack();
      });
      button.addEventListener("contextmenu", event => {
        event.preventDefault();
        event.stopPropagation();
        dismissAlert(alert);
      });
    });
    if (pinnedAlertKey && !alerts.some(a => a.dismissKey === pinnedAlertKey)) {
      pinnedAlertKey = null;
      hideAlertPopup();
    }
  }
  function showAlertPopup(alert, anchor, pinned) {
    const lines = alertDisplayLines(alert);
    const rows = lines.length
      ? lines.map(line => `<div class="alert-line${line.unit ? " alert-unit-line" : ""}" style="${line.color ? `color:${line.color}` : ""}">${escapeHtml(line.text)}</div>`).join("")
      : `<div class="alert-line alert-unit-line">${escapeHtml(alertName(alert))}</div>`;
    alertPopup.innerHTML = `
      <div class="alert-help">Left click to recenter. &nbsp;Right click to dismiss.</div>
      ${rows}`;
    const rect = anchor.getBoundingClientRect();
    const left = Math.min(rect.right + 4, Math.max(8, window.innerWidth - alertPopup.offsetWidth - 8));
    alertPopup.style.left = `${left}px`;
    alertPopup.style.top = `${Math.max(58, Math.min(rect.top + 1, window.innerHeight - 120))}px`;
    alertPopup.style.display = "block";
    if (pinned) {
      alertPopup.oncontextmenu = event => {
        event.preventDefault();
        dismissAlert(alert);
      };
    }
  }
  function hideAlertPopup() {
    alertPopup.style.display = "none";
    alertPopup.oncontextmenu = null;
  }
  function alertTarget(alert) {
    if (alert?.target && Number.isFinite(Number(alert.target.x)) &&
        Number.isFinite(Number(alert.target.y)) && Number.isFinite(Number(alert.target.z)))
      return alert.target;
    const report = (Array.isArray(alert?.reports) ? alert.reports : []).find(r => r?.pos);
    if (report) return report.pos;
    const ref = (Array.isArray(alert?.unitReports) ? alert.unitReports : []).find(r => r?.pos);
    return ref ? ref.pos : null;
  }
  async function setCameraToMapPos(pos) {
    if (!pos) return false;
    const vp = currentHud?.viewport || { w: 80, h: 50 };
    const x = Math.round(Number(pos.x) - (Number(vp.w) || 80) / 2);
    const y = Math.round(Number(pos.y) - (Number(vp.h) || 50) / 2);
    const z = Math.round(Number(pos.z) || 0);
    resetPanPrediction();
    try {
      await fetch(`/camera?player=${encodeURIComponent(player)}&x=${x}&y=${y}&z=${z}`, {
        method: "POST",
        cache: "no-store"
      });
      await loadHud();
      return true;
    } catch (_) {
      return false;
    }
  }
  async function centerAndFlashMapPos(pos) {
    if (!pos) return;
    closeClientPanel();
    closeSelection();
    pinnedAlertKey = null;
    hideAlertPopup();
    await setCameraToMapPos(pos);
    await flashMapTile(pos);
    focusPage();
  }
  function recenterOnAlert(alert) {
    const target = alertTarget(alert);
    setCameraToMapPos(target);
  }
  async function dismissAlert(alert) {
    const keys = Array.isArray(alert?.dismissKeys) ? alert.dismissKeys.filter(Boolean) : [];
    if (!keys.length && alert?.dismissKey) keys.push(alert.dismissKey);
    if (!keys.length) return;
    try {
      await fetch(`/notification-action?player=${encodeURIComponent(player)}&action=dismiss&keys=${encodeURIComponent(keys.join(","))}`,
        { method: "POST", cache: "no-store" });
    } catch (_) {}
    if (pinnedAlertKey === alert.dismissKey) {
      pinnedAlertKey = null;
      hideAlertPopup();
    }
    await loadNotifications();
  }
  function notificationPanelSignature(filterType) {
    const alerts = Array.isArray(notificationState?.alerts) ? notificationState.alerts : [];
    const recent = Array.isArray(notificationState?.recent) ? notificationState.recent : [];
    return JSON.stringify({
      filterType,
      nextReportId: Number(notificationState?.nextReportId) || 0,
      reportCount: Number(notificationState?.reportCount) || 0,
      alerts: alerts.map(alert => [
        alert.type,
        alert.dismissKey,
        alert.latestReportId,
        (Array.isArray(alert.reportIds) ? alert.reportIds : []).join("."),
        (Array.isArray(alert.dismissKeys) ? alert.dismissKeys : []).join(".")
      ]),
      recent: recent.slice(0, 120).map(report => [report.id, report.repeatCount, report.text])
    });
  }
  function renderNotificationsPanel(filterType = null, options = {}) {
    notificationFilterType = filterType;
    activeInfoPanel = "alerts";
    const signature = notificationPanelSignature(filterType);
    if (options.skipIfSame && signature === lastNotificationPanelSignature)
      return;
    const oldBody = clientPanel.querySelector(".alerts-window .info-body, .info-body");
    const oldScrollTop = options.preserveScroll && oldBody ? oldBody.scrollTop : 0;
    const alerts = Array.isArray(notificationState?.alerts) ? notificationState.alerts : [];
    const shownAlerts = filterType === null ? alerts : alerts.filter(a => Number(a.type) === Number(filterType));
    const recent = Array.isArray(notificationState?.recent) ? notificationState.recent : [];
    const alertRows = shownAlerts.length ? shownAlerts.map(alert => {
      const lines = alertDisplayLines(alert);
      const sub = lines.length ? lines.map(l => l.text).join("  ") : alertName(alert);
      const canCenter = !!alertTarget(alert);
      return `<div class="alerts-row" data-alert-row="${escapeHtml(alert.dismissKey || "")}">
        <div class="alerts-icon" style="${alertIconStyle(alert.iconIndex)}"></div>
        <div>
          <div class="alerts-title">${escapeHtml(alertName(alert))}</div>
          <div class="alerts-sub">${escapeHtml(sub)}</div>
        </div>
        <div class="alerts-actions">
          <button class="alerts-action" data-alert-center="${escapeHtml(alert.dismissKey || "")}"${canCenter ? "" : " disabled title=\"No map location\""}>Center</button>
          <button class="alerts-action" data-alert-dismiss="${escapeHtml(alert.dismissKey || "")}">Dismiss</button>
        </div>
      </div>`;
    }).join("") : `<div class="info-message">No active alerts.</div>`;
    const recentRows = recent.length ? recent.slice(0, 120).map(report => `
      <div class="alerts-row">
        <div class="alerts-icon" style="${alertIconStyle(alertTypeFromReport(report))}"></div>
        <div>
          <div class="alerts-title" style="color:${dfTextColor(report)}">${escapeHtml(reportText(report) || report.typeKey || "Announcement")}</div>
          <div class="alerts-sub">Year ${Number(report.year) || 0}, tick ${Number(report.time) || 0}</div>
        </div>
        <div class="alerts-actions">${report.pos ? `<button class="alerts-action" data-report-center="${report.id}">Center</button>` : ""}</div>
      </div>`).join("") : `<div class="info-message">No reports recorded.</div>`;
    clientPanel.className = "visible info-panel alerts-window";
    clientPanel.innerHTML = `
      <div class="info-window">
        <div class="info-header">
          <div class="info-title">Announcements</div>
          <button class="info-close" data-close-panel>&times;</button>
        </div>
        <div class="info-body">
          <div class="alerts-head">
            <div class="info-muted">${Number(notificationState?.reportCount) || 0} total reports</div>
            ${filterType === null ? "" : `<button class="alerts-action" data-alert-filter-clear>All alerts</button>`}
          </div>
          <div class="alerts-section-title">Active alerts</div>
          <div class="alerts-groups">${alertRows}</div>
          <div class="alerts-section-title">Recent reports</div>
          <div class="alerts-recent">${recentRows}</div>
        </div>
      </div>`;
    clientPanel.querySelector("[data-close-panel]")?.addEventListener("click", closeClientPanel);
    clientPanel.querySelector("[data-alert-filter-clear]")?.addEventListener("click", () => renderNotificationsPanel(null));
    clientPanel.querySelectorAll("[data-alert-center]").forEach(button => {
      button.addEventListener("click", () => {
        const alert = alerts.find(a => a.dismissKey === button.dataset.alertCenter);
        if (alert) centerAndFlashMapPos(alertTarget(alert));
      });
    });
    clientPanel.querySelectorAll("[data-alert-dismiss]").forEach(button => {
      button.addEventListener("click", () => {
        const alert = alerts.find(a => a.dismissKey === button.dataset.alertDismiss);
        if (alert) dismissAlert(alert);
      });
    });
    clientPanel.querySelectorAll("[data-report-center]").forEach(button => {
      button.addEventListener("click", () => {
        const report = recent.find(r => String(r.id) === String(button.dataset.reportCenter));
        if (report?.pos) centerAndFlashMapPos(report.pos);
      });
    });
    const newBody = clientPanel.querySelector(".info-body");
    if (options.preserveScroll && newBody)
      newBody.scrollTop = Math.min(oldScrollTop, Math.max(0, newBody.scrollHeight - newBody.clientHeight));
    lastNotificationPanelSignature = signature;
  }
  function alertTypeFromReport(report) {
    const found = (Array.isArray(notificationState?.alerts) ? notificationState.alerts : [])
      .find(alert => Array.isArray(alert.reportIds) && alert.reportIds.includes(report?.id));
    return found ? found.iconIndex : (Number(report?.alertType) || 0);
  }
  async function openNotificationsPanel(filterType = null) {
    setActiveToolbar("alerts");
    clearBuildPlacement(false);
    activeInfoPanel = "alerts";
    clientPanel.className = "visible info-panel alerts-window";
    clientPanel.innerHTML = `<div class="info-window"><div class="info-body"><div class="info-message">Loading announcements...</div></div></div>`;
    await loadNotifications();
    renderNotificationsPanel(filterType);
  }

  function setActiveToolbar(name) {
    document.querySelectorAll("[data-panel].active").forEach(button => button.classList.remove("active"));
    document.querySelectorAll("[data-panel]").forEach(button => {
      if (button.dataset.panel === name)
        button.classList.add("active");
    });
    refreshToolbarSprites(name);
  }

  function defaultSectionForPanel(name) {
    return ({
      citizens: "creatures",
      labor: "labor",
      locations: "places",
      orders: "tasks",
      workorders: "workorders",
      nobles: "nobles",
      objects: "objects",
      justice: "justice",
      stocks: "stocks"
    }[name] || "creatures");
  }

  function panelForInfoSection(section, fallback = activeInfoPanel || "citizens") {
    return ({
      creatures: "citizens",
      tasks: "orders",
      places: "locations",
      labor: "labor",
      workorders: "workorders",
      nobles: "nobles",
      objects: "objects",
      justice: "justice",
      stocks: "stocks"
    }[section] || fallback);
  }

  function localPanelTitle(name) {
    return ({
      stocks: "Stocks",
      build: "Place Building",
      designate: "Designations",
      dig: "Dig",
      stockpile: "Stockpiles",
      zone: "Zones",
      objects: "Objects",
      justice: "Justice",
      search: "Search",
      alerts: "Announcements",
      hauling: "Hauling",
      settings: "Settings",
      speed: "Speed",
      map: "World Map",
      help: "Help",
      about: "DFHack"
    }[name] || name);
  }

  function renderLocalPanel(name) {
    const hud = currentHud || {};
    const title = localPanelTitle(name);
    const rows = [];
    if (name === "stocks") {
      rows.push(`Food: ~${hud.stocks?.food ?? 0}`);
      rows.push(`Drink: ~${hud.stocks?.drink ?? 0}`);
      rows.push("This is browser-owned; it does not open DF's global Stocks screen.");
    } else if (name === "designate" || name === "dig") {
      rows.push("Next layer: paint rectangles in this browser view and commit DF designations.");
    } else {
      rows.push("Panel shell is independent. Its real DF-backed controls are the next wiring step.");
    }
    clientPanel.innerHTML = `
      <div class="kind">client ui</div>
      <h1>${escapeHtml(title)}</h1>
      ${rows.map(row => `<div class="line">${escapeHtml(row)}</div>`).join("")}
    `;
    clientPanel.classList.remove("info-panel");
    clientPanel.classList.add("visible");
  }

