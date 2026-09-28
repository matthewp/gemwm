# GemWM

![GemWM: the GEM menu bar, a terminal and the Bluetooth window on the green desktop](docs/screenshot.png)

A Wayland compositor (wlroots 0.19, C) that looks like Atari TOS/GEM:
the green colour-ST desktop, a GEM menu bar, and black and white GEM
window frames.

GemWM is three programs (plus a terminal theme, below):

- `gemwm`: the compositor. Draws the desktop and window frames.
- `gemwm-menu`: the menu bar, a wlr-layer-shell client. `gemwm` starts
  it automatically (`-M` to skip).
- `gemweb`: a web browser with GEM chrome (optional, needs
  `webkitgtk-6.0`).

## Build

    meson setup build
    ninja -C build

## Install as a login session

    meson setup build          # installs under /usr/local by default
    ninja -C build
    sudo ninja -C build install

This installs `gemwm`, `gemwm-menu`, the `gemwm-session` launcher and
`/usr/share/wayland-sessions/gemwm.desktop`. Choose GemWM in your
display manager. The session log goes to `~/.local/state/gemwm.log`, and
`~/.config/gemwm/env` is sourced at startup for settings.

## Run

Nested inside your current Wayland session (opens as a window):

    ./build/gemwm -s foot

The scale is picked from the screen's pixel density (a whole number, so pixels
stay crisp); `-S 2` forces pixel doubling.

| Keys                | Action                                   |
|---------------------|------------------------------------------|
| Super+T, Alt+Return | open a terminal (`$TERMINAL`, or the GEM one) |
| Super+B             | open the default browser                 |
| Print               | screenshot of the whole screen           |
| Shift+Print         | screenshot of an area you drag out       |
| Alt+Print           | screenshot of the focused window         |
| Volume keys         | volume up, down, mute; mic mute          |
| Super+Q             | close the focused window                 |
| Super+Z             | maximize the focused window (toggle)     |
| Super+F             | fullscreen the focused window (toggle)   |
| Super+Tab           | cycle windows (Shift goes backwards); window mode |
| Alt+Tab             | the same                                 |
| Super+Arrows        | focus the neighbouring tile or column    |
| Super+Ctrl+Arrows   | tiling: swap with the window that way; scrolling: move the column / the window |
| Super+Shift+Left/Right | tiling: push the window into the other column |
| Super+[ / Super+]   | scrolling: consume or expel the window   |
| Super+R             | scrolling: column width ⅓ → ½ → ⅔        |
| Super+1..9, Alt+1..9 | switch workspace                        |
| Super+Shift+1..9, Alt+Shift+1..9 | move the focused window to a workspace |
| Alt+Escape          | quit                                     |

While Super is held, the focused window gets a thick pink border.

Screenshots (also Options > Print Screen) are taken by `gemwm-screenshot
[screen|area|window]`, which needs `grim`, and `slurp` for areas. They're
saved to `~/Pictures/Screenshots` (or `$GEMWM_SCREENSHOT_DIR`), copied to
the clipboard if `wl-copy` is installed, and announced by your notification
daemon if one is running.

Environment:

