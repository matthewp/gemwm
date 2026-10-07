---
title: "Passphrases"
description: "pinentry-gem asks for your GnuPG passphrase in a GEM alert box."
kind: utility
icon: key
command: "pinentry-gem"
order: 9
---

`pass` and `gpg` ask for your key's passphrase through a pinentry, and
GnuPG's own want X or a terminal. A program run with neither (GemMail's
password command, say) then can't ask, and fails until you've unlocked the key
somewhere else. `pinentry-gem` asks in a GEM alert box instead, on Wayland.

Point GnuPG at it in `~/.gnupg/gpg-agent.conf`:

```
pinentry-program /usr/local/bin/pinentry-gem
```

then `gpgconf --kill gpg-agent` (it starts again when next needed). The first
program to need the key gets the box; after that, the agent's cache
(`default-cache-ttl`, in the same file) answers. With no Wayland display, over
SSH say, it hands over to `pinentry-curses`.
