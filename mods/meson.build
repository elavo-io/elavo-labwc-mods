wayland_scanner_bin = find_program('wayland-scanner')

elavo_scanner_code = generator(
  wayland_scanner_bin,
  output: '@BASENAME@-protocol.c',
  arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

elavo_scanner_server = generator(
  wayland_scanner_bin,
  output: '@BASENAME@-protocol.h',
  arguments: ['server-header', '@INPUT@', '@OUTPUT@'],
)

elavo_xml = files('ext-background-effect-v1.xml')
elavo_proto_src = elavo_scanner_code.process(elavo_xml)
elavo_proto_headers = elavo_scanner_server.process(elavo_xml)

elavo_proto_lib = static_library(
  'elavo_mods_protos',
  elavo_proto_src,
  elavo_proto_headers,
  dependencies: [wayland_server],
)

labwc_deps += declare_dependency(
  link_with: elavo_proto_lib,
  sources: elavo_proto_headers,
  # So src/ssd/ssd-titlebar.c can include "elavo-mods/elavo-mods.h".
  include_directories: include_directories('..'),
)

labwc_sources += files(
  'corners.c',
  'clip-corners.c',
  'shadows.c',
  'theme-keys.c',
  'background-effect.c',
  'window-buttons.c',
)

# patches/scenefx defines `scenefx`. With it, blur, corners and shadows are
# SceneFX scene nodes; blur.c is the render-pass blur used without it.
if not is_variable('scenefx')
  labwc_sources += files('blur.c')
endif
