---
title: "Locking and Idle"
description: "Lock the screen with Super+L, by itself when you've gone, and before the computer sleeps."
section: Customizing
order: 4
---

**Super+L**, or **Desk > Lock Screen**, locks the screen. Every window and the
menu bar are hidden, the desktop stays, and a GEM dialog asks for your
password, with a bar of its own across the top saying the screen is locked,
and the time.

Return or **Unlock** checks the password with PAM, as logging in does (the
`gemwm-lock` service, in `/etc/pam.d`, installed with GemWM). A wrong one is a
GEM alert, and anything else PAM says, a fingerprint reader asking for a
finger say, shows in the dialog. Nothing reaches the windows behind it: not
the keys, not the pointer, not the key bindings.

## By itself

- **When you've gone:** after `lock` minutes with no input it locks, and after
  `screen-off` minutes (or a minute after locking) the screens go off. Any key
  or the pointer brings them back. A program that asks, a video player say,
  holds both off while it plays.
- **Before the computer sleeps**, closing the lid say: GemWM has logind wait
  while it locks, so it wakes up locked. `loginctl lock-session` locks too.

```
[idle]
lock = 10              # minutes idle until it locks; 0 never
screen-off = 15        # minutes until the screens go off; 0 never
locker = gemwm-lock    # or another locker
```

Apply changes with `gemwm msg reload-config`.

## If something goes wrong

If the locker dies while the screen's locked, the screen stays locked, showing
only the desktop, and GemWM starts the locker again a moment later.

GemWM speaks the Wayland screen locking protocol (`ext-session-lock-v1`), so
other lockers work too (`locker = swaylock`), and the idle one
(`ext-idle-notify-v1`), for tools like swayidle. From a script:

```
gemwm msg lock          # lock now
gemwm msg lock-status   # {"locked":true,"shown":true}
```
