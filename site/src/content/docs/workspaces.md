---
title: "Workspaces"
description: "Dynamic workspaces, shown in the middle of the menu bar."
section: Modes
order: 4
---

The middle of the menu bar shows the workspaces, `1 | 2 | +`. Click a number
to switch, or `+` to add one.

Workspaces are dynamic: an empty workspace is removed when you leave it, and
the ones after it move up a number. So there are only ever as many as you're
using.

| Keys | Does |
|---|---|
| Super+1..9, Alt+1..9 | switch workspace |
| Super+Shift+1..9, Alt+Shift+1..9 | move the focused window there |

The number keys, with or without Shift, accept one past the last workspace,
which creates a new one. A window's application menu has **Move to
Workspace** too.

From a script:

```
gemwm msg workspace 2             # also: new, next, prev
gemwm msg move-window focused 3   # a window id, or focused; a number, or new
gemwm msg workspaces              # what's where, as JSON
```
