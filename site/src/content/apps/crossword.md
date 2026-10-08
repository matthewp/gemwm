---
title: "Crossword Puzzle"
description: "The week's newspaper crosswords, solved on screen and printed the way a newspaper would."
kind: app
icon: crossword
command: "gemwm-crossword"
order: 5
---

**Desk > Games > Crossword Puzzle**, after MPOS's. The library lists your
puzzles by date with how far along each is; its Download tab has the week's
Wall Street Journal (Monday to Saturday) and Universal (daily) puzzles, as
Across Lite `.puz` files.

A puzzle opens between its Across and Down clues: type to fill, the arrows
move (and turn), Space or clicking the current square switches direction, and
Tab jumps to the next clue. **Check Puzzle** turns the border green or red,
and filling it in right turns it green by itself. Progress is saved as you
type.

Each puzzle in the library has a menu, the arrow at the start of its row (or
right-click the row), with **Open** and **Delete...**. Delete, or **File >
Delete Puzzle...** while it's open, deletes the puzzle and your letters, once
you've said so. A puzzle from the last week can be downloaded again.

**File > Print...** (Ctrl+P) prints the open puzzle the way a newspaper would:
the title and byline, the grid, and the clues in columns around it. The print
dialog has "Include my answers". To make a PDF without the dialog:

```
gemwm-crossword --pdf OUT.pdf [--blank] FILE.puz
```

`gemwm-crossword FILE.puz` opens any `.puz`. Puzzles and saves are in
`~/.local/share/gemwm/crossword/`. It needs GTK 4 and libsoup 3.
