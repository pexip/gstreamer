
sctp_headers = [
  'gstsctpdec.h',
  'gstsctpenc.h',
  'sctpassociation.h',
]

doc_sources = []
foreach s: sctp_sources + sctp_headers
  doc_sources += meson.current_source_dir() / s
endforeach

plugin_sources += {
  'sctp': pathsep.join(doc_sources)
}

sctp_option = get_option('sctp').enable_if(get_option('webrtc').enabled(), error_message: 'webrtc option is enabled')
if sctp_option.disabled()
  subdir_done()
endif

subdir('dcsctp')

sctp_sources = [
  'sctpassociation_factory.c',
  'sctpassociation.c',
  'gstsctpdec.c',
  'gstsctpenc.c',
  'gstsctpplugin.c'
]

sctp_args = [
  '-Wall',
]

if host_system != 'windows'
  sctp_args += ['-Werror']
endif

if not gst_debug_disabled
  sctp_args += ['-DSCTP_DEBUG']
endif

gstsctp = library('gstsctp',
  sctp_sources,
  c_args : gst_plugins_bad_args + sctp_args,
  include_directories : [configinc],
  dependencies : [gst_dep, gstbase_dep, gstsctp_dep, dcsctp_dep],
  install : true,
  install_dir : plugins_install_dir,
)

plugins += [gstsctp]
