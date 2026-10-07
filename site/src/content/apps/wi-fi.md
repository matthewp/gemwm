---
title: "Wi-Fi"
description: "A menu app for iwd, with the networks in range, and passwords asked in a GEM alert."
kind: utility
icon: wifi
command: "gemwm-wifi"
order: 3
---

`gemwm-wifi` is a menu app for [iwd](https://iwd.wiki.kernel.org/): signal
bars in the menu bar, greyed when Wi-Fi is off, with what it's connected to in
the tooltip.

A click opens a window listing the networks in range, strongest first, with
their signal, a lock on those with a password, and **Connect**,
**Disconnect** and **Forget** (for saved ones); turn Wi-Fi on and off, and
**Scan**. A new network's password is asked for in a GEM alert box (Ctrl+V
pastes).

Enterprise (802.1X) networks need setting up in iwd itself. Your user needs to
be allowed to talk to iwd: on Arch, being in `wheel` or `network` is enough.
