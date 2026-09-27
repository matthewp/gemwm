# Atari ST 8x16 font

`AtariST8x16.ttf` is the Atari ST's 8x16 system font as a TrueType font:
each pixel of the original is a square outline, so it stays pixel-sharp at
any whole-number scale. GemWM installs it for its terminal theme.

## License

**This directory is not under GemWM's BSD license.** The glyphs come from
[EmuTOS](https://emutos.sourceforge.io/)'s `bios/fnt_st_8x16.c`, which is
GPL-2.0-or-later, so the font is too:

- `fnt_st_8x16.c` is that source, unchanged
  (Copyright (C) 2001-2021 The EmuTOS development team)
- `COPYING` is the GNU General Public License, version 2
- `AtariST8x16.ttf` is built from it by `st_font.py`

It covers ASCII and the ST's accented letters and symbols that match code
page 437 (0x80-0xAF); other characters fall back to other fonts.

## Rebuilding

    python3 -m venv venv && venv/bin/pip install fonttools
    venv/bin/python st_font.py fnt_st_8x16.c AtariST8x16.ttf
