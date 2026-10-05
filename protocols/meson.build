wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')
wayland_scanner = find_program('wayland-scanner')

wayland_scanner_code = generator(
	wayland_scanner,
	output: '@BASENAME@-protocol.c',
	arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

wayland_scanner_server = generator(
	wayland_scanner,
	output: '@BASENAME@-protocol.h',
	arguments: ['server-header', '@INPUT@', '@OUTPUT@'],
)

server_protocols = [
	'wlr-layer-shell-unstable-v1.xml',
	'wlr-output-power-management-unstable-v1.xml',
	'cosmic-toplevel-info-unstable-v1.xml',
	'labwc-control-v1.xml',
	'cosmic-workspace-unstable-v1.xml',
	#
	# The cosmic toplevel info protocol is an extension of the foreign
	# toplevel list protocol and also references the cosmic workspace
	# protocol from its deprecated workspace_enter/workspace_leave events.
	# wayland-scanner needs the interface definitions of everything a
	# protocol references, so pull those in from wayland-protocols.
	#
	# Note that wlroots keeps its own (hidden) copies of these interface
	# definitions. Duplicating them is safe because interface types are
	# matched by name rather than by pointer (see wl_interface_equal()).
	#
	wl_protocol_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
	wl_protocol_dir / 'staging/ext-workspace/ext-workspace-v1.xml',
]

server_protos_src = []
server_protos_headers = []

foreach xml : server_protocols
	server_protos_src += wayland_scanner_code.process(xml)
	server_protos_headers += wayland_scanner_server.process(xml)
endforeach

lib_server_protos = static_library(
	'server_protos',
	server_protos_src + server_protos_headers,
	dependencies: [wayland_server]
)

server_protos = declare_dependency(
	link_with: lib_server_protos,
	sources: server_protos_headers,
)
