dep_wayland_client = dependency('wayland-client', version: '>= 1.17.0')

clients = [
{
	'basename': 'agl-screenshooter',
	'sources': [
	  'screenshooter.c',
	  '../shared/file-util.c',
	  '../shared/os-compatibility.c',
	  '../shared/xalloc.c',
	  agl_screenshooter_client_protocol_h,
	  agl_screenshooter_protocol_c,
	  xdg_output_unstable_v1_client_protocol_h,
	  xdg_output_unstable_v1_protocol_c,
	],
	'deps_objs' : [ dep_wayland_client ],
	'deps': [ 'cairo' ],
},
]

foreach t: clients
  t_name = t.get('basename')
  t_deps = t.get('deps_objs', [])

  foreach xdep: t.get('deps', [])
    dep = dependency(xdep, required: false)
    if dep.found()
      t_deps += dep
    endif
  endforeach

  executable(
      t_name, t.get('sources'),
      include_directories: [ common_inc ],
      dependencies: [ t_deps, libweston_dep ],
      install: true,
  )

  message('Building client ' + t_name)
endforeach
