#include "web_assets.h"

namespace dfcapture_public {

const char* index_html() {
    return R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>dfcapture</title>
  <style>
    :root {
      color-scheme: dark;
      --bg: #111;
      --panel: #191919;
      --line: #c28713;
      --text: #f8f0dc;
      --muted: #b9afa2;
    }

    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      background: var(--bg);
      color: var(--text);
      font: 14px/1.4 Consolas, "Liberation Mono", monospace;
    }

    .topbar {
      height: 48px;
      display: flex;
      align-items: center;
      gap: 16px;
      padding: 0 14px;
      border-bottom: 2px solid var(--line);
      background: #090909;
    }

    .brand {
      color: #ffd15a;
      font-weight: 700;
    }

    .stage {
      height: calc(100vh - 48px);
      display: grid;
      place-items: center;
      padding: 24px;
    }

    .panel {
      width: min(720px, 100%);
      border: 2px solid var(--line);
      background: var(--panel);
      padding: 20px;
    }

    h1 {
      margin: 0 0 10px;
      font-size: 18px;
      letter-spacing: 0;
    }

    p {
      margin: 8px 0;
      color: var(--muted);
    }

    code {
      color: #ffd15a;
    }
  </style>
</head>
<body>
  <header class="topbar">
    <div class="brand">dfcapture</div>
    <div>public reconstruction shell</div>
  </header>
  <main class="stage">
    <section class="panel">
      <h1>Browser shell is running</h1>
      <p>This is the first compileable milestone: DFHack plugin, local HTTP server, and web shell.</p>
      <p>Next milestones add frame capture, independent cameras, and UI actions as separate commits.</p>
      <p>Health endpoint: <code>/health</code></p>
    </section>
  </main>
</body>
</html>)HTML";
}

} // namespace dfcapture_public
