---
title: "Writing Menu Apps"
short: "Menu Apps"
description: "Put your own item at the right of the menu bar, with any program that prints a line."
section: Going Further
order: 2
---

Everything at the right of the menu bar is a menu app: Wi-Fi, Bluetooth, the
volume, the battery and the clock. A menu app is any program that prints a
line, and a shell script is enough. Add yours to `[Menu Apps]` in
`~/.config/gemwm/menu` (see [the menu bar](/docs/menu-bar/#menu-apps)).

## How it talks

A menu app runs as long as the bar does, and talks over its stdin and stdout:

- Each line it prints replaces its item: `text`, `icon<TAB>text`, or
  `icon<TAB>text<TAB>inverse` for white text on black (as the battery shows
  when it's low). An empty line hides the item.
- A fourth field is a tooltip, shown under the item while the pointer rests on
  it: `icon<TAB>text<TAB>flags<TAB>tooltip`, where flags is `inverse` or
  empty.
- The icon is a PNG file, or an inline 1-bit picture, `bitmap:WxH:hex`: each
  row in hex, eight pixels to a byte, the first pixel in the top bit and 1 for
  black. Either way it's drawn 1:1, as it would be on the ST.
- A click on the item sends it `click 1` (`2` middle, `3` right), and each
  notch of the scroll wheel over it `scroll up` or `scroll down`.
- When its stdin closes, the bar has gone, and it should exit.

If an app exits after running at least 10 seconds, it's restarted; one that
dies right away is left alone (look in the session log,
`~/.local/state/gemwm.log`).

## An example

The load average, every 5 seconds; a click opens htop:

```
#!/bin/sh
while :; do cut -d' ' -f1 /proc/loadavg; sleep 5; done &
ticker=$!
while read -r click; do setsid foot -e htop & done
kill $ticker
```

Then, in `~/.config/gemwm/menu`, keeping the ones you want:

```
[Menu Apps]
Load = exec ~/bin/load-menu-app
Wi-Fi = exec gemwm-wifi --menu-app
Volume = exec gemwm-volume --menu-app
Battery = exec gemwm-battery
Clock = exec gemwm-clock
```

and restart the bar: `pkill gemwm-menu; setsid gemwm-menu &`.