- `GEMWM_DESKTOP`: overrides the desktop colour from the config (see
  [Desktop](#desktop)).
- `GEMWM_FONT`, `GEMWM_FONT_SIZE`: the font, if `[font]` in the config
  doesn't set it (see [Font](#font)).

## Menu bar

Hover a title to drop its menu; click an item to run it. Click the title
(or anywhere outside) to close it again; a click outside never reaches the
window underneath, as on the ST. At the right are the menu apps (below):
Bluetooth, the battery and the clock. Click the clock to switch between
24-hour and 12-hour time. The battery shows a bolt on mains power, and its
charge turns inverted at 10% or less; rest the pointer on it for how long
until it's empty, or full while charging.

As in GEM, the menu bar belongs to what's in front. With a window focused,
it shows **Desk**, a menu named after the window's application (from its
`.desktop` file: Firefox, Foot...), and **Options**. That menu has Close
Window, Full Size (checked when it is), and Move to Workspace. GemWM's own
applications add their menus after it: GemWeb's File, View and Go, and
Bluetooth's File, with its On/Off merged into the top of Options. Any
application can do the same with the `gemwm-app-menu-v1` Wayland protocol
(`protocols/gemwm-app-menu-v1.xml`). With no window focused, the bar shows the desktop's menus: Desk, File, View and
Options. Logout, Restart and Shutdown are at the bottom of Desk, so they're
always there.

The menus are built in, and `~/.config/gemwm/menu` changes them. You only
write what you want to change: a section replaces the built-in menu with the
same name, and every other menu stays as it is.

    # Replace the Internet submenu under Desk
    [Desk > Internet]
    Web Browser = chromium
    Mail = foot -e aerc
    Chat >
    -
    Downloads = foot -e yazi ~/Downloads

    # A submenu inside a submenu
    [Desk > Internet > Chat]
    IRC = foot -e weechat

    # An empty section hides a menu
    [View]

- `[Title]` is a menu in the bar; a new title adds a menu
- `[Title > Sub]` is a submenu, added at the end of Title unless Title has a
  `Sub >` line saying where it goes
- `Label = command` runs the command with `/bin/sh -c`
- `Label` alone is a disabled (grey) item
- `-` is a separator

The defaults are the TOS 1.0 desktop menus, plus two working items under
Desk: **Internet > Web Browser** opens GemWeb (or your default browser if
GemWeb isn't installed), and **Tools > Terminal** opens `$TERMINAL` (foot
if unset). The menu bar reads
its config at startup; restart it with `pkill gemwm-menu; setsid gemwm-menu &`.

### Menu apps

Everything at the right of the bar is a menu app: a small program with an
item there. They're listed in the same file, under `[Menu Apps]`, left to
right. As with menus, your section replaces the built-in one (so list the
ones you want to keep), and an empty section removes them all. The
defaults:

    [Menu Apps]
    Wi-Fi = exec gemwm-wifi --menu-app
    Bluetooth = exec gemwm-bluetooth --menu-app
    Volume = exec gemwm-volume --menu-app
    Battery = exec gemwm-battery
    Clock = exec gemwm-clock          # --12h or --24h, --seconds

`gemwm-clock` and `gemwm-battery` also take their settings from `[clock]`
and `[battery]` in `~/.config/gemwm/config` (see Key bindings).

A menu app runs as long as the bar does, and talks over its stdin and
stdout:

- Each line it prints replaces its item: `text`, `icon<TAB>text`, or
  `icon<TAB>text<TAB>inverse` for white text on black (as the battery shows
  when it's low). An empty line hides the item.
- A fourth field is a tooltip, shown under the item while the pointer
  rests on it: `icon<TAB>text<TAB>flags<TAB>tooltip`, where flags is
  `inverse` or empty.
- The icon is a PNG file, or an inline 1-bit picture, `bitmap:WxH:hex`:
  each row in hex, eight pixels to a byte, the first pixel in the top bit
  and 1 for black. Either way it's drawn 1:1, as it would be on the ST.
- A click on the item sends it `click 1` (`2` middle, `3` right), and
  each notch of the scroll wheel over it `scroll up` or `scroll down`.
- When its stdin closes, the bar has gone, and it should exit.

If an app exits after running at least 10 seconds, it's restarted; one that
dies right away is left alone (look in the session log).

A shell script is enough:

    #!/bin/sh
    # The load average, every 5 seconds; a click opens htop.
    while :; do cut -d' ' -f1 /proc/loadavg; sleep 5; done &
    ticker=$!
    while read -r click; do setsid foot -e htop & done
    kill $ticker

## Bluetooth

`gemwm-bluetooth` is a menu app. The bar shows the Bluetooth
rune, greyed when Bluetooth is off, and the name of what's connected. A click
opens a small window: turn Bluetooth on or off, **Scan** for devices, and
**Pair**, **Connect**, **Disconnect** or **Forget** them. Pairing codes are
shown, or asked about, in GEM alert boxes. It uses BlueZ (`bluetoothd`)
over D-Bus and needs GTK 4 to build; without a Bluetooth adapter, there's
no item.

## Key bindings

Keys are set in `~/.config/gemwm/config`. You only list what you change:
each line adds or replaces one binding, and `none` removes one.

    [keys]
    Super+Q = none                       # unbind
    Super+Return = exec foot             # run a program
    Super+W = close-window focused
    Super+Page_Down = workspace next
    Super+Page_Up = workspace prev
    Ctrl+Alt+Delete = quit

    [highlight]
    color = #ff3fa4                      # the border shown while Super is held
    width = 4                            # 0 turns it off
    tiling = always                      # or super: in tiling mode, keep it on
    dim = tiling                         # grey out unfocused windows: always, off
    dim-opacity = 0.5                    # 0..1, how strong the grey dots are

    [clock]
    mode = 24h                           # 12h, or off; click the clock to switch
    seconds = no

    [battery]
    mode = on                            # or off; hidden anyway without a battery

    [layout]
    mode = window                        # tiling or scrolling: the starting mode
    gap = 8                              # pixels between tiles and columns
    column-width = 0.5                   # scrolling: new columns' share of the screen

    [desktop]                            # see Desktop
    color = green
    image =
    image-mode = fill
    dither = off
    dither-style = diffuse
    pixel-size = 2
    resolution = off
    pattern = none

    [animation]
    mode = slide                         # outline (GEM's moving boxes), off
    duration = 200                       # ms; see Animation

    [font]                               # see Font
    family = Atari ST 8x16
    size = 16

Modifiers are `Super`, `Alt`, `Ctrl` and `Shift`; keys use xkb names
(`Q`, `Tab`, `Return`, `Page_Down`, `F1`, `1`...). An action is any
`gemwm msg` command (see below), plus `exec <command>` and `quit`.
Apply changes with `gemwm msg reload-config`; `[clock]` and `[battery]`
are read by the clock and battery menu apps when the menu bar starts them
(`pkill gemwm-menu; setsid gemwm-menu &`).

## Wi-Fi

`gemwm-wifi` is a menu app for iwd: signal bars in the bar, greyed when
Wi-Fi is off, with what it's connected to in the tooltip. A click opens a
window listing the networks in range, strongest first, with their signal,
a lock on those with a password, and Connect, Disconnect and Forget (for
saved ones); turn Wi-Fi on and off, and Scan. A new network's password is
asked for in a GEM alert box (Ctrl+V pastes). Enterprise (802.1X) networks
need setting up in iwd itself. Your user needs to be allowed to talk to
iwd: on Arch, being in `wheel` or `network` is enough.

## Volume

`gemwm-volume` is a menu app too: a speaker with the output's volume.
Scroll over it to turn it up and down, right-click to mute, and click for a
window with every output and input (the round buttons pick the default,
e.g. your headphones when they connect) and every application playing,
each with a GEM slider and Mute. The volume keys run `gemwm-volume up`,
`down`, `mute` and `mic-mute`, in 5% steps up to 100%, and the bar follows
at once. It uses PulseAudio, or PipeWire's PulseAudio server, and is built
when GTK 4 and `libpulse` are there.

## Animation

In tiling and scrolling modes, changes to the layout are animated: a new
window, a swap or push, a window closing and the others closing up, Super+Z,
and in scrolling mode the strip moving to the focused column. There are two
styles:

    [animation]
    mode = slide              # or outline, or off
    duration = 200            # ms, how long each takes

- **slide** (the default): windows glide to their new places, quick to
  start and gentle to stop, and a new window fades in, rising into its
  place. Only positions glide; a window that changes size takes its new
  size when its application redraws.
- **outline**: the way GEM did it. Windows don't move: in tiling mode a
  window the layout moves or resizes is hidden while its outline (the one
  you drag windows by) steps from its old place to its new one, then it
  appears there, and a new window's outline grows from the middle of its
  place, as GEM opened windows from their icons. Scrolling mode still
  slides its strip, and new windows' outlines grow there too.
- **off**: everything jumps straight to its place.

Window mode isn't animated: its windows only move when you drag them, and
dragging already shows an outline.

## Crossword Puzzle

Desk > Games > Crossword Puzzle (`gemwm-crossword`, after MPOS's). The
library lists your puzzles by date with how far along each is; its
Download tab has the week's Wall Street Journal (Monday to Saturday) and
Universal (daily) puzzles, as Across Lite `.puz` files from the archive at
herbach.dnsalias.com. A puzzle opens between its Across and Down clues:
type to fill, arrows move (and turn), Space or clicking the current square
switches direction, Tab jumps to the next clue. Check Puzzle turns the
border green or red, and filling it in right turns it green by itself.
Progress is saved as you type. Puzzles and saves are in
`~/.local/share/gemwm/crossword/`; `gemwm-crossword FILE.puz` opens any
`.puz`. It needs GTK 4 and libsoup 3 to build.

## Desktop

The desktop is set in `[desktop]` in `~/.config/gemwm/config`:

    [desktop]
    color = green             # or a name below, mono, or #rrggbb
    image = ~/Pictures/atari.png
    image-mode = fill         # fit, center, tile or stretch

The named colours are ST palette entries, colours a real ST could show:
`green` (the colour desktop's own), `dark-green`, `blue`, `navy`, `cyan`,
`teal`, `amber`, `red`, `purple`, `grey`, `dark-grey`, `white` and `black`.
`mono` is the high-resolution ST's 50% dither. An image (PNG, JPEG, WebP...)
is drawn over the colour; `center` and `tile` show it at one desktop pixel
per image pixel, and pixel art blown up 2x or more keeps its square pixels.

To see the desktop as an ST would have shown it, turn on dithering:

    [desktop]
    dither = st16             # st: the ST's 512 colours; st16: 16 of them,
                              # picked for the picture, as in low-res;
                              # atari16: a fixed, bold 16; off
    dither-style = diffuse    # diffuse: a fine speckle; ordered: the
                              # regular crosshatch of 80s computer art
    pixel-size = 2            # how big its pixels are, in desktop pixels
    resolution = 640x400      # or: size them so the screen is about this
                              # many across and down (overrides pixel-size)

`atari16` with `resolution = 640x400` is the busy, colourful look of a
photo on an ST in medium resolution.

The picture is redrawn in big pixels, in those colours, with patterns of
dots for the shades between them. It's done once, when the desktop
changes, so it costs nothing while you work, and windows stay sharp.

A GEM fill pattern can go over the colour, in black:

    [desktop]
    pattern = dots            # lines, vertical-lines, crosshatch, diagonal,
                              # checkerboard, bricks; none

Changes apply with `gemwm msg reload-config`.

## Font

Titles, menus and GemWM's own applications use the Atari ST's 8x16 system
font, at 16, the size it's drawn in whole pixels. If it's a bit much:

    [font]
    family = monospace        # any fontconfig family
    size = 14                 # 14 unless it's the ST font, which is 16

Window titles change on `gemwm msg reload-config`; programs already
running (the menu bar, GemWeb, Bluetooth...) keep their font until they're
started again (`pkill gemwm-menu; setsid gemwm-menu &` for the bar). The
terminal's font is set separately, in its theme.

## Workspaces

The middle of the menu bar shows the workspaces, `1 | 2 | +`. Click a
number to switch, or `+` to add one. Workspaces are dynamic: an empty
workspace is removed when you leave it, and the ones after it move up a
number. The number keys, with or without Shift, accept one past the last
workspace, which creates a new one.

## Tiling

Options > Mode switches between **Window** (overlapping GEM windows),
**Tiling** and **Scrolling** (below), with a check mark on the current one. In tiling mode windows
share the screen in two columns, each stacking its windows top to bottom,
`gap` pixels apart; dialogs float on top. A new window opens where you're
working, as in i3 and sway: the second window starts the right column, and
after that a new window splits the focused window's column, just below it.
A column left empty gives the other the whole width. Super+Ctrl+Arrows swap
the focused window with its neighbour that way; Super+Shift+Left/Right push
it into the other column, below the window beside it (only if its own
column keeps another; from a single column, it starts a second).
Super+Arrows move focus between tiles, and the
focused tile keeps the pink focus border,
while the others are greyed out with GEM's dotted "disabled" pattern (clicks
go straight through it). Super+Z (or a tile's fuller) makes the focused
tile fill the screen; the other tiles stay open but hidden until Super+Z
again, a new window, or moving focus brings the layout back. Tiles can't be
dragged or resized; switching back to window mode returns every window to
where it was.

Changes to the layout are animated; see [Animation](#animation).

## Scrolling

The third mode, after niri. Each workspace is a strip of columns that
scrolls sideways; two half-width columns fill the screen, and the view moves
just enough to keep the focused column in sight, sliding there so you can
see where you went (see [Animation](#animation)). A new window opens as a
column to the right of the focused one.

- Super+Left/Right (or Super+mouse wheel) moves between columns,
  Super+Up/Down between the windows stacked in one.
- Super+[ and Super+] **consume or expel**: a window alone in its column
  joins the neighbouring column as a new row; a window sharing a column
  leaves it for a column of its own. Columns hold rows, nothing deeper.
- Super+Ctrl+Arrows move the focused column left/right, or the window
  up/down within its column.
- Super+R cycles the column between a third, a half and two thirds of the
  screen; Super+Z makes it full width and back.

## Scripting: `gemwm msg`

GemWM listens on a control socket (`$GEMWM_SOCKET`, set for everything it
starts). `gemwm msg` sends one command and prints the JSON reply:

    gemwm msg workspaces              # list workspaces and their windows
    gemwm msg windows                 # id, title, app_id, workspace, focused,
                                      # tiled, and the frame's x/y/width/height
    gemwm msg workspace 2             # also: new, next, prev
    gemwm msg move-window 12 3        # window id or "focused"; 3 or "new"
    gemwm msg focus-window 12         # switches to its workspace if needed
    gemwm msg maximize focused        # or an id; toggles, like Super+Z
    gemwm msg fullscreen focused      # or an id; toggles, like Super+F
    gemwm msg close-window focused
    gemwm msg cycle-windows next      # or prev, like Super+Tab
    gemwm msg mode scrolling          # window, tiling, toggle; alone: report
    gemwm msg focus-direction left    # right, up, down (tiling, scrolling)
    gemwm msg move-direction right    # scrolling: move column / window;
                                      # tiling: swap with that neighbour
    gemwm msg push-direction left     # tiling: into the other column
    gemwm msg consume-or-expel left   # scrolling
    gemwm msg column-width 0.33       # scrolling: or "cycle"
    gemwm msg exec foot               # run a program
    gemwm msg reload-config           # re-read ~/.config/gemwm/config
    gemwm msg quit
    gemwm msg subscribe               # stream workspaces/windows/mode events

Window ids never change while a window is open; workspace numbers are
positions. Errors print `{"error":...}` and exit with status 1. For example,
to send every terminal to workspace 2:

    gemwm msg windows | jq '.windows[] | select(.app_id=="foot") | .id' |
        xargs -I{} gemwm msg move-window {} 2

## Terminal

Super+T, Alt+Return and Desk > Tools > Terminal open `gemwm-terminal`:
[foot](https://codeberg.org/dnkl/foot) dressed as an Atari ST terminal, in
the ST's own 8x16 system font with the ST palette. There are two themes,
light (the mono monitor's black on white) and dark (reverse video):
Options > Terminal Theme switches every open terminal and remembers the
choice, and Ctrl+Shift+D flips a single window.

    gemwm-terminal                  # open one
    gemwm-terminal theme dark       # or light; what the menu does

It layers the theme over your own `~/.config/foot/foot.ini`, so your key
bindings and other settings still apply. To use another terminal instead,
set `TERMINAL` in `~/.config/gemwm/env`.

## GemWeb

A WebKit browser whose tabs, buttons and info line are drawn like GemWM's
windows. It's built when `webkitgtk-6.0` is installed.

    gemweb               # opens a new window
    gemweb URL...        # opens tabs in the last-used window

The tab bar appears once a window has two tabs; Ctrl+T opens the second.

To make it the system default browser, for links opened from other apps:
`xdg-settings set default-web-browser gemweb.desktop`. Desk > Internet >
Web Browser always opens GemWeb.

| Keys                        | Action                    |
|-----------------------------|---------------------------|
| Ctrl+T / Ctrl+W             | new tab / close tab       |
| Ctrl+L, F6                  | edit the address          |
| Ctrl+Tab, Ctrl+Shift+Tab    | next / previous tab       |
| Alt+Left / Alt+Right        | back / forward            |
| Ctrl+R, F5                  | reload                    |
| Ctrl+plus / minus / 0       | zoom                      |

Middle-click or Ctrl+click opens a link in a background tab; middle-click a
tab to close it. Text that isn't an address is searched with DuckDuckGo, or
with `$GEMWEB_SEARCH` (`%s` marks the query). Downloads go to
`~/Downloads`; cookies and site data live in `~/.local/share/gemweb`.

Under GemWM a page scrolls with the window frame's own scroll bars, and its
built-in scroll bar is hidden; elsewhere, pages get GEM-styled scroll bars.

## Windows

- Drag the title bar to move: an outline follows the pointer and the window
  moves on release, as in GEM.
- Sizer (bottom right) resizes the same way.
- Closer (top left) closes, fuller (top right) toggles full screen size.
- Fullscreen (a video, a browser's F11, a game, or Super+F) covers the
  whole screen, menu bar and all, without a frame. Focusing another window
  on that workspace brings it back out, into its old place.
- Scroll bars are real GEM ones, for applications that drive them through
  the `gemwm-scroll-v1` protocol (`protocols/gemwm-scroll-v1.xml`), as GemWeb
  does: drag the slider, click the arrows to step, or click the track to
  page. As in GEM, windows whose applications don't use it have no scroll
  bars, just a thin border and the sizer.
- Clicking a background window only brings it to the top.

## License

BSD 3-Clause, see [LICENSE](LICENSE), except for two pieces that keep
their own licenses:

- `protocols/wlr-layer-shell-unstable-v1.xml`, vendored from wlroots
  (license in the file)
- `extras/fonts/`, the Atari ST font, built from EmuTOS and so under the
  GPL, version 2 or later (see `extras/fonts/README.md`). It is installed
  as a font file for the terminal and is not part of the GemWM programs.
