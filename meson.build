# SPDX-License-Identifier: LGPL-2.1
# CXL Fabric Manager Daemon

project(
    'cxl-fmd',
    'c',
    version: '0.1.0',
    license: 'LGPL-2.1',
    meson_version: '>= 0.60.0',
    default_options: [
        'c_std=gnu17',
        'warning_level=2',
        'werror=false',
    ]
)

cc = meson.get_compiler('c')

# Dependencies
# Force subproject to avoid system libcxlmi with incomplete headers
libcxlmi_proj = subproject('libcxlmi', default_options: ['libdbus=enabled'])
libcxlmi_dep = libcxlmi_proj.get_variable('libcxlmi_dep')

microhttpd_dep = dependency('libmicrohttpd', required: true)
jansson_dep = dependency('jansson', required: true)
threads_dep = dependency('threads', required: true)

# Configuration
conf = configuration_data()
conf.set_quoted('FMD_VERSION', meson.project_version())
conf.set_quoted('FMD_NAME', meson.project_name())
conf.set('FMD_DEFAULT_PORT', 8080)

configure_file(
    output: 'config.h',
    configuration: conf
)

# Install schemas (for static hosting by external web server if needed)
subdir('schema/dmtf')

# Include directories
inc = include_directories('include', '.')

# Source files
src = files(
    'src/main.c',
    'src/log.c',
    'src/discovery.c',
    'src/topology.c',
    'src/mock.c',
    'src/cci_redfish_map.c',
    'src/dcd.c',
    'src/redfish/server.c',
    'src/redfish/handlers.c',
    'src/redfish/dcd_handlers.c',
)

# Build executable
executable(
    'cxl-fmd',
    src,
    include_directories: inc,
    dependencies: [
        libcxlmi_dep,
        microhttpd_dep,
        jansson_dep,
        threads_dep,
    ],
    install: true,
)

# Tests
if get_option('tests').enabled() or (get_option('tests').auto() and not meson.is_subproject())
    subdir('tests')
endif

# Summary
summary({
    'Version': meson.project_version(),
    'libcxlmi': libcxlmi_dep.found(),
    'libmicrohttpd': microhttpd_dep.found(),
    'jansson': jansson_dep.found(),
}, section: 'Configuration')
