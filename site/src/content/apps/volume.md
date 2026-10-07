---
title: "Volume"
description: "The volume in the menu bar, with every output, input and application, each with a GEM slider."
kind: utility
icon: speaker
command: "gemwm-volume"
order: 5
---

`gemwm-volume` is a menu app: a speaker with the output's volume. Scroll over
it to turn it up and down, right-click to mute, and click for a window with
every output and input (the round buttons pick the default: your headphones,
when they connect) and every application playing, each with a GEM slider and
Mute.

The volume keys run `gemwm-volume up`, `down`, `mute` and `mic-mute`, in 5%
steps up to 100%, and the bar follows at once.

It uses PulseAudio, or PipeWire's PulseAudio server, and is built when GTK 4
and `libpulse` are there.
