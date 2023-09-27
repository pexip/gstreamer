if get_option('sctp').disabled()
  subdir_done()
endif

subdir('dcsctp')

sctp_sources = [
#  'dcsctp_transport.cc',
#  'gstsctpdec.c',
#  'gstsctpenc.c',
#  'gstsctpplugin.c',
  'sctpassociation.c'
]

sctp_platform_deps = []

if not gst_debug_disabled
  sctp_args = ['-DSCTP_DEBUG']
else
  sctp_args = []
endif

gstsctp = library('gstsctp',
  sctp_sources,
  c_args : gst_plugins_bad_args + sctp_args,
  cpp_args: ['-std=c++17'],
  include_directories : [configinc],
  dependencies : [gst_dep, gstbase_dep, gstsctp_dep, dcsctp_dep],
  install : true,
  install_dir : plugins_install_dir,
)

plugins += [gstsctp]

#pkg_name = 'gstreamer-dcsctp-1.0'
#gst_libraries += [[pkg_name, {'lib': gst_dcsctp_lib}]]

#pkgconfig.generate(gst_dcsctp_lib,
#  libraries : [gst_dep],
#  variables : pkgconfig_variables,
#  subdirs : pkgconfig_subdirs,
#  name : pkg_name,
#  description : 'dcSCTP plugin',
#)

#gst_dcsctp_dep = declare_dependency(link_with: gst_dcsctp_lib,
#  include_directories : include_directories('.'))

#meson.override_dependency(pkg_name, gst_dcsctp_dep)
