---
title: "Clock and Battery"
description: "The clock and battery at the right of the menu bar."
kind: utility
icon: clock
command: "gemwm-clock, gemwm-battery"
order: 8
---

The clock and the battery are menu apps, at the far right of the bar.

Click the clock to switch between 24-hour and 12-hour time. The battery shows
a bolt on mains power, and its charge turns inverted at 10% or less; rest the
pointer on it for how long until it's empty, or full while charging. Without a
battery, there's no item.

In `~/.config/gemwm/config`:

```
[clock]
mode = 24h                # 12h, or off
seconds = no

[battery]
mode = on                 # or off
```

They're read when the menu bar starts them: `pkill gemwm-menu; setsid
gemwm-menu &`. Want something else in the bar? Any program that prints a line
can be a [menu app](/docs/menu-apps/).
