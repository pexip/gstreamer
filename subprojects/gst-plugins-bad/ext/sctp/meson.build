if get_option('sctp').disabled()
  subdir_done()
endif

subdir('dcsctp')

sctp_sources = [
  'sctpsocket.cc',
  'sctpassociation.c'
#  'dcsctp_transport.cc',
#  'gstsctpdec.c',
#  'gstsctpenc.c',
#  'gstsctpplugin.c',
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
