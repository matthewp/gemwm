# Backgrounds

`sunburst.jpg` is GemWM's default desktop: rays in warm 70s and 80s colours
rising from a sun below the bottom edge, fading to cream. GemWM dithers it to
16 ST colours (`dither = st16`), so the fades become the ST's patterns of dots.
It's installed to `share/gemwm/backgrounds`.

It's drawn by `sunburst.py` (Python with Pillow), at any size:

    ./sunburst.py 3840 2160 sunburst.jpg

Made for GemWM, under GemWM's BSD license.
