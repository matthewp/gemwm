project(
	'gemwm',
	'c',
	version: '0.1.0',
	license: 'BSD-3-Clause',
	license_files: 'LICENSE',
	meson_version: '>=1.1.0',
	default_options: ['c_std=c11', 'warning_level=2', 'werror=false'],
)

add_project_arguments(
	'-DWLR_USE_UNSTABLE',
	'-D_POSIX_C_SOURCE=200809L',
	'-Wno-unused-parameter',
	language: 'c',
)

wlroots = dependency('wlroots-0.19')
wayland_server = dependency('wayland-server')
wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols')
xkbcommon = dependency('xkbcommon')
cairo = dependency('cairo')
gdk_pixbuf = dependency('gdk-pixbuf-2.0')
libm = meson.get_compiler('c').find_library('m')
libdrm = dependency('libdrm').partial_dependency(compile_args: true, includes: true)

wayland_scanner = find_program(
	dependency('wayland-scanner', native: true).get_variable('wayland_scanner'),
	native: true,
)
wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

xdg_shell_xml = wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml'
viewporter_xml = wl_protocol_dir / 'stable/viewporter/viewporter.xml'
layer_shell_xml = 'protocols/wlr-layer-shell-unstable-v1.xml'
scroll_xml = 'protocols/gemwm-scroll-v1.xml'
app_menu_xml = 'protocols/gemwm-app-menu-v1.xml'

# Headers for the compositor.
server_protocols = []
foreach p : [xdg_shell_xml, layer_shell_xml, scroll_xml, app_menu_xml]
	server_protocols += custom_target(
		p.underscorify() + '_server_h',
		input: p,
		output: '@BASENAME@-protocol.h',
		command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
	)
endforeach
# Our own protocols' glue code, for the compositor side.
foreach p : [scroll_xml, app_menu_xml]
	server_protocols += custom_target(
		p.underscorify() + '_server_c',
		input: p,
		output: '@BASENAME@-server-protocol.c',
		command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
	)
endforeach

# Headers and glue code for clients (the menu bar).
client_protocols = []
foreach p : [xdg_shell_xml, viewporter_xml, layer_shell_xml, scroll_xml,
		app_menu_xml]
	client_protocols += custom_target(
		p.underscorify() + '_client_h',
		input: p,
		output: '@BASENAME@-client-protocol.h',
		command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
	)
	client_protocols += custom_target(
		p.underscorify() + '_client_c',
		input: p,
		output: '@BASENAME@-protocol.c',
		command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
	)
endforeach

executable(
	'gemwm',
	[
		'src/main.c',
		'src/view.c',
		'src/layer.c',
		'src/workspace.c',
		'src/tile.c',
		'src/scroll.c',
		'src/scrollbar.c',
		'src/animate.c',
		'src/appmenu.c',
		'src/ipc.c',
		'src/config.c',
		'src/frame.c',
		'src/desktop.c',
		'src/dither.c',
		'src/cairo_buffer.c',
		server_protocols,
	],
	dependencies: [wlroots, wayland_server, xkbcommon, cairo, libdrm, gdk_pixbuf,
		libm],
	install: true,
)

executable(
	'gemwm-menu',
	['menu/menu.c', client_protocols],
	dependencies: [wayland_client, cairo],
	install: true,
)

