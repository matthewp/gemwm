# GemWM

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
| Super+Q             | close the focused window                 |
| Super+Z             | maximize the focused window (toggle)     |
| Super+Tab           | cycle windows (Shift goes backwards); window mode |
| Alt+Tab             | the same                                 |
| Super+Arrows        | focus the neighbouring tile or column    |
| Super+Ctrl+Arrows   | scrolling: move the column / the window  |
| Super+[ / Super+]   | scrolling: consume or expel the window   |
| Super+R             | scrolling: column width ⅓ → ½ → ⅔        |
| Super+1..9, Alt+1..9 | switch workspace                        |
| Super+Shift+1..9, Alt+Shift+1..9 | move the focused window to a workspace |
| Alt+Escape          | quit                                     |

While Super is held, the focused window gets a thick pink border.

Environment:

- `GEMWM_DESKTOP`: unset for colour-ST green, `mono` for the ST
  high-resolution dither, or any `RRGGBB` colour.
- `GEMWM_FONT`: font for titles and menus (a fontconfig family name).

## Menu bar

Hover a title to drop its menu; click an item to run it. Click the title
(or anywhere outside) to close it again; a click outside never reaches the
window underneath, as on the ST. At the right are the menu apps (below):
Bluetooth, the battery and the clock. Click the clock to switch between
24-hour and 12-hour time. The battery shows a bolt on mains power, and its
charge turns inverted at 10% or less.

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
    Bluetooth = exec gemwm-bluetooth --menu-app
    Battery = exec gemwm-battery
    Clock = exec gemwm-clock          # --12h or --24h, --seconds

`gemwm-clock` and `gemwm-battery` also take their settings from `[clock]`
and `[battery]` in `~/.config/gemwm/config` (see Key bindings).

A menu app runs as long as the bar does, and talks over its stdin and
stdout:

- Each line it prints replaces its item: `text`, `icon<TAB>text`, or
  `icon<TAB>text<TAB>inverse` for white text on black (as the battery shows
  when it's low). An empty line hides the item.
- The icon is a PNG file, or an inline 1-bit picture, `bitmap:WxH:hex`:
  each row in hex, eight pixels to a byte, the first pixel in the top bit
  and 1 for black. Either way it's drawn 1:1, as it would be on the ST.
- A click on the item sends it `click 1` (`2` middle, `3` right).
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

Modifiers are `Super`, `Alt`, `Ctrl` and `Shift`; keys use xkb names
(`Q`, `Tab`, `Return`, `Page_Down`, `F1`, `1`...). An action is any
`gemwm msg` command (see below), plus `exec <command>` and `quit`.
Apply changes with `gemwm msg reload-config`; `[clock]` and `[battery]`
are read by the clock and battery menu apps when the menu bar starts them
(`pkill gemwm-menu; setsid gemwm-menu &`).

## Workspaces

The middle of the menu bar shows the workspaces, `1 | 2 | +`. Click a
number to switch, or `+` to add one. Workspaces are dynamic: an empty
workspace is removed when you leave it, and the ones after it move up a
number. The number keys, with or without Shift, accept one past the last
workspace, which creates a new one.

## Tiling

Options > Mode switches between **Window** (overlapping GEM windows),
**Tiling** and **Scrolling** (below), with a check mark on the current one. In tiling mode the first
window takes the left half and the rest stack down the right, `gap` pixels
apart; dialogs float on top. Super+Arrows move focus between tiles, and the
focused tile keeps the pink focus border,
while the others are greyed out with GEM's dotted "disabled" pattern (clicks
go straight through it). Super+Z (or a tile's fuller) makes the focused
tile fill the screen; the other tiles stay open but hidden until Super+Z
again, a new window, or moving focus brings the layout back. Tiles can't be
dragged or resized; switching back to window mode returns every window to
where it was.

## Scrolling

The third mode, after niri. Each workspace is a strip of columns that
scrolls sideways; two half-width columns fill the screen, and the view moves
just enough to keep the focused column in sight. A new window opens as a
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
    gemwm msg close-window focused
    gemwm msg cycle-windows next      # or prev, like Super+Tab
    gemwm msg mode scrolling          # window, tiling, toggle; alone: report
    gemwm msg focus-direction left    # right, up, down (tiling, scrolling)
    gemwm msg move-direction right    # scrolling: move column / window
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
