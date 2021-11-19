project('agl-compositor',
  'c','cpp',
  version: '0.0.19',
  default_options: [
    'warning_level=3',
    'c_std=gnu99',
  ],
  meson_version: '>= 0.47',
  license: 'MIT/Expat',
)

config_h = configuration_data()
agl_compositor_version = '0.0.19'
libweston_version = 'libweston-8'
pkgconfig = import('pkgconfig')
fs = import('fs')

cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')

add_project_arguments(
  cc.get_supported_arguments([
    '-Wno-unused-parameter',
    '-Wno-pedantic',
    '-Wextra',
    '-Werror'
  ]),
  language: 'c'
)

add_project_arguments([
    '-DPACKAGE_STRING="agl-compositor @0@"'.format(meson.project_version()),
    '-D_GNU_SOURCE',
    '-D_ALL_SOURCE',
  ],
  language: 'c'
)

optional_libc_funcs = [ 'memfd_create', 'strchrnul' ]
foreach func: optional_libc_funcs
    if cc.has_function(func)
        add_project_arguments('-DHAVE_@0@=1'.format(func.to_upper()), language: 'c')
    endif
endforeach

dep_libsystemd = dependency('libsystemd', required: false)
dep_libsmack = dependency('libsmack', required: false)
dep_scanner = dependency('wayland-scanner', native: true)
prog_scanner = find_program(dep_scanner.get_pkgconfig_variable('wayland_scanner'))
dep_wp = dependency('wayland-protocols', version: '>= 1.18')
dir_wp_base = dep_wp.get_pkgconfig_variable('pkgdatadir')

depnames = [
  'gstreamer-1.0', 'gstreamer-allocators-1.0',
  'gstreamer-app-1.0', 'gstreamer-video-1.0',
  'gobject-2.0', 'glib-2.0'
]

deps_remoting = []
foreach depname : depnames
  dep = dependency(depname, required: false)
  if not dep.found()
    message('Remoting requires @0@ which was not found. '.format(depname))
  endif
  deps_remoting += dep
endforeach


# the transmitter plug-in requires waltham but we don't have a cflags or libs
# for it so we add waltham depends here. Further more, the output is being
# handled by remoting plug-in
depnames_waltham = [
  'waltham', 'waltham-transmitter',
]

deps_waltham = []
foreach depname : depnames_waltham
  dep = dependency(depname, required: false)
  if dep.found()
    deps_waltham += dep
  else
    message('Waltham requires @0@ which was not found. '.format(depname))
  endif
endforeach

deps_waltham += deps_remoting

agl_shell_xml = files('protocol/agl-shell.xml')
agl_shell_desktop_xml = files('protocol/agl-shell-desktop.xml')
agl_screenshooter = files('protocol/agl-screenshooter.xml')
xdg_shell_xml = join_paths(dir_wp_base, 'stable', 'xdg-shell', 'xdg-shell.xml')

protocols = [
  { 'name': 'agl-shell', 'source': 'internal' },
  { 'name': 'agl-shell-desktop', 'source': 'internal' },
  { 'name': 'agl-screenshooter', 'source': 'internal' },
  { 'name': 'xdg-shell', 'source': 'wp-stable' },
  { 'name': 'xdg-output', 'source': 'unstable', 'version': 'v1' },
]

foreach proto: protocols
    proto_name = proto['name']
    if proto['source'] == 'internal'
        base_file = proto_name
	xml_path = join_paths('protocol', '@0@.xml'.format(base_file))
    elif proto['source'] == 'wp-stable'
        base_file = proto_name
	xml_path = join_paths(dir_wp_base, 'stable', proto_name, '@0@.xml'.format(base_file))
    else
        base_file = '@0@-unstable-@1@'.format(proto_name, proto['version'])
	xml_path = join_paths(dir_wp_base, 'unstable', proto_name, '@0@.xml'.format(base_file))
    endif

    foreach output_type: [ 'client-header', 'server-header', 'private-code' ]
	if output_type == 'client-header'
	    output_file = '@0@-client-protocol.h'.format(base_file)
	elif output_type == 'server-header'
	    output_file = '@0@-server-protocol.h'.format(base_file)
	else
	    output_file = '@0@-protocol.c'.format(base_file)
	    if dep_scanner.version().version_compare('< 1.14.91')
	        output_type = 'code'
	    endif
	endif

	var_name = output_file.underscorify()
	target = custom_target(
	    '@0@ @1@'.format(base_file, output_type),
	    command: [ prog_scanner, output_type, '@INPUT@', '@OUTPUT@' ],
	    input: xml_path,
	    output: output_file,
	)

        set_variable(var_name, target)
    endforeach
endforeach

# libweston pkg-config file already has 'libweston' as prefix but
# agl-compositor uses 'libweston' also. This makes use of the prefix path as to
# allow building and installing the compositor locally
prefix_path = get_option('prefix')
message('prefix_path ' + prefix_path)
if not prefix_path.contains('/usr')
  include_dir_str = join_paths(prefix_path, 'include')
  if not fs.is_dir(include_dir_str)
    error('Prefix path does not contain an include directory!')
  endif
  local_dep = declare_dependency(include_directories: include_dir_str)
else
  local_dep = []
