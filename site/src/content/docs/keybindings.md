---
title: "Key Bindings"
description: "Every default key, and how to add, change or remove them."
section: Customizing
order: 2
---

## The defaults

| Keys | Does |
|---|---|
| Super+T, Alt+Return | open a terminal (`$TERMINAL`, or the [GEM one](/apps/terminal/)) |
| Super+B | open the default browser |
| Print | screenshot of the whole screen |
| Shift+Print | screenshot of an area you drag out |
| Ctrl+Print | drag out an area, or click a window to take it |
| Alt+Print | screenshot of the focused window |
| Volume keys | volume up, down, mute; mic mute |
| Super+Q | close the focused window |
| Super+Z | full size (toggle) |
| Super+F | fullscreen (toggle) |
| Super+Tab, Alt+Tab | cycle windows (Shift goes backwards) |
| Super+W | the [overview](/docs/window-mode/#the-overview): every window at once |
| Super+Arrows | focus the neighbouring tile or column |
| Super+Ctrl+Arrows | tiling: swap with the window that way; scrolling: move the column or the window |
| Super+Shift+Left/Right | tiling: push the window into the other column |
| Super+[ / Super+] | scrolling: consume or expel the window |
| Super+R | scrolling: column width ⅓, ½, ⅔ |
| Super+1..9, Alt+1..9 | switch workspace |
| Super+Shift+1..9, Alt+Shift+1..9 | move the focused window to a workspace |
| Super+L | [lock the screen](/docs/locking/) |
| Alt+Escape | quit GemWM |

While Super is held, the focused window gets a thick pink border (see
[the focus border](/docs/desktop/#the-focus-border)).

## Changing them

Keys are set in `~/.config/gemwm/config`. You only list what you change: each
line adds or replaces one binding, and `none` removes one.

```
[keys]
Super+Q = none                       # unbind
Super+Return = exec foot             # run a program
Super+W = close-window focused
Super+Page_Down = workspace next
Super+Page_Up = workspace prev
Ctrl+Alt+Delete = quit
```

Modifiers are `Super`, `Alt`, `Ctrl` and `Shift`; keys use xkb names (`Q`,
`Tab`, `Return`, `Page_Down`, `F1`, `1`...).

An action is any [`gemwm msg`](/docs/scripting/) command, plus `exec <command>`
and `quit`. So anything you can script, you can bind:

```
[keys]
Super+M = mode toggle                # window, tiling, scrolling, in turn
Super+S = mode scrolling
Super+O = overview
Super+Shift+Return = exec gemweb --app https://mail.example.com --name Mail
```

Apply changes with `gemwm msg reload-config`.
