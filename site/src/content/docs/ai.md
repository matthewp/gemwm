---
title: "AI"
description: "GemWM's programs get AI from Augur, a service on the session bus, and it can be turned off for the whole session."
section: Going Further
order: 4
---

Some of GemWM's programs have AI features, like [GemMail](/apps/gemmail/)
sorting your mail into categories. They get it from
[Augur](https://github.com/matthewp/augur), a service any program on the
session bus can ask: the providers, keys and models are set up in Augur's own
config (`~/.config/augur/config`), not in each program.

With Augur installed and set up, programs show their AI features; they ask
Augur whether it's on. Without it, there's nothing to switch off: the features
simply aren't there. Nothing is sent anywhere until you turn a feature on in
the program that has it.

## Turning it off

To turn AI off for the whole session:

```
[ai]
enabled = false
```

`gemwm-session` reads this when you log in and tells Augur (by
`AUGUR_DISABLED` in the session's environment), so it says it's off to every
program that asks. It takes effect at the next login.
