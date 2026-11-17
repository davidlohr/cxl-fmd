# SPDX-License-Identifier: LGPL-2.1
# cxl-fmd's own registry and schema files: compiled into the daemon (see the top
# meson.build), and installed beside the DMTF schemas for static hosting

install_data(
    'registries/CxlHealth.1.0.0.json',
    install_dir: get_option('datadir') / 'cxl-fmd' / 'Registries',
)

install_data(
    'csdl/Cxl_v1.xml',
    install_dir: get_option('datadir') / 'cxl-fmd' / 'schema',
)
