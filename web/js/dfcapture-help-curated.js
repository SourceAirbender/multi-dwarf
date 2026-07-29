// dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
// Copyright (C) 2026 Gabriel Rios <grios019@gmail.com>
// Copyright (C) 2026 Jake Taplin
//
// SPDX-License-Identifier: AGPL-3.0-only

// HELP-TOOLTIPS: curated one-line supplements for the ? help reference.
//
// Terse captions can receive one clarifying line beneath the generated headline. Keys match the
// generated surface and tooltip text exactly.
(function (root) {
  "use strict";

  var DFHelpCurated = {
    version: "help-curated v1",
    // surface id -> { exact harvested text : curated one-liner }
    notes: {
      tools: {
        "Justice.": "Review crime reports, convict wrongdoers, and interrogate suspects.",
        "Labor management.": "Choose which jobs each dwarf is allowed to perform.",
        "Military and squads.": "Form squads, set uniforms, and give military orders.",
        "World and civilizations.": "See the world map, neighbors, and launch missions or raids.",
        "Place information.": "Guildhalls, temples, hospitals, taverns and other fort locations.",
        "Nobles and administrators.": "Assign the manager, bookkeeper, sheriff, and other officials.",
        "Fortress job list.": "Every job dwarves are currently doing or waiting to do.",
      },
      topbar: {
        "Fortress activity": "A dashboard of what your fort has been building and who has been busy.",
        "Fortress vote": "Multiplayer players propose and vote on fortress decisions.",
        "Players / lobby": "See who is connected and jump to their camera.",
      },
    },
  };

  root.DFHelpCurated = DFHelpCurated;
  if (typeof module !== "undefined" && module.exports) module.exports = DFHelpCurated;
})(typeof window !== "undefined" ? window : (typeof globalThis !== "undefined" ? globalThis : this));
