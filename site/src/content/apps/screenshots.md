---
title: "Screenshots"
description: "The whole screen, an area, or a window, saved and copied to the clipboard."
kind: utility
icon: camera
command: "gemwm-screenshot"
order: 7
---

| Keys | Takes |
|---|---|
| Print | the whole screen |
| Shift+Print | an area you drag out |
| Alt+Print | the focused window |

**Options > Print Screen** takes one too. Screenshots are taken by
`gemwm-screenshot [screen|area|window]`, which needs `grim`, and `slurp` for
areas.

They're saved to `~/Pictures/Screenshots` (or `$GEMWM_SCREENSHOT_DIR`), copied
to the clipboard if `wl-copy` is installed, and announced by a
[notification](/apps/notifications/).
