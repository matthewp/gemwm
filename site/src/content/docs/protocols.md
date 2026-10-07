---
title: "For App Developers"
short: "For Developers"
description: "Give your application GEM menus in the menu bar and GEM scroll bars in its frame."
section: Going Further
order: 3
---

On the Atari ST, an application didn't draw its own menus or scroll bars: it
told GEM what they were, and GEM drew them. GemWM brings both back as Wayland
protocols, so any application can fit in. They're small, and both are in
the repository's [`protocols/`](https://github.com/matthewp/gemwm/tree/main/protocols)
folder.

## Menus in the menu bar

`gemwm-app-menu-v1` puts an application's menus in GemWM's menu bar, after
the window's own menu, while its window is in front, as GEM did. The client
describes them (`menu`, `item`, `separator`, and since version 2 `submenu` and
`end_submenu`), then `commit`s; items can be disabled or checked, and carry a
keyboard shortcut to show. When the user picks one, the client gets
`activate` with the item's id.

GemWM's GTK 4 applications use it through `lib/app-menu.c`, a small helper
that's easy to lift into other programs.

## GEM scroll bars

`gemwm-scroll-v1` gives a window real GEM scroll bars, drawn in its frame. The
client reports how its content is scrolled, an axis at a time (`set_axis`:
position, visible and total, in its own units; only the ratios matter), and
GemWM draws the bars. When the user drags a slider, clicks an arrow or pages
on the track, the client gets `scroll_to`, scrolls, and reports the result as
usual.

[GemWeb](/apps/gemweb/) is the example: a page's scrolling is reported by a
small script, and its own scroll bar is hidden.

## The rest is standard

GemWM speaks the usual protocols, so most applications need nothing special:
xdg-shell, layer-shell (for bars, launchers and the like), xdg-activation (to
bring a window forward), xdg-foreign, screencopy (for screenshots), virtual
keyboard and pointer, and more. Open and Save dialogs come from the desktop
portal: see [Open and Save](/apps/open-and-save/).