install_data('session/gemwm-session', 'session/gemwm-screenshot',
	install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
configure_file(
	input: 'session/gemwm.desktop.in',
	output: 'gemwm.desktop',
	configuration: {'bindir': get_option('prefix') / get_option('bindir')},
	install_dir: get_option('wayland_sessions_dir'),
)

# The terminal: foot in an Atari ST theme, with the ST's own font. The font
# (extras/fonts) is GPL, from EmuTOS; it is data installed next to GemWM,
# not part of it, and keeps its own license.
configure_file(
	input: 'session/gemwm-terminal.in',
	output: 'gemwm-terminal',
	configuration: {'datadir': get_option('prefix') / get_option('datadir')},
	install_dir: get_option('bindir'),
	install_mode: 'rwxr-xr-x',
)
install_data('extras/foot/foot.ini',
	install_dir: get_option('datadir') / 'gemwm')
fontdir = get_option('datadir') / 'fonts' / 'gemwm'
install_data('extras/fonts/AtariST8x16.ttf', 'extras/fonts/COPYING',
	'extras/fonts/README.md', install_dir: fontdir)
# Let fontconfig see the font right away (not when packaging into DESTDIR).
meson.add_install_script('sh', '-c',
	'test -n "$DESTDIR" || fc-cache -f "$MESON_INSTALL_PREFIX/@0@" 2>/dev/null || true'.format(fontdir))

# GemWeb, the browser. Optional: needs webkitgtk-6.0.
webkit = dependency('webkitgtk-6.0', required: false)
if webkit.found()
	executable(
		'gemweb',
		['browser/gemweb.c', 'browser/cookies.c', 'browser/history.c',
			'browser/passwords.c', 'lib/app-menu.c', client_protocols],
		dependencies: [webkit, dependency('gtk4'), dependency('gtk4-wayland'),
			dependency('gstreamer-1.0'), dependency('sqlite3'),
			wayland_client, cairo],
		include_directories: 'lib',
		install: true,
	)
	install_data('browser/gemweb-bw', install_dir: get_option('bindir'),
		install_mode: 'rwxr-xr-x')
	install_data('browser/gemweb.desktop',
		install_dir: get_option('datadir') / 'applications')
endif

# The menu apps at the right of the bar (see menu/menu.c).
foreach app : ['gemwm-clock', 'gemwm-battery']
	executable(app, ['menu-apps' / app + '.c', 'menu-apps/menu-app.c'],
		dependencies: libm, install: true)
endforeach

# gemwm-bluetooth: the Bluetooth menu app and window. Optional: needs GTK 4.
# It talks to BlueZ over D-Bus, so there's nothing else to build against.
gtk4 = dependency('gtk4', required: false)
if gtk4.found()
	icondir = get_option('datadir') / 'gemwm' / 'icons'
	executable(
		'gemwm-bluetooth',
		['bluetooth/gemwm-bluetooth.c', 'lib/app-menu.c', 'lib/gem-draw.c',
			client_protocols],
		dependencies: [gtk4, dependency('gtk4-wayland'), dependency('gio-unix-2.0'),
			wayland_client, cairo],
		c_args: ['-DICON_DIR="@0@"'.format(get_option('prefix') / icondir)],
		include_directories: 'lib',
		install: true,
	)
	install_data('bluetooth/icons/bluetooth.png', 'bluetooth/icons/bluetooth-off.png',
		install_dir: icondir)
	# Named after the app IDs, so the menu bar finds their names.
	install_data('bluetooth/org.gemwm.Bluetooth.desktop',
		'wifi/org.gemwm.WiFi.desktop',
		install_dir: get_option('datadir') / 'applications')

	# gemwm-wifi: the Wi-Fi menu app and window. It talks to iwd over
	# D-Bus, so there's nothing else to build against.
	executable(
		'gemwm-wifi',
		['wifi/gemwm-wifi.c', 'lib/app-menu.c', 'lib/gem-draw.c',
			client_protocols],
		dependencies: [gtk4, dependency('gtk4-wayland'), dependency('gio-unix-2.0'),
			wayland_client, cairo],
		include_directories: 'lib',
		install: true,
	)

	# gemwm-crossword: crossword puzzles, after MPOS's. Also needs libsoup
	# (for downloading them), which WebKitGTK uses too.
	soup = dependency('libsoup-3.0', required: false)
	if soup.found()
		executable(
			'gemwm-crossword',
			['crossword/gemwm-crossword.c', 'crossword/puz.c',
				'crossword/print.c', 'lib/app-menu.c',
				'lib/gem-draw.c', 'lib/gem-print.c', client_protocols],
			dependencies: [gtk4, dependency('gtk4-wayland'),
				dependency('gtk4-unix-print'), soup, wayland_client, cairo],
			include_directories: 'lib',
			install: true,
		)
		install_data('crossword/org.gemwm.Crossword.desktop',
			install_dir: get_option('datadir') / 'applications')
	endif

	# gemwm-printing: the printing menu app and window. Also needs libcups,
	# which it asks about the jobs and printers.
	cups = dependency('cups', required: false)
	if cups.found()
		executable(
			'gemwm-printing',
			['printing/gemwm-printing.c', 'lib/app-menu.c', 'lib/gem-draw.c',
				client_protocols],
			dependencies: [gtk4, dependency('gtk4-wayland'),
				dependency('gio-unix-2.0'), cups, wayland_client, cairo],
			include_directories: 'lib',
			install: true,
		)
		install_data('printing/org.gemwm.Printing.desktop',
			install_dir: get_option('datadir') / 'applications')
	endif

	# gemwm-volume: the volume menu app, window and keys. Also needs
	# libpulse (PulseAudio, or PipeWire's PulseAudio server).
	pulse = dependency('libpulse-mainloop-glib', required: false)
	if pulse.found()
		executable(
			'gemwm-volume',
			['volume/gemwm-volume.c', 'lib/app-menu.c', 'lib/gem-draw.c',
				client_protocols],
			dependencies: [gtk4, dependency('gtk4-wayland'),
				dependency('gio-unix-2.0'), pulse, dependency('libpulse'),
				wayland_client, cairo],
			include_directories: 'lib',
			install: true,
		)
		install_data('volume/org.gemwm.Volume.desktop',
			install_dir: get_option('datadir') / 'applications')
	endif
endif
