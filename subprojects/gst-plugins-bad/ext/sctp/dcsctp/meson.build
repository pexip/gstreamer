subdir('lib')

gst_dcsctp_sources = [
  'dcsctp_transport.cc',
#  'gstdcsctp.c',
#  'gstdcsctplugin.c',
]

gst_dcsctp_cargs = []
gst_dcsctp_cppargs = ['-std=c++17']
gst_dcsctp_linkargs = []

gst_dcsctp_lib = library('gstdcsctp',
  gst_dcsctp_sources,
  c_args: gst_dcsctp_cargs,
  cpp_args: gst_dcsctp_cppargs,
  link_args: gst_dcsctp_linkargs,
  dependencies: [dcsctp_dep],
  install: true,
  include_directories: [include_directories('.')])

pkg_name = 'gstreamer-dcsctp-1.0'
gst_libraries += [[pkg_name, {'lib': gst_dcsctp_lib}]]

pkgconfig.generate(gst_dcsctp_lib,
  libraries : [gst_dep],
  variables : pkgconfig_variables,
  subdirs : pkgconfig_subdirs,
  name : pkg_name,
  description : 'dcSCTP plugin',
)

gst_dcsctp_dep = declare_dependency(link_with: gst_dcsctp_lib,
  include_directories : include_directories('.'))

meson.override_dependency(pkg_name, gst_dcsctp_dep)
