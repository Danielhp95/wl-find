project('wl-find-helper', 'c', default_options: ['c_std=gnu11', 'warning_level=2'])
fs = import('fs')
add_project_arguments('-Wno-unused-parameter', language: 'c')

wayland_protocols = dependency('wayland-protocols').get_variable('pkgdatadir')
wlr_protocols = dependency('wlr-protocols').get_variable('pkgdatadir')
scanner = find_program('wayland-scanner', native: true)

protocols = []
foreach xml : [
  wayland_protocols / 'stable/xdg-shell/xdg-shell.xml', # layer-shell's popups refer to it
  wayland_protocols / 'stable/viewporter/viewporter.xml',
  wayland_protocols / 'unstable/xdg-output/xdg-output-unstable-v1.xml',
  wlr_protocols / 'unstable/wlr-layer-shell-unstable-v1.xml',
  wlr_protocols / 'unstable/wlr-virtual-pointer-unstable-v1.xml',
]
  name = fs.stem(xml)
  protocols += custom_target(name + '-code', input: xml, output: name + '-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  protocols += custom_target(name + '-header', input: xml, output: name + '-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
endforeach

executable('wl-find-helper', 'wl-find-helper.c', protocols,
  dependencies: [
    dependency('wayland-client'),
    dependency('libportal'),
    dependency('libei-1.0'),
    meson.get_compiler('c').find_library('m'),
  ],
  install: true,
)
