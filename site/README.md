# gemwm.org

GemWM's website, built with [Astro](https://astro.build) and made to look like
GemWM itself: the green desktop, the GEM menu bar, and pages as GEM windows,
in the Atari ST's 8x16 font (`public/fonts`, from `extras/fonts`, GPL-2.0+).

    pnpm install
    pnpm dev          # http://localhost:4321
    pnpm build        # into dist/
    pnpm run deploy   # build, then publish it with `cf deploy`

- `src/content/docs/`: the manual, a Markdown file a page; `section` and
  `order` place it in the Docs menu and the side list.
- `src/content/apps/`: a page per application or utility; `kind`, `icon`
  (from `src/components/Icon.astro`) and `screenshot`.
- `src/components/`: the menu bar, the window frame (its gadgets drawn as
  `src/frame.c` draws them), and the desktop icons.
- `public/screenshots/`: taken in a headless nested GemWM.

Most of the docs follow the repository's README; when a feature changes,
change both.
