gst_dcsctp_headers = [
  'gstsctpdec.h',
  'gstsctpenc.h',
  'sctpassociation.h',
]

gst_dcsctp_sources = [
  'dcsctpassociation_factory.c',
  'dcsctpassociation.c',
  'gstdcsctpdec.c',
  'gstdcsctpenc.c',
  'plugin.c'
]

doc_sources = []
foreach s: gst_dcsctp_sources + gst_dcsctp_headers
  doc_sources += meson.current_source_dir() / s
endforeach

plugin_sources += {
  'dcsctp': pathsep.join(doc_sources)
}

dcsctp_option = get_option('dcsctp').enable_if(get_option('webrtc').enabled(), error_message: 'webrtc option is enabled')
if dcsctp_option.disabled()
  subdir_done()
endif

subdir('lib')

gst_dcsctp_args = [
  '-Wall',
]

if host_system != 'windows'
  gst_dcsctp_args += ['-Werror']
endif

if not gst_debug_disabled
  gst_dcsctp_args += ['-DDCSCTP_DEBUG']
endif

gstdcsctp = library('gstdcsctp',
  gst_dcsctp_sources,
  c_args : gst_plugins_bad_args + gst_dcsctp_args,
  include_directories : [configinc],
  dependencies : [gst_dep, gstbase_dep, gstsctp_dep, dcsctp_dep],
  install : true,
  install_dir : plugins_install_dir,
)

plugins += [gstdcsctp]
