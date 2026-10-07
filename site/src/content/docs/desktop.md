---
title: "Desktop and Looks"
short: "Desktop and Looks"
description: "The desktop's colour, picture and pattern, the font, the focus border, and animation."
section: Customizing
order: 1
---

GemWM's settings live in one file, `~/.config/gemwm/config`. You only write
what you change: everything else keeps its default. Apply changes with:

```
gemwm msg reload-config
```

## The desktop

Out of the box, the desktop is GemWM's sunburst, dithered as an ST would have
shown it: the picture behind this site. Choosing anything in `[desktop]`
replaces it. A colour or pattern of your own is shown plain, a picture of your
own isn't dithered unless you set `dither` (below), and `image =`, left empty,
is the colour ST's own plain green desktop.

```
[desktop]
color = green             # or a name below, mono, or #rrggbb
image = ~/Pictures/atari.png
image-mode = fill         # fit, center, tile or stretch
```

The named colours are ST palette entries, colours a real ST could show:
`green` (the colour desktop's own), `dark-green`, `blue`, `navy`, `cyan`,
`teal`, `amber`, `red`, `purple`, `grey`, `dark-grey`, `white` and `black`.
`mono` is the high-resolution ST's 50% dither.

An image (PNG, JPEG, WebP...) is drawn over the colour. `center` and `tile`
show it at one desktop pixel per image pixel, and pixel art blown up 2x or
more keeps its square pixels.

`GEMWM_DESKTOP` in the environment overrides the colour, for a session that
should look different.

## As an ST would show it

Turn on dithering, and the picture is redrawn as an ST would have shown it:

```
[desktop]
dither = st16             # st: the ST's 512 colours; st16: 16 of them,
                          # picked for the picture, as in low-res;
                          # atari16: a fixed, bold 16; off
dither-style = diffuse    # diffuse: a fine speckle; ordered: the
                          # regular crosshatch of 80s computer art
pixel-size = 2            # how big its pixels are, in desktop pixels
resolution = 640x400      # or: size them so the screen is about this
                          # many across and down (overrides pixel-size)
```

`atari16` with `resolution = 640x400` is the busy, colourful look of a photo
on an ST in medium resolution.

The picture is redrawn in big pixels, in those colours, with patterns of dots
for the shades between them. It's done once, when the desktop changes, so it
costs nothing while you work, and windows stay sharp.

## A fill pattern

One of GEM's fill patterns can go over the colour, in black:

```
[desktop]
pattern = dots            # lines, vertical-lines, crosshatch, diagonal,
                          # checkerboard, bricks; none
```

## The font

Titles, menus and GemWM's own applications use the Atari ST's 8x16 system
font, at 16, the size it's drawn in whole pixels. If it's a bit much:

```
[font]
family = monospace        # any fontconfig family
size = 14                 # 14 unless it's the ST font, which is 16
```

Window titles change on `gemwm msg reload-config`; programs already running
(the menu bar, GemWeb...) keep their font until they're started again
(`pkill gemwm-menu; setsid gemwm-menu &` for the bar). The
[terminal](/apps/terminal/)'s font is set in its theme.

## The focus border

While Super is held, the focused window gets a thick border, so you can see
where your keys will go:

```
[highlight]
color = #ff3fa4           # the border shown while Super is held
width = 4                 # 0 turns it off
tiling = super            # or always: keep it on the focused tile
dim = tiling              # grey out unfocused windows: always, off
dim-opacity = 0.5         # 0..1, how strong the grey dots are
```

Dimming covers unfocused windows in GEM's dotted "disabled" pattern; clicks go
straight through it.

## Animation

In tiling and scrolling modes, changes to the layout are animated: a new
window, a swap or a push, a window closing and the others closing up, Super+Z,
and in scrolling mode the strip moving to the focused column.

```
[animation]
mode = slide              # or outline, or off
duration = 200            # ms, how long each takes
```

- **slide** (the default): windows glide to their new places, quick to start
  and gentle to stop, and grow or shrink to their new sizes (the frame moves
  out or in, showing as much of the window as fits: nothing is stretched). A
  new window fades in, rising into its place.
- **outline**: the way GEM did it. Windows don't move: a window the layout
  moves or resizes is hidden while its outline steps from its old place to its
  new one, then it appears there, and a new window's outline grows from the
  middle of its place, as GEM opened windows from their icons. Scrolling mode
  still slides its strip.
- **off**: everything jumps straight to its place.

Window mode isn't animated: its windows only move when you drag them, and
dragging already shows an outline.
