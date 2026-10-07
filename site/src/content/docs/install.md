---
title: "Install"
description: "Build GemWM, install it as a login session, or try it in a window first."
section: Getting Started
order: 1
---

GemWM is built from source, with Meson. It runs on Linux, on
[wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) 0.19.

## What it needs

The compositor itself needs only the basics of a Wayland desktop:

- wlroots 0.19, Wayland and wayland-protocols
- xkbcommon, cairo, gdk-pixbuf and libdrm
- Meson and Ninja, to build

It needs 0.19 exactly: wlroots changes its API between versions, and 0.20 won't
do. On Arch Linux, where 0.20 has replaced it, `wlroots0.19` is in the AUR:

```
sudo pacman -S meson ninja wayland wayland-protocols libxkbcommon cairo gdk-pixbuf2 libdrm
paru -S wlroots0.19        # or yay, or makepkg
```

Everything else is optional. Each of GemWM's programs is built when what it
needs is installed, and left out when it isn't:

| Program | Needs |
|---|---|
| The menu bar, Bluetooth, notifications, the Open and Save dialogs | GTK 4, gtk4-layer-shell, json-glib |
| [GemWeb](/apps/gemweb/) | webkitgtk-6.0 (and GStreamer's plugins, for video and sound) |
| [GemMail](/apps/gemmail/) | libetpan, GMime 3, webkitgtk-6.0 |
| [GemWrite](/apps/gemwrite/) | GTK 4, libxml2, libarchive, enchant |
| [Crossword](/apps/crossword/) | GTK 4, libsoup 3 |
| [Printing](/apps/printing/) | GTK 4, libcups |
| [Volume](/apps/volume/) | GTK 4, libpulse |

## Build

```
git clone https://github.com/matthewp/gemwm
cd gemwm
meson setup build
ninja -C build
```

## Try it in a window

You don't have to log out to see it. From any Wayland desktop, GemWM opens
nested, as a window, with a terminal in it:

```
./build/gemwm -s foot
```

The scale is picked from the screen's pixel density: a whole number, so
pixels stay crisp. `-S 2` forces pixel doubling.

## Install as a login session

```
sudo ninja -C build install
```

This installs under `/usr/local`: `gemwm`, the menu bar, the apps, the
`gemwm-session` launcher, and `/usr/share/wayland-sessions/gemwm.desktop`.
Choose **GemWM** in your display manager when you log in, or use GemWM's own
[login screen](/docs/login-screen/).

`gemwm-session` sources `~/.config/gemwm/env` at startup, for environment
variables you want in the whole session (`TERMINAL`, say), and the session's
log goes to `~/.local/state/gemwm.log`.

To update, pull, then build and install again. Changes to the compositor take
effect when you next log in; most apps pick them up when they're next started.

## Next

[Getting around](/docs/getting-around/) shows the menu bar, windows and the
keys you'll want first.
