if get_option('labnag').allowed()
  wayland_client = dependency('wayland-client')
  wayland_cursor = dependency('wayland-cursor')

  nag_sources = files(
    'labnag.c',
    'pool-buffer.c',
  )

  wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

  protocols = [
    wl_protocol_dir / 'stable/tablet/tablet-v2.xml',
    wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
    wl_protocol_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
    '../protocols/wlr-layer-shell-unstable-v1.xml',
  ]

  foreach xml : protocols
    nag_sources += custom_target(
      xml.underscorify() + '_c',
      input: xml,
      output: '@BASENAME@-protocol.c',
      command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    )
    nag_sources += custom_target(
      xml.underscorify() + '_client_h',
      input: xml,
      output: '@BASENAME@-client-protocol.h',
      command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
    )
  endforeach

  if host_machine.system() in ['freebsd', 'openbsd']
    # For signalfd()
    epoll_dep = dependency('epoll-shim')
  else
    epoll_dep = []
  endif

  executable(
    'labnag',
    nag_sources,
    dependencies: [
      cairo,
      pangocairo,
      glib,
      wayland_client,
      wayland_cursor,
      wlroots,
      server_protos,
      epoll_dep,
      xkbcommon,
    ],
    include_directories: [labwc_inc],
    install: true,
  )
endif

clients = files('lab-sensible-terminal')
install_data(clients, install_dir: get_option('bindir'))

#
# labwcctl
#
# Reads the toplevel list, state and geometry exposed via
# ext-foreign-toplevel-list-v1 and zcosmic-toplevel-info-v1, and sets geometry
# via labwc_control_v1.
#
# The cosmic protocols are vendored in protocols/, but wayland-scanner needs
# the interface definitions of the protocols they reference as well, hence
# the ext-* entries below.
#
ctl_wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

ctl_protocols = [
  ctl_wl_protocol_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
  ctl_wl_protocol_dir / 'staging/ext-workspace/ext-workspace-v1.xml',
  '../protocols/cosmic-workspace-unstable-v1.xml',
  '../protocols/cosmic-toplevel-info-unstable-v1.xml',
  '../protocols/labwc-control-v1.xml',
]

ctl_sources = files('labwcctl.c')

foreach xml : ctl_protocols
  ctl_sources += custom_target(
    'ctl_' + xml.underscorify() + '_c',
    input: xml,
    output: '@BASENAME@-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
  ctl_sources += custom_target(
    'ctl_' + xml.underscorify() + '_client_h',
    input: xml,
    output: '@BASENAME@-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
endforeach

executable(
  'labwcctl',
  ctl_sources,
  dependencies: [dependency('wayland-client')],
  include_directories: [labwc_inc],
  install: true,
)