endif

dir_data = join_paths(prefix_path, get_option('datadir'))
dir_data_agl_compositor = join_paths('agl-compositor', 'protocols')
dir_data_pc = join_paths(dir_data, 'pkgconfig')
libweston_dep = dependency(libweston_version)

deps_libweston = [
  dependency('wayland-server'),
  libweston_dep,
  dependency('libweston-desktop-8'),
  local_dep,
]


srcs_agl_compositor = [
	'src/compositor.c',
	'src/desktop.c',
	'src/layout.c',
	'src/policy.c',
	'src/shell.c',
	'src/screenshooter.c',
	'src/input.c',
	'shared/option-parser.c',
	'shared/os-compatibility.c',
	agl_shell_server_protocol_h,
	agl_shell_desktop_server_protocol_h,
	agl_screenshooter_server_protocol_h,
	agl_shell_protocol_c,
	agl_shell_desktop_protocol_c,
	agl_screenshooter_protocol_c,
	xdg_shell_protocol_c,
]

policy_to_install = get_option('policy-default')
if policy_to_install == 'auto' or policy_to_install == 'allow-all'
  srcs_agl_compositor += 'src/policy-default.c'
  message('Installing allow all policy')
elif policy_to_install == 'deny-all'
  srcs_agl_compositor += 'src/policy-deny.c'
  message('Installing deny all policy')
elif policy_to_install == 'rba'
  srcs_agl_compositor += ['src/policy-rba.c', 'src/rba_adapter.cpp']
  deps_libweston += dependency('librba')
  message('Installing rba policy')
endif

# From meson documentation:
# In order to look for headers in a specific directory you can use args :
# '-I/extra/include/dir, but this should only be used in exceptional cases for
# includes that can't be detected via pkg-config and passed via dependencies.
if libweston_dep.found()
  if not meson.is_cross_build()
    if not prefix_path.contains('/usr')
      dir_path_x11_backend = join_paths(prefix_path, 'include', libweston_version, 'libweston', 'backend-x11.h')
      dir_path_headless_backend = join_paths(prefix_path, 'include', libweston_version, 'libweston', 'backend-headless.h')
    else
      dir_path_x11_backend = join_paths(libweston_version, 'libweston', 'backend-x11.h')
      dir_path_x11_backend = join_paths(libweston_version, 'libweston', 'backend-headless.h')
    endif
  else
    message('Building with cross environment')
    dir_path_x11_backend = join_paths(libweston_version, 'libweston', 'backend-x11.h')
    dir_path_headless_backend = join_paths(libweston_version, 'libweston', 'backend-headless.h')
  endif

  # do the test
  if cc.has_header(dir_path_x11_backend)
    config_h.set('HAVE_BACKEND_X11', 1)
    message('Building with X11 backend')
  endif

  if cc.has_header(dir_path_headless_backend)
    config_h.set('HAVE_BACKEND_HEADLESS', 1)
    message('Building with headless backend')
  endif
endif

if dep_libsystemd.found()
  config_h.set('HAVE_SYSTEMD', 1)

  srcs_agl_compositor += 'src/systemd-notify.c'
  deps_libweston += dep_libsystemd

  message('Found systemd, enabling notify support')
endif

if deps_remoting.length() == depnames.length()
  config_h.set('HAVE_REMOTING', 1)
  message('Found remoting depends, enabling remoting')
endif

if deps_waltham.length() == depnames_waltham.length() + depnames.length()
  config_h.set('HAVE_WALTHAM', 1)
  message('Found waltham depends, enabling waltham')
endif

if dep_libsmack.found()
  config_h.set('HAVE_SMACK', 1)
  deps_libweston += dep_libsmack
endif

dir_module_agl_compositor = join_paths(join_paths(prefix_path, get_option('libdir')), 'agl-compositor')

libexec_compositor = shared_library(
        'exec_compositor',
        sources: srcs_agl_compositor,
        dependencies: deps_libweston,
        install_dir: dir_module_agl_compositor,
        install: true,
        version: '0.0.0',
        soversion: 0
)

dep_libexec_compositor = declare_dependency(
        link_with: libexec_compositor,
        include_directories: [ include_directories('.') ],
        dependencies: deps_libweston,
)

configure_file(output: 'config.h', configuration: config_h)

exe_agl_compositor = executable(
	'agl-compositor',
	'src/main.c',
	dependencies: dep_libexec_compositor,
	install_rpath: dir_module_agl_compositor,
	install: true
)

pkgconfig.generate(
        filebase: 'agl-compositor-@0@-protocols'.format(agl_compositor_version),
        name: 'agl-compositor private protocols',
        version: agl_compositor_version,
        description: 'agl-compositor protocol files',
        variables: [
                'datarootdir=' + join_paths('${prefix}', get_option('datadir')),
                'pkgdatadir=' + join_paths('${pc_sysrootdir}${datarootdir}', dir_data_agl_compositor)
        ],
        install_dir: dir_data_pc
)

install_data(
        [ agl_shell_xml, agl_shell_desktop_xml ],
        install_dir: join_paths(dir_data, dir_data_agl_compositor)
)

common_inc = [ include_directories('src'), include_directories('.') ]
subdir('clients')
