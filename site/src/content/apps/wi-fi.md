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

## Hotel and café networks

Networks that want you to sign in on a web page first are noticed: once
connected, `gemwm-wifi` asks GNOME's connectivity check
(`nmcheck.gnome.org`, the same one NetworkManager uses) whether the internet
answers. If the network answers in its place, the menu bar shows **Sign In**;
click it, or **Sign In...** in the window, and the network's page opens in a
private GemWeb window, so its cookies stay out of your own. Once you've signed
in, the notice goes by itself. If nothing answers at all, the tooltip and
the window say so. The check runs again every 10 minutes, because hotel
sign-ins expire.

Enterprise (802.1X) networks need setting up in iwd itself. Your user needs to
be allowed to talk to iwd: on Arch, being in `wheel` or `network` is enough.
