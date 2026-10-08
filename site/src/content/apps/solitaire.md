---
title: "Solitaire"
description: "Klondike, drawn as GEM would have: drag cards in outline, double-click to send them home, and the cascade when you win."
kind: app
icon: cards
command: "gemwm-solitaire"
screenshot: "/screenshots/solitaire.png"
alt: "Solitaire: the stock, waste and foundations above seven columns of cards on dark green"
order: 6
---

**Desk > Games > Solitaire**: Klondike, the patience game of every desktop,
drawn as GEM would have drawn it. White cards with black outlines on the ST
palette's dark green, hearts and diamonds in red, and the court cards' letters
in the ST's own font, five times over.

## Playing

- Drag cards as GEM moved windows: an outline follows the pointer, and the
  cards go where you let go, if they can.
- Double-click a card to send it home to its foundation.
- Click the stock, or press Space, to draw; click its empty place to turn the
  waste back over.
- A card a move uncovers turns over by itself, and once every card is face up,
  the rest go home on their own. Winning ends in the cascade.

## Menus

**Game** has New Game (^N or F2; a game under way counts as lost, so it asks
first), Undo (^Z, as far back as you like) and Statistics..., and **Options**
has Draw One and Draw Three. The info line shows your moves and the time.

The option and the statistics (games played and won, the winning streak and
the longest) are kept in `~/.local/state/gemwm/solitaire`.
