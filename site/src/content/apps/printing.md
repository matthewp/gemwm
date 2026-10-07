---
title: "Printing"
description: "GEM's print dialog for GemWM's programs, and a menu app for whatever's printing."
kind: utility
icon: printer
command: "gemwm-printing"
order: 6
---

GemWM's own programs print through a GEM print dialog: pick the printer, the
copies and the pages (and the program's own options, like the crossword's
answers), or **PDF file** to save it in your Documents folder. Return prints,
Escape cancels, and the arrow keys go through the printers. Printers come from
CUPS, and the job goes straight to it.

## What's printing

`gemwm-printing` is a menu app for whatever's printing, from any program.
While a job is printing, a printer shows in the bar, with the job in its
tooltip; if the printer needs you (out of paper, a jam, paused), that's shown
in inverse beside it. After the last job it stays half a minute, saying how it
went, then goes.

A click (or **Desk > Tools > Printing**, any time) opens a window: what's
printing, with **Cancel**, what finished lately, and how each printer is. It
listens for CUPS's own signals, and asks CUPS every couple of seconds only
while something's printing. It's built when GTK 4 and libcups are there.
