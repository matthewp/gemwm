---
title: "Scripting with gemwm msg"
short: "Scripting"
description: "Control GemWM from scripts and key bindings, and follow what changes, in JSON."
section: Going Further
order: 1
---

GemWM listens on a control socket (`$GEMWM_SOCKET`, set for everything it
starts). `gemwm msg` sends one command and prints the JSON reply.

## Commands

```
gemwm msg workspaces              # list workspaces and their windows
gemwm msg windows                 # id, title, app_id, workspace, focused,
                                  # tiled, and the frame's x/y/width/height
gemwm msg workspace 2             # also: new, next, prev
gemwm msg move-window 12 3        # window id or "focused"; 3 or "new"
gemwm msg focus-window 12         # switches to its workspace if needed
gemwm msg maximize focused        # or an id; toggles, like Super+Z
gemwm msg fullscreen focused      # or an id; toggles, like Super+F
gemwm msg close-window focused
gemwm msg cycle-windows next      # or prev, like Super+Tab
gemwm msg overview                # shows every window, or stops; Super+W
gemwm msg mode scrolling          # window, tiling, toggle; alone: report
gemwm msg focus-direction left    # right, up, down (tiling, scrolling)
gemwm msg move-direction right    # scrolling: move column / window;
                                  # tiling: swap with that neighbour
gemwm msg push-direction left     # tiling: into the other column
gemwm msg consume-or-expel left   # scrolling
gemwm msg column-width 0.33       # scrolling: or "cycle"
gemwm msg exec foot               # run a program
gemwm msg reload-config           # re-read ~/.config/gemwm/config
gemwm msg quit
gemwm msg lock                    # lock the screen, as Super+L
gemwm msg lock-status             # {"locked":..., "shown":...}
gemwm msg subscribe               # stream workspaces/windows/mode events
```

Window ids never change while a window is open; workspace numbers are
positions. Errors print `{"error":...}` and exit with status 1.

Every command can also be a [key binding](/docs/keybindings/#changing-them).

## Examples

Send every terminal to workspace 2:

```
gemwm msg windows | jq '.windows[] | select(.app_id=="foot") | .id' |
    xargs -I{} gemwm msg move-window {} 2
```

Print the focused window's title whenever it changes, for a status line:

```
gemwm msg subscribe | jq --unbuffered -r '
    select(.event == "windows") | .windows[] | select(.focused) | .title'
```
