sctp_sources = [
  'sctpreceivemeta.c',
  'sctpsendmeta.c',
]

sctp_headers = [
  'sctpreceivemeta.h',
  'sctpsendmeta.h',
  'sctp-prelude.h',
]

install_headers(sctp_headers, subdir : 'gstreamer-1.0/gst/sctp')

libgstsctp = library('gstsctp-' + api_version,
  sctp_sources,
  c_args : gst_plugins_bad_args + ['-DBUILDING_GST_SCTP'],
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gstbase_dep],
)

pkgconfig.generate(libgstsctp,
  libraries : [gst_dep],
  variables : pkgconfig_variables,
  subdirs : pkgconfig_subdirs,
  name : 'gstreamer-sctp-1.0',
  description : 'SCTP helper functions',
)

sctp_gen_sources = []
if build_gir
  sctp_gir = gnome.generate_gir(libgstsctp,
    sources : sctp_sources + sctp_headers,
    namespace : 'GstSctp',
    nsversion : api_version,
    identifier_prefix : 'Gst',
    symbol_prefix : 'gst',
    export_packages : 'gstreamer-sctp-1.0',
    includes : ['Gst-1.0'],
    install : true,
    extra_args : gir_init_section + ['-DGST_USE_UNSTABLE_API'],
    dependencies : [gstbase_dep]
  )
  sctp_gen_sources += sctp_gir
endif

gstsctp_dep = declare_dependency(link_with : libgstsctp,
  include_directories : [libsinc],
  sources : sctp_gen_sources,
  dependencies : [gstbase_dep])

meson.override_dependency('gstreamer-sctp-1.0', gstsctp_dep)
