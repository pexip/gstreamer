vpx_sources = [
  'gstvp8dec.c',
  'gstvp8enc.c',
  'gstvp8utils.c',
  'gstvp9dec.c',
  'gstvp9enc.c',
  'gstvpxdec.c',
  'gstvpxenc.c',
  'gstvpxelement.c',
  'plugin.c',
]

vpx_features = [
  [ 'vpx/vp8cx.h', 'vpx_codec_vp8_cx_algo', '-DHAVE_VP8_ENCODER', 'VP8 encoder' ],
  [ 'vpx/vp8dx.h', 'vpx_codec_vp8_dx_algo', '-DHAVE_VP8_DECODER', 'VP8 decoder' ],
  [ 'vpx/vp8cx.h', 'vpx_codec_vp9_cx_algo', '-DHAVE_VP9_ENCODER', 'VP9 encoder' ],
  [ 'vpx/vp8dx.h', 'vpx_codec_vp9_dx_algo', '-DHAVE_VP9_DECODER', 'VP9 decoder' ],
]

vpx_option = get_option('vpx')
vpx_dep = dependency('vpx-avx', version : '>=1.5.0', required : vpx_option)

if vpx_dep.found()
  vpx_args = ['-DHAVE_VPX_1_3']
  foreach f : vpx_features
    header = f.get(0)
    codec_iface = f.get(1)
    extra_define = f.get(2)
    link_code = '''#include <@0@>
                   int main (int a, char ** g) {
                     const vpx_codec_iface_t *c = &@1@;
                     return c != 0;
                   }'''.format(header,codec_iface)
    if cc.links(link_code, dependencies : vpx_dep)
      vpx_args += extra_define
      message('libvpx provides @0@ interface (@1@)'.format(f.get(3),f.get(1)))
      have_vpx_feature = true
    else
      message('libvpx does not provide @0@ interface (@1@)'.format(f.get(3),f.get(1)))
      have_vpx_feature = false
    endif
    set_variable('have_' + f.get(3).to_lower().underscorify(), have_vpx_feature)
  endforeach

  if vpx_args.length() == 0
    msg = 'libvpx was built without any encoder or decoder features!'
    # Error out if explicitly enabled, but continue with a warning if the
    # plugin is in auto-detect mode to reduce build-time friction.
    if vpx_option.enabled()
      error(msg)
    endif
    warning(msg)
  endif

  if vpx_dep.version().version_compare('>= 1.8.0')
    message('Found vpx >= 1.8.0')
    vpx_args += '-DHAVE_VPX_1_8'
  endif

  gnome = import('gnome')

  gstvpx_enums = gnome.mkenums_simple('gstvpx-enumtypes',
    sources : ['gstvpxenums.h'],
    decorator : 'G_GNUC_INTERNAL',
    install_header: false)

  gstvpx = library('gstvpx',
    vpx_sources, gstvpx_enums,
    c_args : gst_plugins_good_args + vpx_args,
    include_directories : [configinc],
    dependencies : [gstbase_dep, gsttag_dep, gstvideo_dep, vpx_dep],
    install : true,
    install_dir : plugins_install_dir,
  )
  pkgconfig.generate(gstvpx, install_dir : plugins_pkgconfig_install_dir)
  plugins += [gstvpx]

  install_data(sources: ['GstVP8Enc.prs'], install_dir: presetdir)
endif




#Pexip spesific

pex_isas = [
  ['avx',    ['-DGST_ISA_AVX'] ],
  ['avx2',   ['-DGST_ISA_AVX2'] ],
  ['avx512', ['-DGST_ISA_AVX512'] ],
]

vpx_args = [
  '-DHAVE_VPX_1_3',
  '-DHAVE_VPX_1_4',
  '-DHAVE_VP8_ENCODER',
  '-DHAVE_VP8_DECODER',
  '-DHAVE_VP9_ENCODER',
  '-DHAVE_VP9_DECODER',
]

plugins_install_dir_isa = '@0@-'.format(plugins_install_dir)

foreach isa_flags : pex_isas
  isa = isa_flags[0]
  flags = isa_flags[1]

  vpxisa_dep = dependency('vpx-@0@'.format(isa), required : false)

  if vpxisa_dep.found()
    gstvpxisa = shared_library('gstvpx_' + isa,
      vpx_sources, gstvpx_enums,
      c_args : gst_plugins_good_args + vpx_args + flags,
      include_directories: [configinc],
      dependencies : [gstbase_dep, gsttag_dep, gstvideo_dep, vpxisa_dep],
      install : true,
      install_dir : plugins_install_dir_isa + isa,
    )
  endif
endforeach
