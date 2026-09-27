project(
	'gemwm',
	'c',
	version: '0.1.0',
	license: 'MIT',
	meson_version: '>=1.0.0',
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
libdrm = dependency('libdrm').partial_dependency(compile_args: true, includes: true)

wayland_scanner = find_program(
	dependency('wayland-scanner', native: true).get_variable('wayland_scanner'),
	native: true,
)
wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

xdg_shell_xml = wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml'
viewporter_xml = wl_protocol_dir / 'stable/viewporter/viewporter.xml'
layer_shell_xml = 'protocols/wlr-layer-shell-unstable-v1.xml'

# Headers for the compositor.
server_protocols = []
foreach p : [xdg_shell_xml, layer_shell_xml]
	server_protocols += custom_target(
		p.underscorify() + '_server_h',
		input: p,
		output: '@BASENAME@-protocol.h',
		command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
	)
endforeach

# Headers and glue code for clients (the menu bar).
client_protocols = []
foreach p : [xdg_shell_xml, viewporter_xml, layer_shell_xml]
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
		'src/ipc.c',
		'src/config.c',
		'src/frame.c',
		'src/desktop.c',
		'src/cairo_buffer.c',
		server_protocols,
	],
	dependencies: [wlroots, wayland_server, xkbcommon, cairo, libdrm],
	install: true,
)

executable(
	'gemwm-menu',
	['menu/menu.c', client_protocols],
	dependencies: [wayland_client, cairo],
	install: true,
)

install_data('session/gemwm-session', install_dir: get_option('bindir'),
	install_mode: 'rwxr-xr-x')
configure_file(
	input: 'session/gemwm.desktop.in',
	output: 'gemwm.desktop',
	configuration: {'bindir': get_option('prefix') / get_option('bindir')},
	install_dir: get_option('wayland_sessions_dir'),
)

# GemWeb, the browser. Optional: needs webkitgtk-6.0.
webkit = dependency('webkitgtk-6.0', required: false)
if webkit.found()
	executable(
		'gemweb',
		'browser/gemweb.c',
		dependencies: [webkit, dependency('gtk4'), cairo],
		install: true,
	)
	install_data('browser/gemweb.desktop',
		install_dir: get_option('datadir') / 'applications')
endif
