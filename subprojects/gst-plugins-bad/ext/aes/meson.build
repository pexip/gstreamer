aes_sources = [
  'gstaes.c',
  'gstaeshelper.c',
  'gstaesenc.c',
  'gstaesdec.c',
]

aes_cargs = []
openssl_dep = dependency('openssl', version : '>= 1.1.0', required : get_option('aes'))
if openssl_dep.found()
  aes_cargs += ['-DHAVE_OPENSSL']
else
  subdir_done()
endif

winsock2_dep = cc.find_library('ws2_32', required : false)
gst_aes_deps = [gstpbutils_dep, gstvideo_dep, openssl_dep, winsock2_dep, gio_dep, libm]

gstaes = library('gstaes',
  aes_sources,
  c_args : gst_plugins_bad_args + aes_cargs,
  link_args : noseh_link_args,
  include_directories : [configinc],
  dependencies : gst_aes_deps,
  install : true,
  install_dir : plugins_install_dir,
)
plugins += [gstaes]
aes_dep = declare_dependency(include_directories : include_directories('.'))
