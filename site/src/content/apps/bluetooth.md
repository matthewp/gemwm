---
title: "Bluetooth"
description: "A menu app for BlueZ: scan, pair, connect and forget devices, with codes in GEM alert boxes."
kind: utility
icon: bluetooth
command: "gemwm-bluetooth"
order: 4
---

`gemwm-bluetooth` is a menu app. The bar shows the Bluetooth rune, greyed when
Bluetooth is off, and the name of what's connected.

A click opens a small window: turn Bluetooth on or off, **Scan** for devices,
and **Pair**, **Connect**, **Disconnect** or **Forget** them. Pairing codes
are shown, or asked about, in GEM alert boxes. Its menus are in the bar while
it's in front: File, and On/Off at the top of Options.

It uses BlueZ (`bluetoothd`) over D-Bus and needs GTK 4 to build; without a
Bluetooth adapter, there's no item.
