---
title: "Configuration Files"
short: "Files"
description: "Where every setting, and everything GemWM keeps, lives."
section: Going Further
order: 5
---

Every file is optional: GemWM and its programs start with their defaults, and
you write only what you change.

## Settings

| File | What |
|---|---|
| `~/.config/gemwm/config` | GemWM: [desktop](/docs/desktop/), [keys](/docs/keybindings/), layout, highlight, animation, font, clock, battery, AI |
| `~/.config/gemwm/menu` | [the menus and menu apps](/docs/menu-bar/) |
| `~/.config/gemwm/env` | environment variables for the session (`TERMINAL=...`) |
| `/etc/gemwm/greeter.conf` | the [login screen](/docs/login-screen/)'s desktop |
| `~/.config/gemweb/settings` | [GemWeb](/apps/gemweb/): home page, search, ad blocking, passwords, zoom |
| `~/.config/gemmail/settings` | [GemMail](/apps/gemmail/): the account |
| `~/.config/gemmail/categories` | GemMail's AI categories |
| `~/.config/gemwrite/settings.ini` | [GemWrite](/apps/gemwrite/): your name, the spelling language |
| `~/.config/foot/foot.ini` | the [terminal](/apps/terminal/): your own settings, under its theme |

## What GemWM keeps

| Where | What |
|---|---|
| `~/.local/state/gemwm.log` | the session's log |
| `~/.local/state/gemwm/notifications` | [notification](/apps/notifications/) history |
| `~/.local/share/gemweb/` | GemWeb's cookies, site data, history and ad-block rules |
| `~/.local/state/gemweb/zoom` | zoom remembered per site |
| `~/.cache/gemmail/` | GemMail's mail, for offline reading and a quick start |
| `~/.local/share/gemwm/crossword/` | [crossword](/apps/crossword/) puzzles and progress |
| `~/Pictures/Screenshots/` | [screenshots](/apps/screenshots/) (or `$GEMWM_SCREENSHOT_DIR`) |
