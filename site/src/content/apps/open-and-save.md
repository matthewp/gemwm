---
title: "Open and Save"
description: "GEM's item selector, for GemWM's programs and, through the desktop portal, for Firefox, Chromium and Flatpak apps."
kind: utility
icon: selector
command: "gemwm-portal"
order: 2
---

GemWM's programs pick files in GEM's item selector: the directory line with
its pattern (edit it to go elsewhere, or to see other files), the folder's
list with a close box to go up, and the selection beside it. Double-click a
folder to go in; Return chooses.

Other programs get it too, when they ask the desktop portal for their Open and
Save dialogs, as Firefox, Chromium and Flatpak apps do: `gemwm-portal` is
xdg-desktop-portal's file chooser on GemWM, framed as GemWM frames everything,
and centred over the program that asked. The rest of the portal (settings,
screenshots...) is still GTK's.

## Setting up

Installing GemWM installs it: `gemwm.portal` and `gemwm-portals.conf` go in
`/usr/share/xdg-desktop-portal`, where xdg-desktop-portal looks (the
`portal_dir` build option says where else). Log in again for the portal to
pick it up.

One file is chosen at a time, even where several may be, as GEM's selector
chose one.
